#include "connection_impl.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <exec/async_scope.hpp>
#include <exec/finally.hpp>
#include <stdexcept>
#include <string>
#include <ranges>

#include "asioice/agent_config.hpp"
#include "asioice/candidate.hpp"
#include "asioice/detail/binary.hpp"
#include "asioice/detail/detached_with_data.hpp"
#include "asioice/detail/ignore.hpp"
#include "asioice/detail/on_scope_empty.hpp"
#include "asioice/detail/stop_when.hpp"
#include "asioice/detail/string_utils.hpp"
#include "asioice/sctp_transport.hpp"
#include "media_track_impl.hpp"
#include "rtcp.hpp"
#include "asiortc/rtp.hpp"

#include "codecs/default_h264.hpp"
#include "codecs/default_opus.hpp"
#include "codecs/default_vpx.hpp"

namespace asiortc {

static std::string to_string(signaling_state_t s) {
    switch (s) {
    case signaling_state_t::stable:
        return "stable";
    case signaling_state_t::have_local_offer:
        return "have_local_offer";
    case signaling_state_t::have_remote_offer:
        return "have_remote_offer";
    case signaling_state_t::have_local_pranswer:
        return "have_local_pranswer";
    case signaling_state_t::have_remote_pranswer:
        return "have_remote_pranswer";
    case signaling_state_t::closed:
        return "closed";
    }
}

static std::string ice_ufrag_from(const session_description &sdp) {
    if (!sdp.ice_ufrag.empty())
        return sdp.ice_ufrag;
    if (!sdp.medias.empty())
        return sdp.medias[0].ice_ufrag;
    return {};
}

static std::string ice_pwd_from(const session_description &sdp) {
    if (!sdp.ice_pwd.empty())
        return sdp.ice_pwd;
    if (!sdp.medias.empty())
        return sdp.medias[0].ice_pwd;
    return {};
}

static std::string fingerprint_from(const session_description &sdp) {
    if (!sdp.fingerprint.empty())
        return sdp.fingerprint;
    if (!sdp.medias.empty())
        return sdp.medias[0].fingerprint;
    return {};
}

static std::string setup_from(const session_description &sdp) {
    if (!sdp.setup.empty())
        return sdp.setup;
    if (!sdp.medias.empty())
        return sdp.medias[0].setup;
    return {};
}

static std::vector<std::string>
candidates_from(const session_description &sdp) {
    std::vector<std::string> result = sdp.candidates;
    if (!sdp.medias.empty())
        result.insert(result.end(), sdp.medias[0].candidates.begin(),
                      sdp.medias[0].candidates.end());
    return result;
}

static bool ice_options_trickle_from(const session_description &sdp) {
    for (const auto &[name, value] : sdp.attributes)
        if (name == "ice-options" && value == "trickle")
            return true;
    for (const auto &m : sdp.medias)
        for (auto &[name, value] : m.attributes)
            if (name == "ice-options" && value == "trickle")
                return true;
    return false;
}

asiortc::task<void>
connection_impl::_sender_send_loop(std::shared_ptr<rtp_sender> sender,
                                   std::shared_ptr<srtp_transport_type> srtp) {
    asioice::utils::scope_guard on_exit([]() noexcept {
        ICE_IN_DEBUG { std::cout << "_sender_send_loop exited\n"; }
    });
    std::vector<uint8_t> enc_buf;

    auto track = sender->track();
    if (!track)
        co_return;
    while (!sender->stopped()) {
        auto frame = co_await track->recv();
        if (!frame || frame->data.empty())
            co_return;

        if (sender->_encoders.empty()) {
            auto transceiver = sender->transceiver();
            if (!transceiver || transceiver->codecs().empty())
                co_return;

            auto fmt = track->format();
            std::string preferred;
            if (is_encoded_format(fmt)) {
                if (fmt == media_format::vp8)
                    preferred = "VP8";
                else if (fmt == media_format::vp9)
                    preferred = "VP9";
                else if (fmt == media_format::h264)
                    preferred = "H264";
                else if (fmt == media_format::opus)
                    preferred = "opus";
            }

            auto create_encoders = [&](const sdp_codec &c) -> bool {
                auto it = this->_codec_registry.find(c.name);
                if (it == this->_codec_registry.end())
                    return false;
                sender->_pt = c.payload_type;
                const auto &encs = transceiver->send_encodings;
                size_t n = encs.empty() ? 1 : encs.size();
                sender->_encoders.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    codecs::encoder_params ep;
                    if (i < encs.size() && encs[i].max_bitrate)
                        ep.bitrate = static_cast<int>(*encs[i].max_bitrate);
                    sender->_encoders.push_back(it->second(ep));
                }
                return true;
            };

            if (!preferred.empty()) {
                for (const auto &c : transceiver->codecs()) {
                    if (c.name == preferred && create_encoders(c))
                        break;
                }
            }

            if (sender->_encoders.empty()) {
                for (const auto &c : transceiver->codecs()) {
                    if (create_encoders(c))
                        break;
                }
            }

            if (sender->_encoders.empty())
                co_return;
        }
        sender->_force_keyframe = false;

        for (size_t si = 0; si < sender->_streams.size(); ++si) {
            auto &stream = sender->_streams[si];
            auto &enc = sender->_encoders[si];
            auto [payloads, ts] =
                is_encoded_format(frame->format)
                    ? enc->pack(frame->data, frame->timestamp)
                    : enc->encode(*frame, sender->_force_keyframe);

            if (payloads.empty())
                continue;

            for (size_t i = 0; i < payloads.size(); ++i) {
                uint16_t twcc_seq = 0;
                std::vector<uint8_t> ext_data;
                auto tr = sender->transceiver();
                if (tr) {
                    const auto &extmaps =
                        tr->receiver()->parameters().header_extensions;
                    const auto ext_id = [&](std::string_view uri) -> int {
                        for (const auto &e : extmaps)
                            if (e.uri == uri)
                                return e.id;
                        return 0;
                    };

                    int mid_id = ext_id("urn:ietf:params:rtp-hdrext:sdes:mid");
                    if (mid_id > 0 && mid_id < 15) {
                        std::string mid = tr->mid();
                        ext_data.push_back(
                            static_cast<uint8_t>((mid_id << 4) | 0));
                        ext_data.push_back(
                            static_cast<uint8_t>(mid.empty() ? '0' : mid[0]));
                    }

                    // RID ext (1 byte for single-char rid)
                    int rid_id =
                        ext_id("urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id");
                    if (rid_id > 0 && rid_id < 15 && !stream->rid.empty()) {
                        ext_data.push_back(static_cast<uint8_t>(
                            (rid_id << 4) |
                            (static_cast<int>(stream->rid.size()) - 1)));
                        ext_data.insert(ext_data.end(), stream->rid.begin(),
                                        stream->rid.end());
                    }

                    if (frame->kind == media_kind::video) {
                        int abs_id = ext_id("http://www.webrtc.org/experiments/"
                                            "rtp-hdrext/abs-send-time");
                        if (abs_id > 0 && abs_id < 15) {
                            uint64_t ntp = sender->_streams[0]->ntp_timestamp;
                            uint32_t abs = static_cast<uint32_t>(
                                ntp ? (ntp >> 14) & 0xFFFFFF : 0);
                            ext_data.push_back(
                                static_cast<uint8_t>((abs_id << 4) | 2));
                            ext_data.push_back(
                                static_cast<uint8_t>((abs >> 16) & 0xFF));
                            ext_data.push_back(
                                static_cast<uint8_t>((abs >> 8) & 0xFF));
                            ext_data.push_back(
                                static_cast<uint8_t>(abs & 0xFF));
                        }
                    }

                    int tcc_id = ext_id("http://www.ietf.org/id/"
                                        "draft-holmer-rmcat-transport-wide-"
                                        "cc-extensions-01");
                    if (tcc_id > 0 && tcc_id < 15) {
                        twcc_seq = ++this->_transport_wide_seq;
                        ext_data.push_back(
                            static_cast<uint8_t>((tcc_id << 4) | 1));
                        ext_data.push_back(
                            static_cast<uint8_t>((twcc_seq >> 8) & 0xFF));
                        ext_data.push_back(
                            static_cast<uint8_t>(twcc_seq & 0xFF));
                    }

                    while (ext_data.size() % 4)
                        ext_data.push_back(0);
                }

                rtp::rtp_packet rtp_pkt;
                rtp_pkt.payload_type = sender->_pt;
                bool last = (i == payloads.size() - 1);
                rtp_pkt.marker = last;
                uint16_t seq = ++stream->seq;
                rtp_pkt.sequence_number = seq;
                rtp_pkt.timestamp = ts ? ts : frame->timestamp;
                rtp_pkt.ssrc = stream->ssrc;
                rtp_pkt.payload = payloads[i];
                if (!ext_data.empty()) {
                    rtp_pkt.extension = 1;
                    rtp_pkt.extension_profile = 0xBEDE;
                    rtp_pkt.extension_data = std::move(ext_data);
                }

                std::vector<uint8_t> rtp_buf(rtp_pkt.serialized_size());
                rtp_pkt.write_to(rtp_buf.data(), rtp_buf.size());

                stream->history[seq % rtp_stream::HISTORY_SIZE] =
                    rtp_stream::history_entry{.payload = payloads[i],
                                              .seq = seq,
                                              .timestamp =
                                                  ts ? ts : frame->timestamp};

                auto r = co_await srtp->send_rtp(rtp_buf);
                if (std::get<0>(r))
                    co_return;

                this->_tx_packets++;
                this->_tx_bytes += rtp_buf.size();
                if (twcc_seq) {
                    this->_twcc_sent[twcc_seq %
                                     connection_impl::TWCC_SENT_SIZE] =
                        connection_impl::_twcc_sent_entry{
                            .transport_seq = twcc_seq,
                            .ssrc = stream->ssrc,
                            .size = rtp_buf.size(),
                            .send_time = std::chrono::steady_clock::now()};
                }

                stream->octet_count +=
                    static_cast<uint32_t>(payloads[i].size());
                stream->packet_count++;
                auto now = std::chrono::high_resolution_clock::now();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                              now.time_since_epoch())
                              .count();
                stream->ntp_timestamp =
                    static_cast<uint64_t>(us) * 65536 / 1000000;
                stream->rtp_timestamp = ts ? ts : frame->timestamp;
            }
        }
    }
}

static asioice::agent_config get_agent_config(asiortc::configuration &&cfg) {
    asioice::agent_config ret{};
    ret.username = asioice::utils::random_string(8);
    ret.password = asioice::utils::random_string(28);
    ret.ice_controlling = false;
    ret.use_loopback = true;
    ret.transport_policy =
        cfg.ice_transport_policy == ice_transport_policy_t::all
            ? asioice::transport_policy::ALL
            : asioice::transport_policy::RELAY;
    ret.connectivity_check_timeout = std::chrono::milliseconds(60 * 1000);
    ret.enable_mdns = true;
    ret.ice_servers.urls = std::move(cfg.ice_servers.urls);
    ret.ice_servers.username = std::move(cfg.ice_servers.username);
    ret.ice_servers.password = std::move(cfg.ice_servers.password);
    return ret;
}

connection_impl::connection_impl(connection_impl::executor_type ex,
                                 asiortc::configuration cfg)
    : _executor{std::move(ex)}, _bundle_policy{cfg.bundle_policy},
      _agent{_executor, get_agent_config(std::move(cfg))}, _sync_sender{*this} {
    _register_default_codecs();
    _agent.on_local_candidates(
        std::bind_front(&connection_impl::do_on_candidates, this));
}

asiortc::task<void> connection_impl::apply_descriptions() {
    if (_roles_set || !_local_desc || !_remote_desc)
        co_return;
    _roles_set = true;

    bool remote_support_trickle_ice = ice_options_trickle_from(*_remote_desc);
    _agent.config().trickle_ice = remote_support_trickle_ice;

    auto ufrag = ice_ufrag_from(*_remote_desc);
    auto pwd = ice_pwd_from(*_remote_desc);
    if (!ufrag.empty())
        _agent.set_remote_username(std::move(ufrag));
    if (!pwd.empty())
        _agent.set_remote_password(std::move(pwd));

    _ice_transport = _agent.create_ice_transport(1);
    _dtls_transport =
        std::make_shared<dtls_transport_type>(_ice_transport, std::move(_cert));

    const auto fp_str = fingerprint_from(*_remote_desc);
    auto remote_fp = asioice::ssl::fingerprint::from_sdp(fp_str);
    if (remote_fp)
        _dtls_transport->set_expected_remote_fingerprint(std::move(*remote_fp));

    auto candidate_lines = candidates_from(*_remote_desc);
    if (candidate_lines.empty()) {
        if (!remote_support_trickle_ice)
            co_await _agent.add_remote_candidate();
        co_return;
    }
    exec::async_scope scope;
    for (const auto &line : candidate_lines) {
        auto c = asioice::candidate::from_sdp(line);
        if (c) {
            scope.spawn(_agent.add_remote_candidate(std::move(*c)) |
                        asioice::utils::ignore());
        }
    }
    co_await (asioice::utils::on_scope_empty(scope) |
              stdexec::continues_on(asioice::utils::scheduler{_executor}));
    if (!remote_support_trickle_ice)
        co_await _agent.add_remote_candidate();
}

void connection_impl::start_gathering() {
    _gathering_state = ice_gathering_state_t::gathering;
    _gathering_task = stdexec::spawn_future(
        stdexec::starts_on(
            stdexec::inline_scheduler{},
            exec::finally(
                this->_agent.gather_candidates() | stdexec::then([this] {
                    if (!_local_desc)
                        return;
                    for (const auto &c : this->_agent.local_candidates())
                        _local_desc->candidates.push_back(c.to_sdp());
                }),
                stdexec::just() | stdexec::then([this] {
                    // this->_gathering_task.reset();
                    if (_gathering_state == ice_gathering_state_t::gathering)
                        _gathering_state = ice_gathering_state_t::complete;
                }))),
        _scope.get_token());
}

void connection_impl::do_on_candidates(std::span<const asioice::candidate> cc) {
    if (_local_desc) {
        for (const auto &c : cc)
            _local_desc->candidates.push_back(c.to_sdp());
    }
    if (_on_candidates)
        _on_candidates(cc);
}

void connection_impl::start_connecting() {
    if (_local_desc && _remote_desc) {
        _connecting_task = stdexec::spawn_future(
            stdexec::starts_on(
                stdexec::inline_scheduler{},
                exec::finally(this->do_connect(),
                              stdexec::just() | stdexec::then([this] {
                                  //   this->_connecting_task.reset();
                              }))),
            _scope.get_token());
    }
}

// Strips codec-specific RTP framing before jitter buffer ingestion.
// VP8/VP9: removes payload descriptor.  H264: FU-A/STAP-A reassembly.
// Other codecs (Opus, G.711, etc.): pass through unchanged.
static std::vector<uint8_t> depayload(const std::string &codec_name,
                                      std::vector<uint8_t> payload) {
    if ((codec_name == "VP8" || codec_name == "VP9") && !payload.empty()) {
        // Parse VP8/VP9 payload descriptor to find bitstream offset
        const uint8_t *d = payload.data();
        size_t dlen = payload.size();
        uint8_t b0 = d[0];
        size_t pos = 1;
        if ((b0 >> 7) & 1) { // extended control
            if (dlen > 1) {
                uint8_t ext = d[1];
                pos = 2;
                if ((ext & 0x80) && dlen > pos) // picture_id
                    pos += (d[pos] & 0x80) ? 2 : 1;
                if ((ext & 0x40) && dlen > pos) // tl0picidx
                    ++pos;
                if ((ext & 0x30) && dlen > pos) // tid/keyidx
                    ++pos;
            }
        }
        if (pos <= dlen)
            payload.erase(payload.begin(), payload.begin() + pos);
    } else if (codec_name == "H264" && !payload.empty()) {
        // H264: nothing to strip at the packet level;
        // individual NAL units are self-contained.
        // FU-A/STAP-A reassembly is handled in H264DecoderImpl.
    }
    return payload;
}

static std::optional<uint32_t> parse_ssrc_value(std::string_view line) {
    auto sp = line.find(' ');
    auto s = (sp != std::string_view::npos) ? line.substr(0, sp) : line;
    if (s.empty())
        return std::nullopt;
    uint32_t v = 0;
    for (auto c : s) {
        if (c < '0' || c > '9')
            return std::nullopt;
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    return v;
}

void connection_impl::_rebuild_pt_maps() {
    _pt_codec_map.clear();
    _pt_receiver_map.clear();
    _mid_track_map.clear();
    _mid_ext_id = 0;

    for (const auto &t : _transceivers) {
        if (!t)
            continue;
        auto recv = t->receiver();
        auto track =
            recv ? std::static_pointer_cast<media_track_impl>(recv->track())
                 : nullptr;
        for (const auto &c : t->codecs()) {
            _pt_codec_map.emplace(c.payload_type, c);
            if (recv && track) {
                _pt_receiver_map.try_emplace(
                    c.payload_type, _pt_recv_entry{recv, track, c.name});
                if (c.name != "rtx" && track->_decoder_codec_name.empty())
                    track->_decoder_codec_name = c.name;
            }
        }

        if (!t->mid().empty() && track)
            _mid_track_map[t->mid()] = track;

        if (_mid_ext_id == 0 && recv) {
            for (const auto &e : recv->parameters().header_extensions) {
                if (e.uri == "urn:ietf:params:rtp-hdrext:sdes:mid") {
                    _mid_ext_id = e.id;
                    break;
                }
            }
        }
    }

    if (_remote_desc) {
        for (const auto &rm : _remote_desc->medias) {
            if (rm.mid.empty() || rm.ssrcs.empty())
                continue;
            auto mit = _mid_track_map.find(rm.mid);
            if (mit == _mid_track_map.end())
                continue;
            for (const auto &ssrc_line : rm.ssrcs) {
                auto ssrc = parse_ssrc_value(ssrc_line);
                if (ssrc)
                    _ssrc_track_map.try_emplace(*ssrc, mit->second);
            }
        }
    }
}

const sdp_codec *connection_impl::_find_codec(uint8_t pt) const {
    auto it = _pt_codec_map.find(pt);
    return it != _pt_codec_map.end() ? &it->second : nullptr;
}

asiortc::task<void> connection_impl::do_connect() {
    _connection_state = connection_state_t::connecting;
    asioice::utils::scope_guard on_exit([this]() noexcept {
        if (_connection_state == connection_state_t::connecting)
            _connection_state = connection_state_t::failed;
    });
    if (!this->_agent.config().trickle_ice) {
        while (_gathering_state != ice_gathering_state_t::complete)
            co_await (
                _gathering_state.on_change() |
                stdexec::continues_on(asioice::utils::scheduler{_executor}));
    }
    if (!co_await _agent.connect()) {
        ICE_IN_DEBUG { std::cerr << "ICE connect failed\n"; }
        co_return;
    }

    auto setup_str = setup_from(*_remote_desc);
    bool we_are_active = (setup_str != "active");
    auto dtls_role = we_are_active
                         ? dtls_transport_type::handshake_type::client
                         : dtls_transport_type::handshake_type::server;
    auto hs_ec = co_await _dtls_transport->async_handshake(dtls_role);
    if (hs_ec) {
        ICE_IN_DEBUG {
            std::cerr << "DTLS handshake failed: " << hs_ec.message() << '\n';
        }
        co_return;
    }

    bool needs_srtp = false;
    bool needs_sctp = _need_sctp;
    if (_remote_desc) {
        for (auto &m : _remote_desc->medias) {
            if (m.proto.find("SAVP") != std::string::npos)
                needs_srtp = true;
            if (m.media_type == "application")
                needs_sctp = true;
        }
    }

    if (needs_srtp) {
        auto keys = _dtls_transport->export_srtp_key_material();
        if (keys &&
            keys->profile != asioice::ssl::srtp_protection_profile::none) {
            _srtp_transport =
                std::make_shared<srtp_transport_type>(_ice_transport);
            auto id = reinterpret_cast<uintptr_t>(_srtp_transport.get());
            _transport_stats_id = "transport_" + std::to_string(id);
            auto srtp_role = we_are_active ? asioice::ssl::dtls_role::client
                                           : asioice::ssl::dtls_role::server;
            _srtp_transport->setup(*keys, srtp_role);
            _srtp_transport->on_new_ssrc(
                std::bind_front(&connection_impl::do_on_new_ssrc, this));
            _srtp_transport->on_rtp_rtcp_packet(
                std::bind_front(&connection_impl::do_on_rtp_rtcp_packet, this));
            _start_sender_loops();
            _start_nack_loop();

            for (const auto &t : _transceivers) {
                auto receiver = t->receiver();
                if (!receiver)
                    continue;
                receiver->_rtcp_loop = stdexec::spawn_future(
                    stdexec::starts_on(
                        stdexec::inline_scheduler{},
                        _receiver_rtcp_loop(receiver->shared_from_this(),
                                            _srtp_transport)),
                    _scope.get_token());
            }
        }
    }

    if (!needs_sctp) {
        _connection_state = connection_state_t::connected;
        co_return;
    }

    _sctp_transport = std::make_shared<sctp_transport_type>(_dtls_transport);
    _sctp_transport->start();

    bool sctp_ok = false;
    if (we_are_active) {
        sctp_ok = co_await _sctp_transport->accept();
    } else {
        sctp_ok = co_await _sctp_transport->connect();
    }
    if (!sctp_ok) {
        ICE_IN_DEBUG { std::cerr << "SCTP setup failed\n"; }
        co_return;
    }

    _data_channel_manager.emplace(_sctp_transport, we_are_active);
    _data_channel_manager->on_remote_channel(
        std::bind_front(&connection_impl::do_on_data_channel, this));
    _data_channel_manager->start();
    _connection_state = connection_state_t::connected;
}

asiortc::task<void>
connection_impl::set_local_description(session_description desc) {
    switch (_signaling_state.get()) {
    case signaling_state_t::have_local_offer:
    case signaling_state_t::have_local_pranswer:
    case signaling_state_t::have_remote_pranswer:
        throw std::logic_error{"set_local_description: invalid state: " +
                               to_string(_signaling_state.get())};
    case signaling_state_t::stable:
    case signaling_state_t::closed:
        if (desc.type != "offer")
            throw std::logic_error{
                "set_local_description: desc.type != \"offer\""};
        break;
    case signaling_state_t::have_remote_offer:
        if (desc.type != "answer")
            throw std::logic_error{
                "set_local_description: desc.type != \"answer\""};
        break;
    }
    bool is_offer = (desc.type == "offer");
    _local_desc = std::move(desc);
    if (is_offer) {
        auto t_it = _transceivers.begin();
        for (const auto &media : _local_desc->medias) {
            if (t_it == _transceivers.end())
                break;
            (*t_it)->set_mid(media.mid);
            ++t_it;
        }
        _signaling_state = signaling_state_t::have_local_offer;
    } else
        _signaling_state = signaling_state_t::have_local_pranswer;

    co_await apply_descriptions();
    _rebuild_pt_maps();
    start_gathering();
    start_connecting();
}

asiortc::task<void>
connection_impl::set_remote_description(session_description desc) {
    switch (_signaling_state.get()) {
    case signaling_state_t::have_remote_offer:
    case signaling_state_t::have_local_pranswer:
    case signaling_state_t::have_remote_pranswer:
        throw std::logic_error{"set_remote_description: invalid state: " +
                               to_string(_signaling_state.get())};
    case signaling_state_t::stable:
    case signaling_state_t::closed:
        if (desc.type != "offer")
            throw std::logic_error{
                "set_remote_description: desc.type != \"offer\""};
        break;
    case signaling_state_t::have_local_offer:
        if (desc.type != "answer")
            throw std::logic_error{
                "set_remote_description: desc.type != \"answer\""};
        break;
    }
    bool is_offer = (desc.type == "offer");
    _remote_desc = std::move(desc);
    if (is_offer) {
        if (_remote_desc->bundle_groups.size() > 1)
            throw std::logic_error{
                "max-compat bundle policy not supported: "
                "multiple BUNDLE groups would require multiple transports"};
        if (!_remote_desc->bundle_groups.empty()) {
            const auto &bundled = _remote_desc->bundle_groups[0];
            for (const auto &rm : _remote_desc->medias)
                if (rm.media_type != "application" &&
                    std::find(bundled.begin(), bundled.end(), rm.mid) ==
                        bundled.end())
                    throw std::logic_error{
                        "balanced/max-compat bundle policy not supported: "
                        "media section mid=" +
                        rm.mid + " not in BUNDLE group"};
        }
        for (const auto &rm : _remote_desc->medias) {
            if (rm.media_type == "application")
                continue;
            if (rm.media_type != "video" && rm.media_type != "audio")
                continue;
            if (rm.mid.empty())
                throw std::invalid_argument{"rm.mid == \"\""};
            std::shared_ptr<rtp_transceiver> tr = nullptr;
            auto it = std::find_if(
                _transceivers.begin(), _transceivers.end(), [&](const auto &t) {
                    if (!t->mid().empty() || t->codecs().empty() ||
                        t->stopped())
                        return false;
                    const auto &name = t->codecs()[0].name;
                    if (rm.media_type == "audio")
                        return name == "opus" || name == "PCMU" ||
                               name == "PCMA" || name == "telephone-event" ||
                               name.starts_with("G7") || name == "CN" ||
                               name == "L16" || name == "L24";
                    return name.starts_with("VP") || name.starts_with("H26") ||
                           name.starts_with("AV1") || name == "rtx" ||
                           name == "red" || name == "ulpfec" ||
                           name == "flexfec";
                });
            if (it != _transceivers.end()) {
                (*it)->set_mid(rm.mid);
                (*it)->from_remote_sdp(rm);
                std::vector<sdp_codec> codecs;
                codecs.reserve((*it)->codecs().size());
                std::vector<bool> used(rm.rtpmaps.size(), false);
                for (const auto &lc : (*it)->codecs()) {
                    for (size_t j = 0; j < rm.rtpmaps.size(); ++j) {
                        if (used[j])
                            continue;
                        const auto &rc = rm.rtpmaps[j];
                        if (rc.name != lc.name ||
                            rc.clock_rate != lc.clock_rate)
                            continue;
                        // RTX: validate apt points to an accepted PT
                        if (lc.name == "rtx" && !lc.encoding_params.empty()) {
                            auto apt_pos = lc.encoding_params.find("apt=");
                            if (apt_pos != std::string::npos) {
                                int apt = std::stoi(
                                    lc.encoding_params.substr(apt_pos + 4));
                                bool apt_ok = false;
                                for (const auto &c : codecs)
                                    if (c.name != "rtx" &&
                                        c.payload_type == apt) {
                                        apt_ok = true;
                                        break;
                                    }
                                if (!apt_ok)
                                    continue;
                            }
                        }
                        auto c{lc};
                        c.payload_type = rc.payload_type;
                        codecs.emplace_back(std::move(c));
                        used[j] = true;
                        break;
                    }
                }
                (*it)->set_codecs(std::move(codecs));
                _rebuild_pt_maps();
                if (!(*it)->codecs().empty())
                    tr = *it;
            }
            if (!tr) {
                tr = std::make_shared<rtp_transceiver>(weak_from_this());
                tr->set_mid(rm.mid);
                tr->set_direction(sdp_direction::recvonly);
                tr->wire_back_references();
                tr->from_remote_sdp(rm);
                tr->set_codecs(rm.rtpmaps);
                _transceivers.push_back(tr);
            }
            if (tr->direction() == sdp_direction::sendrecv ||
                tr->direction() == sdp_direction::recvonly) {
                auto receiver = tr->receiver();
                auto track = receiver->track();
                if (!track) {
                    auto k = rm.media_type == "audio" ? media_kind::audio
                                                      : media_kind::video;
                    track = std::make_shared<media_track_impl>(k, rm.mid);
                    tr->receiver()->set_track(track);
                }
                if (_on_track_cb)
                    _on_track_cb(std::move(receiver), std::move(track),
                                 rm.msids, std::move(tr));
            }
        }
        std::erase_if(_transceivers,
                      [](const auto &tr) { return tr->mid().empty(); });
        _signaling_state = signaling_state_t::have_remote_offer;
    } else {
        for (auto &tr : _transceivers) {
            auto it =
                std::ranges::find_if(_remote_desc->medias, [&](const auto &rm) {
                    return rm.mid == tr->mid();
                });
            if (it == _remote_desc->medias.end() || it->rtpmaps.empty()) {
                tr->set_direction(sdp_direction::inactive);
                tr->stop();
                continue;
            }
            const auto &rm = *it;
            std::erase_if(tr->_codecs, [&](const auto &c) {
                return std::ranges::none_of(
                    it->rtpmaps, [&c](const sdp_codec &rc) {
                        return rc.name == c.name &&
                               rc.clock_rate == c.clock_rate;
                    });
            });
            if (tr->_codecs.empty()) {
                tr->set_direction(sdp_direction::inactive);
                tr->stop();
                continue;
            }
            tr->from_remote_sdp(rm);
            if (tr->direction() == sdp_direction::inactive) {
                tr->stop();
                continue;
            }

            if (tr->direction() == sdp_direction::sendrecv ||
                tr->direction() == sdp_direction::recvonly) {
                auto receiver = tr->receiver();
                auto track = receiver->track();
                if (!track) {
                    auto k = rm.media_type == "audio" ? media_kind::audio
                                                      : media_kind::video;
                    track = std::make_shared<media_track_impl>(k, rm.mid);
                    tr->receiver()->set_track(track);
                }
                if (_on_track_cb)
                    _on_track_cb(std::move(receiver), std::move(track),
                                 rm.msids, tr);
            }
        }
        _signaling_state = signaling_state_t::have_remote_pranswer;
    }

    co_await apply_descriptions();
    _rebuild_pt_maps();
    start_connecting();
}

void connection_impl::on_track(connection_impl::on_track_cb cb) {
    _on_track_cb = std::move(cb);
}

void connection_impl::register_encoder(std::string name,
                                       encoder_factory factory) {
    _codec_registry[std::move(name)] = std::move(factory);
}

void connection_impl::register_decoder(std::string name,
                                       decoder_factory factory) {
    _decoder_registry[std::move(name)] = std::move(factory);
}

asiortc::task<session_description> connection_impl::create_offer() {
    auto fp = _cert.get_fingerprint(asioice::ssl::hash_algorithm::sha256);

    session_description offer;
    offer.type = "offer";
    offer.version = 0;
    offer.origin.username = "-";
    offer.origin.session_id = 0;
    offer.origin.session_version = 0;
    offer.origin.nettype = "IN";
    offer.origin.addrtype = "IP4";
    offer.origin.addr = "0.0.0.0";
    offer.session_name = "-";
    offer.timing.start = 0;
    offer.timing.stop = 0;
    offer.ice_ufrag = _agent.local_username();
    offer.ice_pwd = _agent.local_password();
    offer.fingerprint = fp.hash_name() + " " + fp.value;
    offer.setup = "actpass";

    int mid_counter = 0;

    if (_need_sctp) {
        sdp_media app;
        app.mid = std::to_string(mid_counter);
        app.media_type = "application";
        app.port = 9;
        app.proto = "UDP/DTLS/SCTP";
        app.conn_nettype = "IN";
        app.conn_addrtype = "IP4";
        app.conn_addr = "0.0.0.0";
        app.direction = sdp_direction::sendrecv;
        app.sctpmap = "webrtc-datachannel";
        app.sctp_port = 5000;
        offer.medias.push_back(std::move(app));
        ++mid_counter;
    }

    for (auto &t : _transceivers) {
        const auto &sender = t->sender();
        if (!sender || !sender->track())
            continue;
        auto media = t->to_offer_sdp_media(std::to_string(mid_counter));
        media.ice_ufrag = offer.ice_ufrag;
        media.ice_pwd = offer.ice_pwd;
        media.fingerprint = offer.fingerprint;
        media.setup = offer.setup;
        offer.medias.push_back(std::move(media));
        ++mid_counter;
    }

    std::vector<std::string> mids;
    for (int i = 0; i < mid_counter; ++i)
        mids.push_back(std::to_string(i));
    if (!mids.empty())
        offer.bundle_groups.push_back(std::move(mids));

    if (_transceivers.size() > 0) {
        offer.msid_semantic = "WMS";
        offer.msid_tokens = {"*"};
    }

    offer.attributes.emplace_back("ice-options", "trickle");

    co_return offer;
}

asiortc::task<session_description> connection_impl::create_answer() {
    if (!_remote_desc ||
        _signaling_state != signaling_state_t::have_remote_offer) {
        throw std::runtime_error("set_remote_description must be called first");
    }

    auto fp = _cert.get_fingerprint(asioice::ssl::hash_algorithm::sha256);
    const auto remote_setup = setup_from(_remote_desc.value());
    const bool we_are_active = (remote_setup != "active");

    session_description answer;
    answer.type = "answer";
    answer.version = 0;
    answer.origin.username = "-";
    answer.origin.session_id = 0;
    answer.origin.session_version = 0;
    answer.origin.nettype = "IN";
    answer.origin.addrtype = "IP4";
    answer.origin.addr = "0.0.0.0";
    answer.session_name = "-";
    answer.timing.start = 0;
    answer.timing.stop = 0;
    answer.ice_ufrag = _agent.local_username();
    answer.ice_pwd = _agent.local_password();
    answer.fingerprint = fp.hash_name() + " " + fp.value;
    answer.setup = we_are_active ? "active" : "passive";

    for (const auto &rm : _remote_desc->medias) {
        if (rm.media_type == "application") {
            sdp_media answer_media;
            answer_media.mid = rm.mid;
            answer_media.media_type = rm.media_type;
            answer_media.port = 9;
            answer_media.proto = rm.proto;
            answer_media.conn_nettype = "IN";
            answer_media.conn_addrtype = "IP4";
            answer_media.conn_addr = "0.0.0.0";
            answer_media.direction = rm.direction;
            answer_media.sctpmap = rm.sctpmap;
            answer_media.sctp_port = rm.sctp_port;
            answer.medias.push_back(std::move(answer_media));
            continue;
        }

        auto it = std::find_if(
            _transceivers.begin(), _transceivers.end(),
            [&mid = rm.mid](const auto &t) { return t->mid() == mid; });

        sdp_media answer_media;
        if (it != _transceivers.end()) {
            answer_media = (*it)->to_answer_sdp_media(rm);
        } else {
            answer_media.mid = rm.mid;
            answer_media.media_type = rm.media_type;
            answer_media.port = 0;
            answer_media.proto = rm.proto;
            answer_media.direction = sdp_direction::inactive;
        }

        answer_media.conn_nettype = "IN";
        answer_media.conn_addrtype = "IP4";
        answer_media.conn_addr = "0.0.0.0";
        answer.medias.push_back(std::move(answer_media));
    }

    std::vector<std::string> answer_bundle;
    for (const auto &m : answer.medias)
        answer_bundle.push_back(m.mid);
    if (!answer_bundle.empty())
        answer.bundle_groups.push_back(std::move(answer_bundle));

    if (ice_options_trickle_from(*_remote_desc))
        answer.attributes.emplace_back("ice-options", "trickle");

    answer.msid_semantic = "WMS";
    answer.msid_tokens = {"*"};

    co_return answer;
}

std::shared_ptr<asiortc::data_channel>
connection_impl::create_data_channel(std::string label,
                                     asiortc::data_channel::options options) {
    if (_roles_set && !_sctp_transport)
        throw std::runtime_error{
            "No SCTP transport: create_data_channel must be called before "
            "set_local_description or set_remote_description"};

    _need_sctp = true;
    return std::shared_ptr<asiortc::data_channel>(new asiortc::data_channel(
        weak_from_this(), std::move(label), std::move(options)));
}

std::shared_ptr<rtp_transceiver>
connection_impl::add_transceiver(media_kind kind, rtp_transceiver_init init) {
    auto t = std::make_shared<rtp_transceiver>(weak_from_this());
    t->wire_back_references();
    t->set_direction(init.direction);
    t->set_codecs(kind == media_kind::video ? default_video_codecs()
                                            : default_audio_codecs());
    t->sender()->set_msids(std::move(init.streams));
    t->send_encodings = std::move(init.send_encodings);

    auto &encs = t->send_encodings;
    auto &streams = t->sender()->_streams;
    streams.clear();
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    if (encs.empty()) {
        auto s = std::make_shared<rtp_stream>();
        s->ssrc = dist(gen);
        s->rtx_ssrc = dist(gen);
        streams.push_back(std::move(s));
    } else {
        auto codecs = t->codecs();
        for (size_t i = 0; i < encs.size(); ++i) {
            auto s = std::make_shared<rtp_stream>();
            s->ssrc = dist(gen);
            s->rtx_ssrc = dist(gen);
            s->rid = encs[i].rid;
            // Assign RTX pt from codec list: index 2*i + 1
            size_t rtx_idx = 2 * i + 1;
            if (rtx_idx < codecs.size() && codecs[rtx_idx].name == "rtx")
                s->rtx_pt = codecs[rtx_idx].payload_type;
            streams.push_back(std::move(s));
        }
    }

    auto recv_track = std::make_shared<media_track_impl>(
        kind, "recv-" + std::to_string(_transceivers.size()));
    t->receiver()->set_track(std::move(recv_track));
    _transceivers.push_back(t);
    _rebuild_pt_maps();
    return t;
}

std::shared_ptr<rtp_transceiver>
connection_impl::add_transceiver(std::shared_ptr<media_track> track,
                                 rtp_transceiver_init init) {
    auto t = add_transceiver(track->kind(), std::move(init));
    t->sender()->set_track(std::move(track));
    return t;
}

asiortc::task<void> connection_impl::_sender_rtcp_loop(
    std::shared_ptr<rtp_sender> sender,
    std::shared_ptr<connection_impl::srtp_transport_type> srtp) {
    asioice::utils::scope_guard on_exit([]() noexcept {
        ICE_IN_DEBUG { std::cout << "_sender_rtcp_loop exited\n"; }
    });
    static thread_local std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dist(0.5, 1.5);
    std::vector<uint8_t> compound;

    net::steady_timer timer(this->get_executor());
    while (sender->stopped()) {
        auto delay =
            std::chrono::milliseconds(static_cast<int>(dist(gen) * 1000));
        timer.expires_after(delay);
        auto ec = co_await timer.async_wait(asioice::utils::use_sender);
        if (ec)
            co_return;

        compound.clear();
        for (const auto &stream : sender->_streams) {
            rtcp::rtcp_packet sr_pkt;
            sr_pkt.type = rtcp::packet_type::SR;
            sr_pkt.ssrc = stream->ssrc;
            sr_pkt.ntp_timestamp = stream->ntp_timestamp;
            sr_pkt.rtp_timestamp = stream->rtp_timestamp;
            sr_pkt.sender_packet_count = stream->packet_count;
            sr_pkt.sender_octet_count = stream->octet_count;
            std::vector<uint8_t> sr_buf(sr_pkt.serialized_size());
            sr_pkt.write_to(sr_buf.data(), sr_buf.size());
            compound.insert(compound.end(), sr_buf.begin(), sr_buf.end());
        }

        auto sdes = rtcp::sdes_chunk{0, "asiortc"}.bytes();
        compound.insert(compound.end(), sdes.begin(), sdes.end());

        auto send_result = co_await srtp->send_rtcp(compound);
        if (std::get<0>(send_result))
            co_return;
    }
}

asiortc::task<void> connection_impl::_receiver_rtcp_loop(
    std::shared_ptr<rtp_receiver> receiver,
    std::shared_ptr<connection_impl::srtp_transport_type> srtp) {
    asioice::utils::scope_guard on_exit([]() noexcept {
        ICE_IN_DEBUG { std::cout << "_receiver_rtcp_loop exited\n"; }
    });
    static thread_local std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dist(0.5, 1.5);

    net::steady_timer timer(this->get_executor());

    auto track = receiver->track();
    if (!track)
        co_return;
    while (receiver->stopped()) {
        auto delay =
            std::chrono::milliseconds(static_cast<int>(dist(gen) * 1000));
        timer.expires_after(delay);
        auto ec = co_await timer.async_wait(asioice::utils::use_sender);
        if (ec)
            co_return;

        // Build RR for SSRCs belonging to this receiver's track
        std::vector<uint8_t> compound;
        for (auto &[ssrc, st] : this->_stream_stats) {
            if (st.packets_received == 0)
                continue;

            // Only report on SSRCs associated with this receiver's track
            auto it_track = this->_ssrc_track_map.find(ssrc);
            if (it_track == this->_ssrc_track_map.end() ||
                it_track->second != track)
                continue;

            uint32_t extended_max = st.cycles + st.max_seq;
            uint32_t expected_interval =
                st.packets_expected - st.expected_prior;
            uint32_t received_interval =
                st.packets_received - st.received_prior;
            int lost_interval = static_cast<int>(expected_interval) -
                                static_cast<int>(received_interval);
            uint8_t fraction = 0;
            if (expected_interval > 0 && lost_interval > 0)
                fraction = static_cast<uint8_t>((lost_interval << 8) /
                                                expected_interval);
            int32_t cumulative_lost =
                static_cast<int32_t>(st.packets_expected) -
                static_cast<int32_t>(st.packets_received);
            if (cumulative_lost < 0)
                cumulative_lost = 0;
            if (cumulative_lost > 0x7FFFFF)
                cumulative_lost = 0x7FFFFF;

            uint32_t jitter = static_cast<uint32_t>(st.jitter_q4 >> 4);
            uint32_t lsr = static_cast<uint32_t>((st.lsr >> 16) & 0xFFFFFFFF);
            uint32_t dlsr = 0;
            if (st.lsr != 0) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - st.lsr_time)
                        .count();
                double d = elapsed / 1000000.0;
                if (d > 0 && d < 65536)
                    dlsr = static_cast<uint32_t>(d * 65536);
            }

            rtcp::rtcp_packet rr_pkt;
            rr_pkt.type = rtcp::packet_type::RR;
            rr_pkt.report_count = 1;
            rr_pkt.ssrc = 0;
            rtcp::report_block rb;
            rb.ssrc = ssrc;
            rb.fraction_lost = fraction;
            rb.cumulative_lost = cumulative_lost;
            rb.ext_highest_seq = extended_max;
            rb.jitter = jitter;
            rb.lsr = lsr;
            rb.dlsr = dlsr;
            rr_pkt.blocks.push_back(rb);
            std::vector<uint8_t> rr_buf(rr_pkt.serialized_size());
            rr_pkt.write_to(rr_buf.data(), rr_buf.size());
            compound.insert(compound.end(), rr_buf.begin(), rr_buf.end());

            st.expected_prior = st.packets_expected;
            st.received_prior = st.packets_received;
        }

        if (compound.empty())
            continue;

        auto send_result = co_await srtp->send_rtcp(compound);
        if (std::get<0>(send_result))
            co_return;
    }
}

asiortc::task<void> connection_impl::_nack_loop() {
    net::steady_timer timer(this->get_executor());
    while (true) {
        timer.expires_after(std::chrono::milliseconds(20));
        auto ec = co_await timer.async_wait(asioice::utils::use_sender);
        if (ec)
            co_return;

        auto now = std::chrono::steady_clock::now();
        std::vector<uint32_t> empty_ssrcs;
        for (auto &nack_entry : this->_nack_list) {
            auto ssrc = nack_entry.first;
            auto &entries = nack_entry.second;
            std::erase_if(entries,
                          [](const auto &e) { return e.retries >= 10; });

            std::vector<uint16_t> batch;
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->last_sent);
                if (elapsed.count() <
                    static_cast<int64_t>(this->_current_rtt_ms)) {
                    continue;
                }
                batch.push_back(it->sequence_number);
                it->last_sent = now;
                it->retries++;
            }
            if (entries.empty())
                empty_ssrcs.push_back(ssrc);
            if (!batch.empty()) {
                auto nack_bytes =
                    rtcp::rtcp_rtpfb{rtcp::packet_type::RTPFB_NACK, 0, ssrc,
                                     std::move(batch)}
                        .bytes();
                this->_sync_sender.send_rtcp(std::move(nack_bytes));
            }
        }
        for (auto ssrc : empty_ssrcs)
            this->_nack_list.erase(ssrc);
    }
}

void connection_impl::_start_nack_loop() {
    if (!_srtp_transport)
        return;
    _nack_loop_task = stdexec::spawn_future(
        stdexec::starts_on(stdexec::inline_scheduler{}, _nack_loop()),
        _scope.get_token());
}

asiortc::task<bool>
connection_impl::send_rtp(std::shared_ptr<rtp_sender> sender,
                          rtp::rtp_packet pkt) {
    if (!_srtp_transport)
        co_return false;

    if (!sender->_streams.empty()) {
        auto &st = sender->_streams[0];
        pkt.ssrc = st->ssrc;
        pkt.sequence_number = ++st->seq;
    }

    std::vector<uint8_t> buf(pkt.serialized_size());
    pkt.write_to(buf.data(), buf.size());
    try {
        auto r = co_await _srtp_transport->send_rtp(buf);
        bool ok = !std::get<0>(r);
        if (ok) {
            _tx_packets++;
            _tx_bytes += buf.size();
            if (!sender->_streams.empty()) {
                auto &st = sender->_streams[0];
                st->packet_count++;
                st->octet_count += static_cast<uint32_t>(pkt.payload.size());
            }
        }
        co_return ok;
    } catch (...) {
        co_return false;
    }
}

void connection_impl::_start_sender_loops() {
    if (!_srtp_transport)
        return;

    for (auto &t : _transceivers) {
        auto sender = t->sender();
        if (!sender || sender->_stopped)
            continue;

        auto track = sender->track();
        if (!track)
            continue;

        sender->_send_rtp_loop = stdexec::spawn_future(
            stdexec::starts_on(
                stdexec::inline_scheduler{},
                _sender_send_loop(sender->shared_from_this(), _srtp_transport)),
            _scope.get_token());

        sender->_send_rtcp_loop = stdexec::spawn_future(
            stdexec::starts_on(
                stdexec::inline_scheduler{},
                _sender_rtcp_loop(sender->shared_from_this(), _srtp_transport)),
            _scope.get_token());
    }
}

void connection_impl::close() noexcept {
    _scope.request_stop();
    _connection_state = connection_state_t::closed;
    _agent.close();
    _sync_sender.stop();
    if (_data_channel_manager)
        _data_channel_manager->stop();
    _ice_transport.reset();
    _dtls_transport.reset();
    if (_srtp_transport) {
        _srtp_transport->on_rtp_rtcp_packet(nullptr);
        _srtp_transport->on_new_ssrc(nullptr);
        _srtp_transport.reset();
    }
    _sctp_transport.reset();

    for (auto &t : _transceivers)
        t->stop();
    _transceivers.clear();
    _ssrc_track_map.clear();
    _pt_codec_map.clear();
    _pt_receiver_map.clear();
    _mid_track_map.clear();
    _nack_list.clear();
    _nack_loop_task.reset();
    _mid_ext_id = 0;

    _local_desc.reset();
    _remote_desc.reset();
    _pending_local_desc.reset();
    _pending_remote_desc.reset();
    _gathering_task.reset();
    _connecting_task.reset();

    _on_remote_channel_cb = nullptr;
    _on_rtp_rtcp_cb = nullptr;
    _on_new_ssrc_cb = nullptr;

    asioice::utils::detached_with_data(
        stdexec::starts_on(
            stdexec::inline_scheduler{},
            _scope.join() |
                stdexec::continues_on(asioice::utils::scheduler{_executor})),
        this->shared_from_this());
}

void connection_impl::on_remote_channel(on_data_channel_cb cb) {
    _on_remote_channel_cb = std::move(cb);
}

ice_connection_state_t
connection_impl::to_ice_connection_state(asioice::agent_state_t s) noexcept {
    switch (s) {
    case asioice::agent_state_t::INIT:
    case asioice::agent_state_t::GATHERING:
        return ice_connection_state_t::init;
    case asioice::agent_state_t::CONNECTING:
        return ice_connection_state_t::checking;
    case asioice::agent_state_t::CONNECTED:
        return ice_connection_state_t::connected;
    case asioice::agent_state_t::CLOSED:
        return ice_connection_state_t::failed;
    }
}

void connection_impl::do_on_data_channel(
    std::shared_ptr<connection_impl::data_channel_type> p) {
    assert(p);
    if (_on_remote_channel_cb) {
        auto ch =
            std::shared_ptr<asiortc::data_channel>(new asiortc::data_channel(
                this->weak_from_this(), p->label(), p->options()));
        ch->_channel = std::move(p);
        _on_remote_channel_cb(std::move(ch));
    }
}

asiortc::task<bool> data_channel::open() {
    if (_channel)
        co_return true;
    auto conn = _conn.lock();
    if (!conn)
        co_return false;
    auto *manager = conn->data_channel_manager();
    if (!manager)
        co_return false;
    _channel = co_await manager->create_data_channel(_label, _options);
    if (!_channel)
        co_return false;
    _options = _channel->options();
    co_return true;
}

rtc_stats_report connection_impl::get_stats() const {
    rtc_stats_report report;
    auto now = std::chrono::system_clock::now();

    // Transport stats
    auto ts = std::make_shared<rtc_transport_stats>();
    ts->timestamp = now;
    ts->type = "transport";
    ts->id = _transport_stats_id;
    ts->packets_sent = _tx_packets;
    ts->packets_received = _rx_packets;
    ts->bytes_sent = _tx_bytes;
    ts->bytes_received = _rx_bytes;
    ts->ice_role = "unknown";
    ts->dtls_state = _srtp_transport ? "connected" : "new";
    report[ts->id] = ts;

    // Outbound stats per sender
    for (auto &t : _transceivers) {
        auto s = t->sender();
        if (!s)
            continue;
        auto track = s->track();
        if (!track)
            continue;
        for (const auto &stream : s->_streams) {
            auto os = std::make_shared<rtc_outbound_rtp_stream_stats>();
            os->timestamp = now;
            os->type = "outbound-rtp";
            os->id =
                "outbound-rtp_" + t->mid() +
                (s->_streams.size() > 1 ? "_" + std::to_string(stream->ssrc)
                                        : "");
            os->ssrc = stream->ssrc;
            os->kind = track->kind() == media_kind::video ? "video" : "audio";
            os->transport_id = _transport_stats_id;
            os->packets_sent = stream->packet_count;
            os->bytes_sent = stream->octet_count;
            os->track_id = track->id();
            report[os->id] = os;
        }
    }

    // Inbound stats per SSRC
    for (auto &[ssrc, st] : _stream_stats) {
        auto is = std::make_shared<rtc_inbound_rtp_stream_stats>();
        is->timestamp = now;
        is->type = "inbound-rtp";
        is->id = "inbound-rtp_" + std::to_string(ssrc);
        is->ssrc = ssrc;
        auto it = _ssrc_track_map.find(ssrc);
        is->kind = (it != _ssrc_track_map.end() &&
                    it->second->kind() == media_kind::video)
                       ? "video"
                       : "audio";
        is->transport_id = _transport_stats_id;
        is->packets_received = st.packets_received;
        is->packets_lost =
            static_cast<int64_t>(st.packets_expected) - st.packets_received;
        is->jitter = static_cast<uint32_t>(st.jitter_q4 >> 4);
        report[is->id] = is;
    }

    // Remote outbound stats (from RTCP SR)
    for (auto &[ssrc, ro] : _remote_outbound_stats) {
        auto s = std::make_shared<rtc_remote_outbound_rtp_stream_stats>(ro);
        s->kind = "audio";
        auto it = _ssrc_track_map.find(ssrc);
        if (it != _ssrc_track_map.end())
            s->kind =
                it->second->kind() == media_kind::video ? "video" : "audio";
        s->transport_id = _transport_stats_id;
        s->id = "remote-outbound-rtp_" + std::to_string(ssrc);
        report[s->id] = s;
    }

    // Remote inbound stats (from RTCP RR)
    for (auto &[ssrc, ri] : _remote_inbound_stats) {
        auto s = std::make_shared<rtc_remote_inbound_rtp_stream_stats>(ri);
        s->kind = "audio";
        auto it = _ssrc_track_map.find(ssrc);
        if (it != _ssrc_track_map.end())
            s->kind =
                it->second->kind() == media_kind::video ? "video" : "audio";
        s->transport_id = _transport_stats_id;
        s->id = "remote-inbound-rtp_" + std::to_string(ssrc);
        report[s->id] = s;
    }

    return report;
}

void connection_impl::do_on_rtp_rtcp_packet(asioice::io_buffer_ptr buf) {
    _rx_packets++;
    _rx_bytes += buf->size();
    auto pkt = rtp::rtp_packet::parse(buf->data(), buf->size());
    if (pkt) {
        std::shared_ptr<media_track_impl> mid_track;

        if (!pkt->extension_data.empty()) {
            const auto &ext = pkt->extension_data;
            size_t off = 0;
            while (off + 1 <= ext.size()) {
                uint8_t hdr = ext[off];
                if (hdr == 0) {
                    off++;
                    continue;
                }
                uint8_t id = (hdr >> 4) & 0xF;
                uint8_t len = (hdr & 0xF) + 1;
                if (id == 4 && off + 1 + len <= ext.size()) {
                    uint16_t tcc_seq =
                        (static_cast<uint16_t>(ext[off + 1]) << 8) |
                        static_cast<uint16_t>(ext[off + 2]);
                    _twcc_recv.push_back(
                        {.transport_seq = tcc_seq,
                         .recv_time = std::chrono::steady_clock::now()});
                }
                if (_mid_ext_id > 0 &&
                    id == static_cast<uint8_t>(_mid_ext_id) &&
                    off + 1 + len <= ext.size()) {
                    std::string mid(
                        reinterpret_cast<const char *>(&ext[off + 1]), len);
                    auto mit = _mid_track_map.find(mid);
                    if (mit != _mid_track_map.end())
                        mid_track = mit->second;
                }
                off += 1 + len;
            }
        }

        // Track stream statistics for RTCP RR
        auto &st = _stream_stats[pkt->ssrc];
        st.ssrc = pkt->ssrc;
        if (!st.base_seq_set) {
            st.base_seq = pkt->sequence_number;
            st.base_seq_set = true;
        }
        if (pkt->sequence_number < st.max_seq) {
            if (st.max_seq - pkt->sequence_number > 0x8000)
                st.cycles += 0x10000;
        }
        st.max_seq = std::max<uint32_t>(st.max_seq, pkt->sequence_number);
        st.packets_expected = st.cycles + st.max_seq - st.base_seq + 1;
        st.packets_received++;

        auto now = std::chrono::steady_clock::now();
        auto arr_ts = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count() /
            1000 * 90);
        if (st.last_arrival_ts != 0) {
            int transit = static_cast<int>(arr_ts - st.last_arrival_ts) -
                          static_cast<int>(pkt->timestamp - st.last_rtp_ts);
            if (transit < 0)
                transit = -transit;
            st.jitter_q4 += transit - ((st.jitter_q4 + 8) >> 4);
        }
        st.last_arrival_ts = arr_ts;
        st.last_rtp_ts = pkt->timestamp;

        // PLI + NACK trigger: gap detection
        uint32_t expected =
            st.base_seq_set
                ? st.cycles + st.base_seq + (st.packets_expected % 0x10000)
                : pkt->sequence_number;
        if (st.base_seq_set &&
            pkt->sequence_number != static_cast<uint16_t>(expected)) {
            st.consecutive_lost++;

            // NACK: collect lost sequence numbers
            std::vector<uint16_t> lost;
            uint16_t s = static_cast<uint16_t>(expected);
            while (s != pkt->sequence_number) {
                if (lost.size() < 32)
                    lost.push_back(s);
                else
                    break;
                ++s;
            }
            if (!lost.empty()) {
                auto &nack_list = _nack_list[pkt->ssrc];
                for (uint16_t s : lost) {
                    bool exists = false;
                    for (const auto &e : nack_list)
                        if (e.sequence_number == s) {
                            exists = true;
                            break;
                        }
                    if (!exists)
                        nack_list.push_back(
                            {s, std::chrono::steady_clock::now(),
                             std::chrono::steady_clock::time_point{}, 0});
                }
            }

            if (st.consecutive_lost >= 3 && _srtp_transport) {
                auto pli =
                    rtcp::rtcp_psfb{rtcp::packet_type::PSFB_PLI, 0, pkt->ssrc}
                        .bytes();
                _sync_sender.send_rtcp(std::move(pli));
                st.consecutive_lost = 0;
            }
        } else {
            st.consecutive_lost = 0;
        }

        // Received packet → remove from NACK list
        {
            auto nit = _nack_list.find(pkt->ssrc);
            if (nit != _nack_list.end()) {
                auto &entries = nit->second;
                std::erase_if(entries, [&](const auto &e) {
                    return e.sequence_number == pkt->sequence_number;
                });
                if (entries.empty())
                    _nack_list.erase(nit);
            }
        }

        // Three-tier demux: MID -> SSRC -> PT
        auto to_push = [&](std::shared_ptr<media_track_impl> track) {
            auto *codec = _find_codec(pkt->payload_type);
            if (!codec)
                return;

            bool is_new =
                (_ssrc_track_map.find(pkt->ssrc) == _ssrc_track_map.end());
            if (is_new)
                _ssrc_track_map[pkt->ssrc] = track;

            if (!track->_decoder && !track->_decoder_codec_name.empty()) {
                auto dit = _decoder_registry.find(track->_decoder_codec_name);
                if (dit != _decoder_registry.end())
                    track->_decoder = dit->second();
            }

            {
                auto pt_it = _pt_receiver_map.find(pkt->payload_type);
                if (pt_it != _pt_receiver_map.end()) {
                    auto &recv = pt_it->second.receiver;
                    if (recv && recv->_on_rtp_cb) {
                        if (!recv->_on_rtp_cb(*pkt))
                            return;
                    }
                }
            }

            if (codec->name == "rtx" && pkt->payload.size() >= 2) {
                pkt->sequence_number = asioice::binary::ntoh<uint16_t>(
                    *reinterpret_cast<const uint16_t *>(pkt->payload.data()));
                pkt->payload.erase(pkt->payload.begin(),
                                   pkt->payload.begin() + 2);
            } else {
                pkt->payload = depayload(codec->name, std::move(pkt->payload));
            }

            if (pkt->payload.empty())
                return;

            track->push_frame(std::move(*pkt));
        };

        if (mid_track) {
            to_push(mid_track);
        } else if (auto it = _ssrc_track_map.find(pkt->ssrc);
                   it != _ssrc_track_map.end()) {
            to_push(it->second);
        } else if (auto pt_it = _pt_receiver_map.find(pkt->payload_type);
                   pt_it != _pt_receiver_map.end()) {
            if (pt_it->second.codec_name != "rtx")
                to_push(pt_it->second.track);
        }
    } else {
        // Handle compound RTCP
        auto cps = rtcp::parse_compound(buf->data(), buf->size());
        for (const auto &cp : cps) {
            if (cp.type == rtcp::packet_type::SR) {
                auto &st = _stream_stats[cp.ssrc];
                st.ssrc = cp.ssrc;
                st.lsr = cp.ntp_timestamp;
                st.lsr_time = std::chrono::steady_clock::now();

                auto &ro = _remote_outbound_stats[cp.ssrc];
                ro.timestamp = std::chrono::system_clock::now();
                ro.type = "remote-outbound-rtp";
                ro.ssrc = cp.ssrc;
                ro.packets_sent = cp.sender_packet_count;
                ro.bytes_sent = cp.sender_octet_count;
                ro.remote_timestamp = cp.ntp_timestamp;
            } else if (cp.type == rtcp::packet_type::RR) {
                for (const auto &b : cp.blocks) {
                    auto &ri = _remote_inbound_stats[b.ssrc];
                    ri.timestamp = std::chrono::system_clock::now();
                    ri.type = "remote-inbound-rtp";
                    ri.ssrc = b.ssrc;
                    ri.packets_lost = b.cumulative_lost;
                    ri.jitter = b.jitter;
                    ri.fraction_lost = b.fraction_lost / 256.0;
                    if (b.lsr && b.dlsr) {
                        auto now = std::chrono::steady_clock::now();
                        auto it = _stream_stats.find(b.ssrc);
                        if (it != _stream_stats.end() &&
                            it->second.lsr_time.time_since_epoch().count() >
                                0) {
                            auto rtt_us = std::chrono::duration_cast<
                                              std::chrono::microseconds>(
                                              now - it->second.lsr_time)
                                              .count() -
                                          (static_cast<int64_t>(b.dlsr) *
                                           1000000 / 65536);
                            if (rtt_us > 0)
                                ri.round_trip_time = rtt_us / 1000000.0;
                        }
                    }
                }
            } else if (cp.type == rtcp::packet_type::PSFB) {
                if (cp.report_count == rtcp::packet_type::PSFB_PLI ||
                    cp.report_count == rtcp::packet_type::PSFB_FIR) {
                    // PLI/FIR → force keyframe for matching SSRC
                    for (const auto &t : _transceivers) {
                        auto s = t->sender();
                        if (!s || s->stopped())
                            continue;
                        if (cp.media_ssrc) {
                            for (const auto &stream : s->_streams)
                                if (stream->ssrc == cp.media_ssrc)
                                    s->_force_keyframe = true;
                        } else {
                            s->_force_keyframe = true;
                        }
                    }
                } else if (cp.report_count == rtcp::packet_type::PSFB_APP &&
                           !cp.payload.empty()) {
                    auto [bitrate, ssrcs] =
                        rtcp::parse_remb(cp.payload.data(), cp.payload.size());
                    for (auto ssrc : ssrcs) {
                        for (auto &t : _transceivers) {
                            auto s = t->sender();
                            if (s) {
                                codecs::encoder_params p;
                                p.bitrate = static_cast<int>(bitrate);
                                for (auto &enc : s->_encoders)
                                    enc->set_parameters(p);
                            }
                        }
                    }
                }
            } else if (cp.type == rtcp::packet_type::RTPFB &&
                       cp.report_count == rtcp::packet_type::RTPFB_NACK) {
                auto lost =
                    rtcp::parse_nack(cp.payload.data(), cp.payload.size());
                for (uint16_t seq : lost) {
                    for (auto &t : _transceivers) {
                        auto s = t->sender();
                        if (!s)
                            continue;
                        for (const auto &stream : s->_streams) {
                            if (cp.media_ssrc && stream->ssrc != cp.media_ssrc)
                                continue;
                            auto &e =
                                stream->history[seq % rtp_stream::HISTORY_SIZE];
                            if (e && e->seq == seq) {
                                // RTX wrap: 12B header + 2B OSN +
                                // payload
                                std::vector<uint8_t> rtx(14 +
                                                         e->payload.size());
                                rtx[0] = 0x80;
                                rtx[1] = stream->rtx_pt;
                                uint16_t rseq = ++stream->rtx_seq;
                                asioice::binary::write_big<uint16_t>(
                                    rtx.data() + 2, rseq);
                                asioice::binary::write_big<uint32_t>(
                                    rtx.data() + 4, e->timestamp);
                                asioice::binary::write_big<uint32_t>(
                                    rtx.data() + 8, stream->rtx_ssrc);
                                // OSN: original sequence number
                                asioice::binary::write_big<uint16_t>(
                                    rtx.data() + 12, seq);
                                std::memcpy(rtx.data() + 14, e->payload.data(),
                                            e->payload.size());
                                _sync_sender.send_rtp(std::move(rtx));
                                break;
                            }
                        }
                    }
                }
            } else if (cp.type == rtcp::packet_type::RTPFB &&
                       cp.report_count == rtcp::packet_type::RTPFB_TCC) {
                auto fb = rtcp::parse_transport_cc(cp.payload.data(),
                                                   cp.payload.size());
                // Store for future GCC consumption
                (void)fb;
            }
        }
    }
    if (_on_rtp_rtcp_cb)
        _on_rtp_rtcp_cb(std::move(buf));
}

bool connection_impl::do_on_new_ssrc(uint32_t ssrc,
                                     std::span<const uint8_t> data) {
    auto pkt = rtp::rtp_packet::parse(data.data(), data.size());
    if (!pkt)
        return false;

    if (_on_new_ssrc_cb)
        return _on_new_ssrc_cb(ssrc, data);
    return true;
}

void connection_impl::sync_rtp_rtcp_sender::send_rtp(
    std::vector<uint8_t> data) {
    if (_impl._connection_state == connection_state_t::closed ||
        !_impl._srtp_transport)
        return;
    _pending_rtp.push(std::move(data));
    if (!_send_rtp_task)
        _send_rtp_task = stdexec::spawn_future(
            [](auto srtp, auto &q) -> asiortc::task<void> {
                asioice::utils::scope_guard on_exit([]() noexcept {
                    ICE_IN_DEBUG {
                        std::cout
                            << "sync_rtp_rtcp_sender::_send_rtp_task exited\n";
                    }
                });
                while (true) {
                    auto data = q.try_pop();
                    if (!data)
                        data = co_await q.async_pop_stoppable();
                    if (!data)
                        co_return;
                    co_await srtp->send_rtp(*data);
                }
            }(_impl._srtp_transport, _pending_rtp),
            _impl._scope.get_token());
}

void connection_impl::sync_rtp_rtcp_sender::send_rtcp(
    std::vector<uint8_t> data) {
    if (_impl._connection_state == connection_state_t::closed ||
        !_impl._srtp_transport)
        return;
    _pending_rtcp.push(std::move(data));
    if (!_send_rtcp_task)
        _send_rtcp_task = stdexec::spawn_future(
            [](auto srtp, auto &q) -> asiortc::task<void> {
                asioice::utils::scope_guard on_exit([]() noexcept {
                    ICE_IN_DEBUG {
                        std::cout
                            << "sync_rtp_rtcp_sender::_send_rtcp_task exited\n";
                    }
                });
                while (true) {
                    auto data = q.try_pop();
                    if (!data)
                        data = co_await q.async_pop_stoppable();
                    if (!data)
                        co_return;
                    co_await srtp->send_rtcp(*data);
                }
            }(_impl._srtp_transport, _pending_rtcp),
            _impl._scope.get_token());
}

void connection_impl::_register_default_codecs() {
    _codec_registry.try_emplace("VP8", [](const auto &p) {
        return std::make_shared<codecs::DefaultVp8Encoder>(p);
    });
    _codec_registry.try_emplace("VP9", [](const auto &p) {
        return std::make_shared<codecs::DefaultVp9Encoder>(p);
    });
    _codec_registry.try_emplace("H264", [](const auto &p) {
        return std::make_shared<codecs::DefaultH264Encoder>(p);
    });
    _codec_registry.try_emplace("opus", [](const auto &p) {
        return std::make_shared<codecs::DefaultOpusEncoder>(p);
    });
}

} // namespace asiortc

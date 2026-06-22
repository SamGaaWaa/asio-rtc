#include "connection_impl.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exec/async_scope.hpp>
#include <exec/finally.hpp>
#include <stdexcept>
#include <string>

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
#include "rtp.hpp"

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

asioice::task<void>
connection_impl::_sender_send_loop(std::weak_ptr<rtp_sender> weak_sender,
                                   std::shared_ptr<srtp_transport_type> srtp) {

    std::vector<uint8_t> enc_buf;

    while (true) {
        auto sender = weak_sender.lock();
        if (!sender || sender->stopped())
            co_return;

        auto track = sender->track();
        if (!track)
            co_return;

        auto frame = co_await track->recv();
        if (!frame)
            co_return;

        // VP8 RTP payload: 1-byte descriptor + VP8 bitstream
        uint8_t vp8_desc = frame->data[0];
        const uint8_t *vp8_data = frame->data.data() + 1;
        size_t vp8_len = frame->data.size() - 1;

        // Fragment large frames to stay under MTU
        static constexpr size_t MAX_RTP_PAYLOAD = 1200;

        if (vp8_len <= MAX_RTP_PAYLOAD - 1) {
            // Single packet
            std::vector<uint8_t> rtp_buf(12 + frame->data.size());
            rtp_buf[0] = 0x80;
            rtp_buf[1] = frame->payload_type | 0x80; // marker
            uint16_t seq = ++sender->_seq;
            asioice::binary::write_big<uint16_t>(rtp_buf.data() + 2, seq);
            asioice::binary::write_big<uint32_t>(rtp_buf.data() + 4,
                                                 frame->timestamp);
            asioice::binary::write_big<uint32_t>(rtp_buf.data() + 8,
                                                 frame->ssrc);
            std::memcpy(rtp_buf.data() + 12, frame->data.data(),
                        frame->data.size());

            enc_buf.resize(rtp_buf.size() +
                           srtp_transport_base::max_protect_rtp_overhead());
            auto r = co_await srtp->send_rtp(rtp_buf, enc_buf);
            if (std::get<0>(r))
                co_return;
        } else {
            // Fragmented VP8 RTP
            size_t off = 0;
            bool first = true;
            while (off < vp8_len) {
                size_t chunk = std::min(vp8_len - off, MAX_RTP_PAYLOAD - 1);
                uint8_t desc = first ? vp8_desc : uint8_t(0);
                std::vector<uint8_t> rtp_buf(12 + 1 + chunk);
                rtp_buf[0] = 0x80;
                bool is_last = (off + chunk >= vp8_len);
                rtp_buf[1] = frame->payload_type | (is_last ? 0x80 : 0);
                uint16_t seq = ++sender->_seq;
                asioice::binary::write_big<uint16_t>(rtp_buf.data() + 2, seq);
                asioice::binary::write_big<uint32_t>(rtp_buf.data() + 4,
                                                     frame->timestamp);
                asioice::binary::write_big<uint32_t>(rtp_buf.data() + 8,
                                                     frame->ssrc);
                rtp_buf[12] = desc;
                std::memcpy(rtp_buf.data() + 13, vp8_data + off, chunk);

                enc_buf.resize(rtp_buf.size() +
                               srtp_transport_base::max_protect_rtp_overhead());
                auto r = co_await srtp->send_rtp(rtp_buf, enc_buf);
                if (std::get<0>(r))
                    co_return;

                off += chunk;
                first = false;
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
    ret.enable_mdns = true;
    ret.ice_servers.urls = std::move(cfg.ice_servers.urls);
    ret.ice_servers.username = std::move(cfg.ice_servers.username);
    ret.ice_servers.password = std::move(cfg.ice_servers.password);
    return ret;
}

connection_impl::connection_impl(connection_impl::executor_type ex,
                                 asiortc::configuration cfg)
    : _executor{std::move(ex)},
      _agent{_executor, get_agent_config(std::move(cfg))} {
    _agent.on_local_candidates(
        std::bind_front(&connection_impl::do_on_candidates, this));
}

asioice::task<void> connection_impl::apply_descriptions() {
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
    if (_gathering_task)
        return;
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
                    this->_gathering_task.reset();
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
    if (_local_desc && _remote_desc && !_connecting_task) {
        _connecting_task = stdexec::spawn_future(
            stdexec::starts_on(
                stdexec::inline_scheduler{},
                exec::finally(this->do_connect(),
                              stdexec::just() | stdexec::then([this] {
                                  this->_connecting_task.reset();
                              }))),
            _scope.get_token());
    }
}

asioice::task<void> connection_impl::do_connect() {
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
            auto srtp_role = we_are_active ? asioice::ssl::dtls_role::client
                                           : asioice::ssl::dtls_role::server;
            _srtp_transport->setup(*keys, srtp_role);

            auto user_new_ssrc = std::move(_pending_srtp_new_ssrc_cb);
            auto user_rtp = std::move(_pending_srtp_rtp_cb);

            _srtp_transport->on_new_ssrc(
                [this, user_cb = std::move(user_new_ssrc)](
                    uint32_t ssrc,
                    std::span<const uint8_t> data) mutable -> bool {
                    auto pkt = rtp::rtp_packet::parse(data.data(), data.size());
                    if (pkt) {
                        for (auto &t : _transceivers) {
                            for (auto &c : t->codecs()) {
                                if (c.payload_type == pkt->payload_type) {
                                    auto recv = t->receiver();
                                    auto track = recv ? recv->track() : nullptr;
                                    if (track) {
                                        _ssrc_track_map[ssrc] =
                                            std::static_pointer_cast<
                                                media_track_impl>(track);
                                    }
                                    goto ssrc_done;
                                }
                            }
                        }
                    }
                ssrc_done:
                    if (user_cb)
                        return user_cb(ssrc, data);
                    return true;
                });

            _srtp_transport->on_rtp_rtcp_packet(
                [this, user_cb = std::move(user_rtp)](
                    asioice::io_buffer_ptr buf) mutable {
                    auto pkt = rtp::rtp_packet::parse(buf->data(), buf->size());
                    if (pkt) {
                        auto it = _ssrc_track_map.find(pkt->ssrc);
                        if (it != _ssrc_track_map.end()) {
                            auto hdr_len = pkt->serialized_size();
                            auto payload_start =
                                reinterpret_cast<const uint8_t *>(buf->data()) +
                                hdr_len;
                            auto payload_len = buf->size() - hdr_len;

                            media_frame frame;
                            frame.kind = it->second->kind();
                            frame.ssrc = pkt->ssrc;
                            frame.timestamp = pkt->timestamp;
                            frame.payload_type = pkt->payload_type;
                            frame.marker = pkt->marker;
                            frame.data.assign(payload_start,
                                              payload_start + payload_len);
                            it->second->push_frame(std::move(frame));
                        }
                    }
                    if (user_cb)
                        user_cb(std::move(buf));
                });
            _start_sender_loops();
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
        std::bind_front(&connection_impl::on_data_channel, this));
    _data_channel_manager->start();
    _connection_state = connection_state_t::connected;
}

asioice::task<void>
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
    if (is_offer)
        _signaling_state = signaling_state_t::have_local_offer;
    else
        _signaling_state = signaling_state_t::have_local_pranswer;

    co_await apply_descriptions();
    start_gathering();
    start_connecting();
}

asioice::task<void>
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
        _signaling_state = signaling_state_t::have_remote_offer;
        for (const auto &rm : _remote_desc->medias) {
            if (rm.media_type == "application")
                continue;
            auto it =
                std::find_if(_transceivers.begin(), _transceivers.end(),
                             [&](const auto &t) { return t->mid() == rm.mid; });
            if (it == _transceivers.end()) {
                auto tr = std::make_shared<rtp_transceiver>(weak_from_this());
                tr->set_mid(rm.mid);
                tr->set_direction(sdp_direction::recvonly);
                tr->wire_back_references();
                tr->from_remote_sdp(rm);
                tr->set_codecs(rm.rtpmaps);

                auto k = rm.media_type == "audio" ? media_kind::audio
                                                  : media_kind::video;
                auto track = std::make_shared<media_track_impl>(
                    k, rm.mid,
                    static_cast<net::io_context &>(_executor.context()));
                tr->receiver()->set_track(track);

                auto receiver = tr->receiver();
                _transceivers.push_back(tr);
                // TODO: should invoke here?
                if (_on_track_cb)
                    _on_track_cb(std::move(receiver), std::move(track),
                                 rm.msids, std::move(tr));
            } else {
                (*it)->from_remote_sdp(rm);
            }
        }
    } else {
        _signaling_state = signaling_state_t::have_remote_pranswer;
    }

    co_await apply_descriptions();
    start_connecting();
}

void connection_impl::on_track(connection_impl::on_track_cb cb) {
    _on_track_cb = std::move(cb);
}

asioice::task<session_description> connection_impl::create_offer() {
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
        if (t->mid().empty())
            t->set_mid(std::to_string(mid_counter));
        auto media = t->to_offer_sdp_media();
        media.ice_ufrag = offer.ice_ufrag;
        media.ice_pwd = offer.ice_pwd;
        media.fingerprint = offer.fingerprint;
        media.setup = offer.setup;
        offer.medias.push_back(std::move(media));
        ++mid_counter;
    }

    for (int i = 0; i < mid_counter; ++i)
        offer.bundle_groups.push_back(std::to_string(i));

    offer.attributes.emplace_back("ice-options", "trickle");

    co_return offer;
}

asioice::task<session_description> connection_impl::create_answer() {
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
    answer.bundle_groups = _remote_desc->bundle_groups.empty()
                               ? std::vector<std::string>{"0"}
                               : _remote_desc->bundle_groups;
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

    if (ice_options_trickle_from(*_remote_desc))
        answer.attributes.emplace_back("ice-options", "trickle");

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
    _transceivers.push_back(t);
    return t;
}

std::shared_ptr<rtp_transceiver>
connection_impl::add_transceiver(std::shared_ptr<media_track> track,
                                 rtp_transceiver_init init) {
    auto t = add_transceiver(track->kind(), std::move(init));
    t->sender()->set_track(std::move(track));
    return t;
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

        sender->_send_loop = stdexec::spawn_future(
            stdexec::starts_on(
                stdexec::inline_scheduler{},
                _sender_send_loop(sender->weak_from_this(), _srtp_transport)),
            _scope.get_token());
    }
}

void connection_impl::close() noexcept {
    _scope.request_stop();
    _connection_state = connection_state_t::closed;
    _agent.close();
    if (_data_channel_manager)
        _data_channel_manager->stop();
    _ice_transport.reset();
    _dtls_transport.reset();
    _srtp_transport.reset();
    _sctp_transport.reset();

    for (auto &t : _transceivers)
        t->stop();
    _transceivers.clear();
    _ssrc_track_map.clear();

    _local_desc.reset();
    _remote_desc.reset();
    _pending_local_desc.reset();
    _pending_remote_desc.reset();
    _gathering_task.reset();
    _connecting_task.reset();

    _on_remote_channel_cb = nullptr;

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

void connection_impl::on_data_channel(
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

asioice::task<bool> data_channel::open() {
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

} // namespace asiortc

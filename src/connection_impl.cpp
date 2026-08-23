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
#include <mutex>
#include <charconv>

#include <boost/container/static_vector.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/container/flat_map.hpp>

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
#include "samlog.hpp"
#include "rtc_base/logging.h"

#include "rtp_packetizer/h264_packetizer.hpp"
#include "rtp_packetizer/opus_packetizer.hpp"
#include "rtp_packetizer/vpx_packetizer.hpp"

namespace asiortc {

struct dcsctp_log_sink final : webrtc::LogSink {
    dcsctp_log_sink() {}

    auto log(webrtc::LoggingSeverity severity) {
        return samlog::__log_from_generator(samlog::logger_instance().get(),
                                            severity_to_level(severity));
    }

    void OnLogMessage(const std::string &msg, webrtc::LoggingSeverity severity,
                      const char *tag) override {
        log(severity) << [&](auto sink) { sink("[{}]: {}\n", tag, msg); };
    }

    void OnLogMessage(const std::string &message,
                      webrtc::LoggingSeverity severity) override {
        log(severity) << [&](auto sink) { sink(message); };
    }

    void OnLogMessage(const std::string &message) override {
#ifdef NDEBUG
        OnLogMessage(message, webrtc::LoggingSeverity::LS_INFO);
#else
        OnLogMessage(message, webrtc::LoggingSeverity::LS_VERBOSE);
#endif
    }

    void OnLogMessage(std::string_view msg, webrtc::LoggingSeverity severity,
                      const char *tag) override {
        log(severity) << [&](auto sink) { sink("[{}]: {}\n", tag, msg); };
    }

    void OnLogMessage(std::string_view message,
                      webrtc::LoggingSeverity severity) override {
        log(severity) << [&](auto sink) { sink(message); };
    }

    void OnLogMessage(std::string_view message) override {
#ifdef NDEBUG
        OnLogMessage(message, webrtc::LoggingSeverity::LS_INFO);
#else
        OnLogMessage(message, webrtc::LoggingSeverity::LS_VERBOSE);
#endif
    }

    void OnLogMessage(const webrtc::LogLineRef &line) override {
        log(line.severity()) << [&](auto sink) {
            sink("[{}][{}:{}]: {}\n", line.tag(), line.filename(), line.line(),
                 line.message());
        };
    }

    static constexpr samlog::log_level
    severity_to_level(webrtc::LoggingSeverity severity) {
        switch (severity) {
        case webrtc::LoggingSeverity::LS_VERBOSE:
            return samlog::log_level::debug;
        case webrtc::LoggingSeverity::LS_INFO:
            return samlog::log_level::info;
        case webrtc::LoggingSeverity::LS_WARNING:
            return samlog::log_level::warn;
        case webrtc::LoggingSeverity::LS_ERROR:
            return samlog::log_level::error;
        case webrtc::LoggingSeverity::LS_NONE:
            return samlog::log_level::off;
        }
    }
};

static webrtc::LoggingConfig get_dcsctp_log_config() {
    webrtc::LoggingConfig config;
    config.AddSink(std::make_unique<dcsctp_log_sink>());
    config.set_debug_severity(webrtc::LoggingSeverity::LS_WARNING);
#ifndef NDEBUG
    config.set_min_severity(webrtc::LoggingSeverity::LS_VERBOSE);
#endif
    return config;
}

static void init_dcsctp_logger() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (!webrtc::InitializeLogging(get_dcsctp_log_config()))
            throw std::runtime_error{"InitializeLogging failed"};
    });
}

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
    if (!sdp.fingerprints.empty())
        return std::format("{} {}", sdp.fingerprints[0].algorithm,
                           sdp.fingerprints[0].value);
    if (!sdp.medias.empty() && !sdp.medias[0].fingerprints.empty())
        return std::format("{} {}", sdp.medias[0].fingerprints[0].algorithm,
                           sdp.medias[0].fingerprints[0].value);
    return {};
}

static sdp_setup_role setup_from(const session_description &sdp) {
    if (sdp.setup)
        return *sdp.setup;
    if (!sdp.medias.empty() && sdp.medias[0].setup)
        return *sdp.medias[0].setup;
    return sdp_setup_role::passive;
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
    if (std::ranges::find(sdp.ice_options, "trickle") != sdp.ice_options.end())
        return true;
    return std::ranges::any_of(sdp.medias, [](const auto &m) {
        return std::ranges::find(m.ice_options, "trickle") !=
               m.ice_options.end();
    });
}

std::pair<std::vector<uint8_t>, uint16_t> connection_impl::make_rtp(
    rtp_sender &sender, rtp_stream &stream, const std::vector<uint8_t> &payload,
    uint32_t timestamp, uint16_t sequence_number, bool marker) {
    uint16_t twcc_seq = 0;
    std::vector<uint8_t> ext_data;
    auto tr = sender.transceiver();
    if (tr) {
        const auto &extmaps = tr->receiver()->parameters().header_extensions;
        const auto ext_id = [&](std::string_view uri) -> int {
            for (const auto &e : extmaps)
                if (e.uri == uri)
                    return e.id;
            return 0;
        };

        int mid_id = ext_id("urn:ietf:params:rtp-hdrext:sdes:mid");
        if (mid_id > 0 && mid_id < 15) {
            std::string mid = tr->mid();
            ext_data.push_back(static_cast<uint8_t>((mid_id << 4) | 0));
            ext_data.push_back(
                static_cast<uint8_t>(mid.empty() ? '0' : mid[0]));
        }

        int rid_id = ext_id("urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id");
        if (rid_id > 0 && rid_id < 15 && !stream.rid.empty()) {
            ext_data.push_back(static_cast<uint8_t>(
                (rid_id << 4) | (static_cast<int>(stream.rid.size()) - 1)));
            ext_data.insert(ext_data.end(), stream.rid.begin(),
                            stream.rid.end());
        }

        if (tr->kind() == media_kind::video) {
            int abs_id = ext_id("http://www.webrtc.org/experiments/"
                                "rtp-hdrext/abs-send-time");
            if (abs_id > 0 && abs_id < 15) {
                uint64_t ntp = sender._streams[0]->ntp_timestamp;
                uint32_t abs =
                    static_cast<uint32_t>(ntp ? (ntp >> 14) & 0xFFFFFF : 0);
                ext_data.push_back(static_cast<uint8_t>((abs_id << 4) | 2));
                ext_data.push_back(static_cast<uint8_t>((abs >> 16) & 0xFF));
                ext_data.push_back(static_cast<uint8_t>((abs >> 8) & 0xFF));
                ext_data.push_back(static_cast<uint8_t>(abs & 0xFF));
            }
        }

        int tcc_id = ext_id("http://www.ietf.org/id/"
                            "draft-holmer-rmcat-transport-wide-"
                            "cc-extensions-01");
        if (tcc_id > 0 && tcc_id < 15) {
            twcc_seq = ++this->_transport_wide_seq;
            ext_data.push_back(static_cast<uint8_t>((tcc_id << 4) | 1));
            ext_data.push_back(static_cast<uint8_t>((twcc_seq >> 8) & 0xFF));
            ext_data.push_back(static_cast<uint8_t>(twcc_seq & 0xFF));
        }

        while (ext_data.size() % 4)
            ext_data.push_back(0);
    }

    uint8_t pt = 0;
    if (auto tr = sender.transceiver(); tr)
        pt = tr->payload_type();
    rtp::rtp_packet pkt;
    pkt.payload_type = pt;
    pkt.marker = marker ? 1 : 0;
    pkt.sequence_number = sequence_number;
    pkt.timestamp = timestamp;
    pkt.ssrc = stream.ssrc;
    pkt.payload = payload;
    if (!ext_data.empty()) {
        pkt.extension = 1;
        pkt.extension_profile = 0xBEDE;
        pkt.extension_data = std::move(ext_data);
    }
    std::vector<uint8_t> buf(pkt.serialized_size());
    pkt.write_to(buf.data(), buf.size());
    return {std::move(buf), twcc_seq};
}

void connection_impl::update_send_stats(rtp_stream &stream,
                                        const std::vector<uint8_t> &payload,
                                        std::size_t rtp_buf_size,
                                        uint16_t twcc_seq, uint32_t timestamp) {
    this->_tx_packets++;
    this->_tx_bytes += rtp_buf_size;
    if (twcc_seq) {
        this->_twcc_sent[twcc_seq % connection_impl::TWCC_SENT_SIZE] =
            connection_impl::_twcc_sent_entry{
                .transport_seq = twcc_seq,
                .ssrc = stream.ssrc,
                .size = rtp_buf_size,
                .send_time = std::chrono::steady_clock::now()};
    }

    stream.octet_count += static_cast<uint32_t>(payload.size());
    stream.packet_count++;
    auto now = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  now.time_since_epoch())
                  .count();
    stream.ntp_timestamp = static_cast<uint64_t>(us) * 65536 / 1000000;
    stream.rtp_timestamp = timestamp;
}

asiortc::task<void>
connection_impl::_sender_send_loop(std::shared_ptr<rtp_sender> sender,
                                   std::shared_ptr<srtp_transport_type> srtp) {
    asioice::utils::scope_guard on_exit([]() noexcept {
        SAMLOG_INFO(auto sink) { sink("_sender_send_loop exited\n"); };
    });

    auto track = sender->track();
    if (!track)
        co_return;

    auto packetizer = sender->packetizer();
    if (!packetizer)
        co_return;

    {
        auto tr = sender->transceiver();
        SAMLOG_INFO(auto sink) {
            sink("_sender_send_loop started: kind={} mid={} pt={}\n",
                 track->kind() == media_kind::video ? "video" : "audio",
                 tr ? tr->mid() : "?", tr ? tr->payload_type() : 0);
        };
    }

    while (!sender->stopped()) {
        std::vector<encode_target> layers;
        auto tr = sender->transceiver();
        if (tr && !tr->send_encodings.empty()) {
            for (const auto &enc : tr->send_encodings) {
                encode_target t;
                t.max_bitrate =
                    enc.max_bitrate
                        ? std::optional<int>(static_cast<int>(*enc.max_bitrate))
                        : std::nullopt;
                t.rid = enc.rid;
                layers.push_back(std::move(t));
            }
        } else {
            layers.push_back({});
        }

        auto frames = co_await track->recv(layers);
        if (frames.empty())
            co_return;

        for (size_t si = 0; si < frames.size() && si < sender->_streams.size();
             ++si) {
            auto &frame = frames[si];
            if (frame.data.empty())
                continue;
            auto [payloads, ts] = packetizer->pack(frame.data, frame.timestamp);

            auto &stream = sender->_streams[si];
            for (size_t i = 0; i < payloads.size(); ++i) {
                uint16_t seq = ++stream->seq;
                auto [rtp_buf, twcc_seq] =
                    make_rtp(*sender, *stream, payloads[i], ts, seq,
                             i == payloads.size() - 1);

                stream->history[seq % rtp_stream::HISTORY_SIZE] =
                    rtp_stream::history_entry{
                        .payload = payloads[i], .seq = seq, .timestamp = ts};

                auto r = co_await srtp->send_rtp(rtp_buf);
                if (std::get<0>(r))
                    co_return;

                update_send_stats(*stream, payloads[i], rtp_buf.size(),
                                  twcc_seq, ts);
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
      _agent{_executor, get_agent_config(std::move(cfg))} {
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
            SAMLOG_INFO(auto sink) {
                sink("connection_impl::apply_descriptions add candidate: {}\n",
                     line);
            };
            scope.spawn(
                _agent.add_remote_candidate(std::move(*c)) |
                stdexec::then([&line](auto success) noexcept {
                    if (!success) {
                        SAMLOG_WARN(auto sink) {
                            sink("_agent.add_remote_candidate failed: {}\n",
                                 line);
                        };
                    } else {
                        SAMLOG_INFO(auto sink) {
                            sink("_agent.add_remote_candidate success: {}\n",
                                 line);
                        };
                    }
                }));
        } else {
            SAMLOG_WARN(auto sink) {
                sink("connection_impl::apply_descriptions parse candidate line "
                     "failed: {}\n",
                     line);
            };
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
            this->_agent.gather_candidates() | stdexec::then([this] {
                this->_gathering_state = ice_gathering_state_t::complete;
            })),
        _scope.get_token());
}

void connection_impl::do_on_candidates(std::span<const asioice::candidate> cc) {
    if (_local_desc) {
        for (const auto &c : cc)
            _local_desc->candidates.push_back(c.to_sdp());
    }
    if (cc.empty() && _gathering_state == ice_gathering_state_t::gathering)
        _gathering_state = ice_gathering_state_t::complete;
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
    if ((asioice::utils::nceq(codec_name, "VP8") ||
         asioice::utils::nceq(codec_name, "VP9")) &&
        !payload.empty()) {
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
    } else if (asioice::utils::nceq(codec_name, "H264") && !payload.empty()) {
        // H264: nothing to strip at the packet level;
        // individual NAL units are self-contained.
        // FU-A/STAP-A reassembly is handled in H264DecoderImpl.
    }
    return payload;
}

void connection_impl::build_ssrc_map(rtp_transceiver &tr, const sdp_media &rm) {
    if (rm.port == 0 || rm.direction == sdp_direction::recvonly)
        return;
    for (const auto &rtpmap : rm.rtpmaps()) {
        if (!asioice::utils::nceq("rtx", rtpmap.name))
            continue;
        auto apt = rtpmap.find_param("apt");
        if (!apt || apt->empty())
            continue;
        uint8_t pt = 0;
        if (auto res =
                std::from_chars(apt->data(), apt->data() + apt->size(), pt);
            res.ec != std::errc{})
            continue;
        _rtx_pt_to_pt[rtpmap.payload_type] = pt;
        SAMLOG_TRACE(auto sink) {
            sink("create RTX payload type map: {} -> {}\n",
                 (int)rtpmap.payload_type, (int)pt);
        };
    }

    boost::container::flat_set<uint32_t> repair_ssrcs;
    for (const auto &g : rm.ssrc_groups) {
        if (g.semantics != "FID" || g.ssrcs.size() != 2)
            continue;
        _rtx_ssrc_to_ssrc[g.ssrcs[1]] = g.ssrcs[0];
        repair_ssrcs.insert(g.ssrcs[1]);
        SAMLOG_TRACE(auto sink) {
            sink("create RTX ssrc map: {} -> {}\n", g.ssrcs[1], g.ssrcs[0]);
        };
    }
    for (const auto &g : rm.ssrc_groups) {
        if (g.semantics != "FEC" || g.ssrcs.size() != 2)
            continue;
        repair_ssrcs.insert(g.ssrcs[1]);
    }

    for (const auto &ssrc : rm.ssrcs) {
        if (repair_ssrcs.contains(ssrc.ssrc))
            continue;
        tr.receiver()->create_ssrc_context(ssrc.ssrc, _ssrc_set);
    }
}

asiortc::task<void> connection_impl::do_connect() {
    _connection_state = connection_state_t::connecting;
    asioice::utils::scope_guard on_exit([this]() noexcept {
        if (_connection_state == connection_state_t::connecting)
            _connection_state = connection_state_t::failed;
    });
    if (!this->_agent.config().trickle_ice && this->_gathering_task) {
        co_await std::move(*this->_gathering_task);
        this->_gathering_task.reset();
    }
    if (!co_await _agent.connect()) {
        SAMLOG_INFO(auto sink) { sink("ICE connect failed\n"); };
        co_return;
    }
    _ice_send_loop =
        stdexec::spawn_future(this->ice_send_loop(), _scope.get_token());
    _periodic_cleaning_task = stdexec::spawn_future(
        this->periodic_cleaning_loop(), _scope.get_token());

    auto setup = setup_from(*_remote_desc);
    bool we_are_active = (setup != sdp_setup_role::active);
    auto dtls_role = we_are_active
                         ? dtls_transport_type::handshake_type::client
                         : dtls_transport_type::handshake_type::server;
    auto hs_ec = co_await _dtls_transport->async_handshake(dtls_role);
    if (hs_ec) {
        SAMLOG_INFO(auto sink) {
            sink("DTLS handshake failed: {}\n", hs_ec.message());
        };
        co_return;
    }

    bool needs_srtp = false;
    bool needs_sctp = _need_sctp;
    if (_remote_desc) {
        for (auto &m : _remote_desc->medias) {
            if (m.media_type == sdp_media_type::audio ||
                m.media_type == sdp_media_type::video)
                needs_srtp = true;
            else if (m.media_type == sdp_media_type::application)
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

    init_dcsctp_logger();
    _sctp_transport = std::make_shared<sctp_transport_type>(_dtls_transport);
    _sctp_transport->start();

    bool sctp_ok = false;
    if (we_are_active) {
        sctp_ok = co_await _sctp_transport->accept();
    } else {
        sctp_ok = co_await _sctp_transport->connect();
    }
    if (!sctp_ok) {
        SAMLOG_INFO(auto sink) { sink("SCTP setup failed\n"); };
        co_return;
    }

    _data_channel_manager.emplace(_sctp_transport, we_are_active);
    _data_channel_manager->on_remote_channel(
        std::bind_front(&connection_impl::do_on_data_channel, this));
    _data_channel_manager->start();
    _connection_state = connection_state_t::connected;
}

asiortc::task<void> connection_impl::set_local_description(
    std::unique_ptr<session_description> desc_ptr) {
    const auto &desc = *desc_ptr;
    switch (_signaling_state.get()) {
    case signaling_state_t::have_local_offer:
    case signaling_state_t::have_local_pranswer:
    case signaling_state_t::have_remote_pranswer:
        throw std::logic_error{"set_local_description: invalid state: " +
                               to_string(_signaling_state.get())};
    case signaling_state_t::stable:
    case signaling_state_t::closed:
        if (desc.sdp_type != "offer")
            throw std::logic_error{
                "set_local_description: desc.sdp_type != \"offer\""};
        break;
    case signaling_state_t::have_remote_offer:
        if (desc.sdp_type != "answer")
            throw std::logic_error{
                "set_local_description: desc.sdp_type != \"answer\""};
        break;
    }
    bool is_offer = (desc.sdp_type == "offer");
    if (is_offer) {
        for (const auto &tr : _transceivers) {
            auto mline_index = tr->mline_index();
            if (!mline_index.has_value())
                continue;
            if (desc.medias.size() <= *mline_index) {
                auto msg = std::format("Invalid mline {}\n", *mline_index);
                SAMLOG_ERROR(auto sink) { sink(msg); };
                throw std::runtime_error{msg};
            }
            const auto &m = desc.medias[*mline_index];
            if (!tr->mid().empty() && tr->mid() != m.mid) {
                auto msg =
                    std::format("MID mismatch {} != {}\n", tr->mid(), m.mid);
                SAMLOG_ERROR(auto sink) { sink(msg); };
                throw std::runtime_error{msg};
            }
        }
        for (auto &tr : _transceivers) {
            if (tr->mline_index() && tr->mid().empty())
                tr->set_mid(desc.medias[*tr->mline_index()].mid);
        }
        _local_desc = std::move(desc_ptr);
        _signaling_state = signaling_state_t::have_local_offer;
    } else {
        _local_desc = std::move(desc_ptr);
        _signaling_state = signaling_state_t::have_local_pranswer;
    }
    _agent.config().ice_controlling = is_offer;

    co_await apply_descriptions();
    start_gathering();
    start_connecting();
}

asiortc::task<void> connection_impl::set_remote_description(
    std::unique_ptr<session_description> desc_ptr) {
    const auto &desc = *desc_ptr;
    switch (_signaling_state.get()) {
    case signaling_state_t::have_remote_offer:
    case signaling_state_t::have_local_pranswer:
    case signaling_state_t::have_remote_pranswer:
        throw std::logic_error{"set_remote_description: invalid state: " +
                               to_string(_signaling_state.get())};
    case signaling_state_t::stable:
    case signaling_state_t::closed:
        if (desc.sdp_type != "offer")
            throw std::logic_error{
                "set_remote_description: desc.sdp_type != \"offer\""};
        break;
    case signaling_state_t::have_local_offer:
        if (desc.sdp_type != "answer")
            throw std::logic_error{
                "set_remote_description: desc.sdp_type != \"answer\""};
        break;
    }
    bool is_offer = (desc.sdp_type == "offer");
    if (is_offer) {
        auto bundle = std::ranges::find_if(
            desc.groups, [](const auto &g) { return g.semantic == "BUNDLE"; });
        if (bundle == desc.groups.end())
            throw std::logic_error{"bundle policy not supported: "};
        for (const auto &rm : desc.medias)
            if (rm.media_type == sdp_media_type::application &&
                std::ranges::find(bundle->items, rm.mid) == bundle->items.end())
                throw std::logic_error{
                    "balanced/max-compat bundle policy not supported: "
                    "media section mid=" +
                    rm.mid + " not in BUNDLE group"};
        for (const auto &rm : desc.medias) {
            if (rm.media_type == sdp_media_type::application)
                continue;
            media_kind rm_kind = media_kind::video;
            if (rm.media_type == sdp_media_type::audio)
                rm_kind = media_kind::audio;
            else if (rm.media_type != sdp_media_type::video)
                continue;
            if (rm.mid.empty())
                throw std::invalid_argument{"rm.mid == \"\""};
            std::shared_ptr<rtp_transceiver> tr = nullptr;
            // TODO: consider direction
            auto it = std::find_if(
                _transceivers.begin(), _transceivers.end(), [&](const auto &t) {
                    return !t->mid().empty()
                               ? false
                               : (!t->stopped() && t->kind() == rm_kind);
                });
            if (it == _transceivers.end())
                continue;
            tr = *it;
            tr->set_mid(rm.mid);
            tr->from_remote_offer(rm);
            if (tr->direction() == sdp_direction::inactive)
                continue;
            if (tr->direction() == sdp_direction::sendrecv ||
                tr->direction() == sdp_direction::recvonly) {
                build_ssrc_map(*tr, rm);
                auto receiver = tr->receiver();
                auto track = receiver->track();
                if (_on_track_cb) {
                    std::vector<std::string> stream_ids;
                    stream_ids.reserve(rm.msids.size());
                    for (const auto &m : rm.msids)
                        stream_ids.push_back(m.stream_id);
                    _on_track_cb(std::move(receiver), std::move(track),
                                 std::move(stream_ids), std::move(tr));
                }
            }
            auto ext_it = std::ranges::find_if(rm.extmaps, [](const auto &ex) {
                return ex.uri == "urn:ietf:params:rtp-hdrext:sdes:mid";
            });
            if (ext_it != rm.extmaps.end())
                _mid_ext_id = ext_it->id;
        }
        _remote_desc = std::move(desc_ptr);
        _signaling_state = signaling_state_t::have_remote_offer;
    } else {
        for (auto &tr : _transceivers) {
            auto it = std::ranges::find_if(desc.medias, [&](const auto &rm) {
                return rm.mid == tr->mid();
            });
            if (it == desc.medias.end()) {
                tr->set_direction(sdp_direction::inactive);
                tr->stop();
                continue;
            }
            const auto &rm = *it;
            tr->from_remote_answer(rm);
            if (tr->direction() == sdp_direction::inactive) {
                tr->stop();
                continue;
            }

            if (tr->direction() == sdp_direction::sendrecv ||
                tr->direction() == sdp_direction::recvonly) {
                build_ssrc_map(*tr, rm);
                auto receiver = tr->receiver();
                auto track = receiver->track();
                if (_on_track_cb) {
                    std::vector<std::string> stream_ids;
                    stream_ids.reserve(rm.msids.size());
                    for (const auto &m : rm.msids)
                        stream_ids.push_back(m.stream_id);
                    _on_track_cb(std::move(receiver), std::move(track),
                                 std::move(stream_ids), tr);
                }
            }
        }
        _remote_desc = std::move(desc_ptr);
        _signaling_state = signaling_state_t::have_remote_pranswer;
    }
    _agent.config().ice_controlling = !is_offer;

    co_await apply_descriptions();
    start_connecting();
}

void connection_impl::on_track(connection_impl::on_track_cb cb) {
    _on_track_cb = std::move(cb);
}

asiortc::task<std::unique_ptr<session_description_interface>>
connection_impl::create_offer() {
    struct media_entry {
        const rtp_transceiver *tr = nullptr; // 描述 sctp 时为空
        std::size_t index;                   // 在 SDP 中的 m-line 索引
        std::string mid;                     // 分配的 MID
        bool is_sctp = false;
    };

    std::vector<media_entry> media_entries;

    const session_description *current_desc =
        _local_desc ? _local_desc.get() : (const session_description *)nullptr;

    boost::container::flat_map<std::string, std::size_t> mid_to_index;
    boost::container::flat_set<std::size_t> recyclable_indices;
    std::optional<std::size_t> sctp_index;

    if (current_desc) {
        for (std::size_t i = 0; i < current_desc->medias.size(); ++i) {
            const auto &m = current_desc->medias[i];
            if (m.mid.empty())
                continue;

            mid_to_index[m.mid] = i;

            if (m.media_type == sdp_media_type::application) {
                sctp_index = i;
                continue;
            }

            if (m.port == 0) {
                recyclable_indices.insert(i);
            }
        }
    }

    // 处理现有的 transceivers
    media_entries.reserve(_transceivers.size());
    for (const auto &tr : _transceivers) {
        if (tr->mid().empty())
            continue; // 稍后处理新的

        auto it = mid_to_index.find(tr->mid());
        if (it != mid_to_index.end()) {
            std::size_t index = it->second;
            if (index == sctp_index) {
                // SDP 声明是 data channel，实际不是
                continue;
            }
            media_entries.push_back({tr.get(), index, tr->mid()});

            recyclable_indices.erase(index);
            mid_to_index.erase(it);
        } else {
            // TODO:
        }
    }

    // 下一个追加位置的起点
    std::size_t next_append_index =
        current_desc ? current_desc->medias.size() : 0;

    // 获取一个可用的索引
    const auto get_index = [&]() -> std::size_t {
        if (!recyclable_indices.empty()) {
            auto idx = *recyclable_indices.begin();
            recyclable_indices.erase(recyclable_indices.begin());
            mid_to_index.erase(current_desc->medias[idx].mid);
            return idx;
        }
        return next_append_index++;
    };

    // 处理 SCTP
    const bool sctp_active = _need_sctp;
    if (sctp_index.has_value()) {
        // 已存在 SCTP 行
        const std::string &old_mid = current_desc->medias[*sctp_index].mid;
        media_entries.push_back({nullptr, *sctp_index, old_mid, true});
    } else if (sctp_active) {
        media_entries.push_back({nullptr, get_index(), "", true});
    }

    // 处理新的 RTP Transceivers
    for (const auto &tr : _transceivers) {
        if (!tr->mid().empty())
            continue; // 已处理
        if (tr->stopped())
            continue; // 新的但已停止
        media_entries.push_back({tr.get(), get_index(), ""});
    }

    // 填补 index 空隙
    for (const auto &[mid, idx] : mid_to_index)
        media_entries.push_back({nullptr, idx, mid});

    std::ranges::sort(media_entries, [](const auto &a, const auto &b) {
        return a.index < b.index;
    });

    auto fp = _cert.get_fingerprint(asioice::ssl::hash_algorithm::sha256);
    auto offer_ptr = std::make_unique<session_description>();
    auto &offer = *offer_ptr;
    offer.sdp_type = "offer";

    offer.origin.session_version = _session_version++;
    if (current_desc) {
        offer.session_name = current_desc->session_name;
        // offer.timing = current_desc->timing;
        // offer.origin = current_desc->origin;
    } else {
        offer.version = 0;
        offer.origin = sdp_origin{};
        offer.session_name = "-";
        // offer.timing.start = 0;
        // offer.timing.stop = 0;
    }

    offer.ice_ufrag = _agent.local_username();
    offer.ice_pwd = _agent.local_password();
    offer.fingerprints.emplace_back(fp.hash_name(), fp.value);
    offer.setup = sdp_setup_role::actpass;

    boost::container::flat_set<std::string> used_mids;
    for (const auto &entry : media_entries) {
        if (!entry.mid.empty())
            used_mids.insert(entry.mid);
    }

    const auto next_mid = [&used_mids, this]() {
        while (true) {
            auto mid = std::to_string(_mid_counter);
            ++_mid_counter;
            if (used_mids.find(mid) == used_mids.end()) {
                used_mids.insert(mid);
                return mid;
            }
        }
    };

    for (auto &entry : media_entries) {
        if (entry.mid.empty())
            entry.mid = next_mid();
    }

    {
        // 构建 sync group
        boost::container::flat_map<std::string, std::vector<std::string>>
            sync_group;
        for (const auto &entry : media_entries) {
            if (entry.tr == nullptr)
                continue;
            for (const auto &msid : entry.tr->sender()->msids()) {
                auto it = sync_group.lower_bound(msid);
                if (it == sync_group.end() || it->first != msid) {
                    sync_group.emplace_hint(
                        it, msid, std::vector<std::string>{entry.mid});
                } else {
                    it->second.push_back(entry.mid);
                }
            }
        }

        for (auto &g : sync_group) {
            if (g.second.size() > 1)
                offer.groups.emplace_back("LS", std::move(g.second));
        }
    }

    for (const auto &entry : media_entries) {
        const auto &mid = entry.mid;

        if (entry.is_sctp) {
            sdp_media app;
            app.mid = mid;
            app.media_type = sdp_media_type::application;
            app.port = sctp_active ? 9 : 0;
            app.proto = sdp_proto::UDP_DTLS_SCTP;
            app.conn_nettype = "IN";
            app.conn_addrtype = "IP4";
            app.conn_addr = "0.0.0.0";
            app.direction = sdp_direction::sendrecv;
            // app.sctpmap = "webrtc-datachannel";
            app.sctp_port = 5000;
            app.add_format("webrtc-datachannel");

            app.ice_ufrag = offer.ice_ufrag;
            app.ice_pwd = offer.ice_pwd;
            app.fingerprints = offer.fingerprints;
            app.setup = offer.setup;

            offer.medias.push_back(std::move(app));
        } else {
            auto t = entry.tr;
            if (t == nullptr) {
                assert(current_desc);
                const auto &rm = current_desc->medias[entry.index];
                sdp_media m = rm;
                m.port = 0;
                m.direction = sdp_direction::inactive;
                offer.medias.push_back(std::move(m));
                continue;
            }
            auto media = t->to_offer_sdp_media(mid);

            media.ice_ufrag = offer.ice_ufrag;
            media.ice_pwd = offer.ice_pwd;
            media.fingerprints = offer.fingerprints;
            media.setup = offer.setup;
            offer.medias.push_back(std::move(media));
            t->set_mline_index(offer.medias.size() - 1);
        }
    }

    std::vector<std::string> mids;
    mids.reserve(offer.medias.size());
    for (const auto &media : offer.medias)
        if (media.port != 0)
            mids.push_back(media.mid);

    if (!mids.empty())
        offer.groups.emplace_back("BUNDLE", std::move(mids));

    if (!offer.medias.empty()) {
        offer.msid_semantic = sdp_msid_semantic{"WMS", {"*"}};
    }

    offer.ice_options.emplace_back("trickle");

    co_return std::unique_ptr<session_description_interface>(
        std::move(offer_ptr));
}

asiortc::task<std::unique_ptr<session_description_interface>>
connection_impl::create_answer() {
    if (!_remote_desc ||
        _signaling_state != signaling_state_t::have_remote_offer) {
        throw std::runtime_error("set_remote_description must be called first");
    }

    auto fp = _cert.get_fingerprint(asioice::ssl::hash_algorithm::sha256);
    const auto remote_setup = setup_from(*_remote_desc);
    const bool we_are_active = (remote_setup != sdp_setup_role::active);

    auto answer_ptr = std::make_unique<session_description>();
    auto &answer = *answer_ptr;
    answer.sdp_type = "answer";
    answer.version = 0;
    answer.origin = sdp_origin{};
    answer.session_name = "-";
    answer.ice_ufrag = _agent.local_username();
    answer.ice_pwd = _agent.local_password();
    answer.fingerprints.emplace_back(fp.hash_name(), fp.value);
    answer.setup =
        we_are_active ? sdp_setup_role::active : sdp_setup_role::passive;

    for (const auto &rm : _remote_desc->medias) {
        if (rm.media_type == sdp_media_type::application) {
            sdp_media answer_media;
            answer_media.mid = rm.mid;
            answer_media.media_type = rm.media_type;
            answer_media.port = 9;
            answer_media.proto = rm.proto;
            answer_media.add_format(rm.formats());
            answer_media.conn_nettype = "IN";
            answer_media.conn_addrtype = "IP4";
            answer_media.conn_addr = "0.0.0.0";
            answer_media.direction = rm.direction;
            // answer_media.sctpmap = rm.sctpmap;
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
        answer.groups.emplace_back("BUNDLE", std::move(answer_bundle));

    if (ice_options_trickle_from(*_remote_desc))
        answer.ice_options.emplace_back("trickle");

    answer.msid_semantic = sdp_msid_semantic{"WMS", {"*"}};

    co_return std::unique_ptr<session_description_interface>{
        std::move(answer_ptr)};
}

std::shared_ptr<asiortc::data_channel>
connection_impl::create_data_channel(std::string label,
                                     asiortc::data_channel::options options) {
    if (_roles_set && !_sctp_transport)
        throw std::runtime_error{
            "No SCTP transport: create_data_channel must be called before "
            "set_local_description or set_remote_description"};

    _need_sctp = true;
    return std::make_shared<asiortc::data_channel>(
        weak_from_this(), std::move(label), std::move(options));
}

static sdp_rtpmap make_codec(media_format fmt) {
    if (fmt == media_format::h264)
        return {102, "H264", 90000};
    if (fmt == media_format::vp9)
        return {98, "VP9", 90000};
    if (fmt == media_format::vp8)
        return {96, "VP8", 90000};
    if (fmt == media_format::opus)
        return {111, "opus", 48000};
    std::unreachable();
}

std::shared_ptr<rtp_transceiver>
connection_impl::add_transceiver(media_description desc,
                                 rtp_transceiver_init init) {
    if (desc.format == media_format::unknown)
        throw std::invalid_argument(
            "add_transceiver: description format cannot be unknown");
    if (init.direction == sdp_direction::inactive)
        throw std::invalid_argument{
            "add_transceiver: init.direction == sdp_direction::inactive"};
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFF);

    auto t = std::make_shared<rtp_transceiver>(desc, weak_from_this());
    t->wire_back_references();

    t->set_direction(init.direction);
    t->sender()->set_msids(std::move(init.streams));
    t->send_encodings = std::move(init.send_encodings);
    t->_codec = sdp_rtpmap::from_media_description(desc);

    auto fmt = desc.format;
    if (fmt == media_format::h264) {
        t->sender()->_packetizer =
            std::make_unique<rtp_packetizer::H264Packetizer>();
    } else if (fmt == media_format::vp9) {
        t->sender()->_packetizer =
            std::make_unique<rtp_packetizer::Vp9Packetizer>();
    } else if (fmt == media_format::vp8) {
        t->sender()->_packetizer =
            std::make_unique<rtp_packetizer::Vp8Packetizer>();
    } else if (fmt == media_format::opus) {
        t->sender()->_packetizer =
            std::make_unique<rtp_packetizer::OpusPacketizer>();
    }

    auto rtx_pt = static_cast<uint8_t>(t->_codec.payload_type + 1);
    const auto &encs = t->send_encodings;
    auto &streams = t->sender()->_streams;

    if (encs.empty()) {
        auto s = std::make_shared<rtp_stream>();
        s->ssrc = dist(gen);
        if (t->kind() == media_kind::video) {
            s->rtx_ssrc = dist(gen);
            s->rtx_pt = rtx_pt;
        }
        streams.push_back(std::move(s));
    } else {
        for (size_t i = 0; i < encs.size(); ++i) {
            auto s = std::make_shared<rtp_stream>();
            s->ssrc = dist(gen);
            s->rid = encs[i].rid;
            if (t->kind() == media_kind::video) {
                s->rtx_ssrc = dist(gen);
                s->rtx_pt = rtx_pt + static_cast<uint8_t>(2 * i);
            }
            streams.push_back(std::move(s));
        }
    }
    t->receiver()->set_rtcp_ssrc(streams.front()->ssrc);

    _transceivers.push_back(t);
    return t;
}

std::shared_ptr<rtp_transceiver>
connection_impl::add_transceiver(std::shared_ptr<media_track> track,
                                 rtp_transceiver_init init) {
    auto t = this->add_transceiver(track->description(), std::move(init));
    t->sender()->set_track(std::move(track));
    return t;
}

std::shared_ptr<rtp_sender>
connection_impl::add_track(std::shared_ptr<media_track> track,
                           std::vector<std::string> streams) {
    auto it = std::ranges::find_if(_transceivers, [&](const auto &tr) {
        return !tr->stopped() && tr->kind() == track->kind() &&
               tr->sender()->track() == nullptr;
    });
    if (it == _transceivers.end()) {
        return add_transceiver(std::move(track),
                               {.streams = std::move(streams)})
            ->sender();
    }

    auto &tr = **it;
    tr.sender()->set_track(std::move(track));
    tr.sender()->set_msids(std::move(streams));
    tr.set_direction([&] {
        switch (tr.direction()) {
        case sdp_direction::sendrecv:
            return sdp_direction::sendrecv;
        case sdp_direction::sendonly:
            return sdp_direction::sendonly;
        case sdp_direction::recvonly:
            return sdp_direction::sendrecv;
        case sdp_direction::inactive:
            return sdp_direction::sendonly;
        }
        std::unreachable();
    }());
    return tr.sender();
}

asiortc::task<void> connection_impl::_sender_rtcp_loop(
    std::shared_ptr<rtp_sender> sender,
    std::shared_ptr<connection_impl::srtp_transport_type> srtp) {
    asioice::utils::scope_guard on_exit([]() noexcept {
        SAMLOG_INFO(auto sink) { sink("_sender_rtcp_loop exited\n"); };
    });
    static thread_local std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dist(0.5, 1.5);
    std::vector<uint8_t> compound;

    net::steady_timer timer(this->get_executor());
    while (!sender->stopped()) {
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
        SAMLOG_INFO(auto sink) { sink("_receiver_rtcp_loop exited\n"); };
    });
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dist(0.5, 1.5);

    net::steady_timer timer(this->get_executor());

    auto track = receiver->track();
    if (!track)
        co_return;
    while (!receiver->stopped()) {
        auto delay =
            std::chrono::milliseconds(static_cast<int>(dist(gen) * 1000));
        timer.expires_after(delay);
        auto ec = co_await timer.async_wait(asioice::utils::use_sender);
        if (ec)
            co_return;

        // Build RR for SSRCs belonging to this receiver
        std::vector<uint8_t> compound;
        for (auto &ctx : this->_ssrc_set) {
            if (&ctx.receiver() != receiver.get())
                continue;
            if (ctx.packets_received_count() == 0)
                continue;

            uint32_t extended_max = ctx.extended_max();
            uint64_t expected_interval =
                ctx.packets_expected_count() - ctx.expected_prior();
            uint64_t received_interval =
                ctx.packets_received_count() - ctx.received_prior();
            uint8_t fraction = ctx.fraction_lost();
            int64_t cumulative_lost =
                static_cast<int64_t>(ctx.packets_expected_count()) -
                static_cast<int64_t>(ctx.packets_received_count());
            if (cumulative_lost < 0)
                cumulative_lost = 0;
            if (cumulative_lost > 0x7FFFFF)
                cumulative_lost = 0x7FFFFF;

            uint32_t jitter = static_cast<uint32_t>(ctx.jitter_q4() >> 4);
            uint32_t lsr =
                static_cast<uint32_t>((ctx.lsr() >> 16) & 0xFFFFFFFF);
            uint32_t dlsr = 0;
            if (ctx.lsr() != 0) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - ctx.lsr_time())
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
            rb.ssrc = ctx.ssrc();
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

            ctx.advance_prior();
        }

        if (compound.empty())
            continue;

        auto sdes = rtcp::sdes_chunk{0, "asiortc"}.bytes();
        compound.insert(compound.end(), sdes.begin(), sdes.end());

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
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch())
                          .count();
        std::vector<uint16_t> nacks;
        for (auto &ctx : _ssrc_set) {
            ctx.nack_gen().get_nacks(now_ms, nacks);
            if (!nacks.empty()) {
                auto nackfb = rtcp::rtcp_rtpfb{rtcp::packet_type::RTPFB_NACK,
                                               ctx.receiver().rtcp_ssrc(),
                                               ctx.ssrc(), std::move(nacks)};
                auto nack_bytes = nackfb.bytes();
                bool sent = this->sync_send_rtcp(nack_bytes);
                SAMLOG_TRACE(auto sink) {
                    sink("sent rtcp nack feedback: {}, {} items, {} bytes\n",
                         sent ? "success" : "failed", nackfb.lost.size(),
                         nack_bytes.size());
                };
            }
        }

        if (!_transceivers.empty()) {
            const auto &sndr = _transceivers.front()->_sender;
            if (sndr && !sndr->_streams.empty()) {
                _twcc.set_sender_ssrc(sndr->_streams.front()->ssrc);
            }
        }
        auto twcc_bytes = this->_twcc.report();
        if (!twcc_bytes.empty()) {
            bool sent = this->sync_send_rtcp(twcc_bytes);
            if (!sent)
                SAMLOG_TRACE(auto sink) {
                    sink("sent rtcp twcc feedback failed, {} bytes\n",
                         twcc_bytes.size());
                };
        }
    }
}

void connection_impl::_start_nack_loop() {
    if (!_srtp_transport)
        return;
    if (std::ranges::none_of(_transceivers, [](const auto &tr) {
            return std::to_underlying(tr->direction()) &
                   std::to_underlying(sdp_direction::recvonly);
        }))
        return;
    _nack_loop_task = stdexec::spawn_future(
        stdexec::starts_on(stdexec::inline_scheduler{}, _nack_loop()),
        _scope.get_token());
}

void connection_impl::rewrite_rtp_packet(std::span<uint8_t> data,
                                         const rtp_sender &sender) noexcept {
    if (data.size() < 12 || sender._streams.empty())
        return;
    auto &st = sender._streams[0];
    asioice::binary::write_big<uint16_t>(data.data() + 2, ++st->seq);
    asioice::binary::write_big<uint32_t>(data.data() + 8, st->ssrc);
}

std::span<uint8_t>
connection_impl::encrypt_rtp(std::span<const uint8_t> data,
                             std::span<uint8_t> buf) noexcept {
    if (!_srtp_transport)
        return {};
    return _srtp_transport->protect_rtp(data, buf);
}

void connection_impl::update_sender_status_after_send_rtp(
    std::size_t octet, std::size_t encrypted,
    const rtp_sender &sender) noexcept {
    _tx_packets++;
    _tx_bytes += encrypted;
    if (!sender._streams.empty()) {
        auto &st = sender._streams[0];
        st->packet_count++;
        st->octet_count += static_cast<uint32_t>(octet);
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

        sender->set_connection(weak_from_this());

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
    _ice_send_loop.reset();
    _periodic_cleaning_task.reset();
    while (!_ssrc_set.empty()) {
        auto it = _ssrc_set.begin();
        it->unlink();
    }
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
    _nack_loop_task.reset();
    _mid_ext_id.reset();

    _local_desc.reset();
    _remote_desc.reset();
    _pending_local_desc.reset();
    _pending_remote_desc.reset();
    _gathering_task.reset();
    _connecting_task.reset();

    _on_remote_channel_cb = nullptr;
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
        auto ch = std::make_shared<asiortc::data_channel>(
            this->weak_from_this(), p->label(), p->options());
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
    for (auto &ctx : _ssrc_set) {
        auto is = std::make_shared<rtc_inbound_rtp_stream_stats>();
        is->timestamp = now;
        is->type = "inbound-rtp";
        auto ssrc = ctx.ssrc();
        is->id = "inbound-rtp_" + std::to_string(ssrc);
        is->ssrc = ssrc;
        is->kind = (ctx.receiver().track() &&
                    ctx.receiver().track()->kind() == media_kind::video)
                       ? "video"
                       : "audio";
        is->transport_id = _transport_stats_id;
        is->packets_received = ctx.packets_received_count();
        is->packets_lost = static_cast<int64_t>(ctx.packets_expected_count()) -
                           ctx.packets_received_count();
        is->jitter = static_cast<uint32_t>(ctx.jitter_q4() >> 4);
        report[is->id] = is;
    }

    // Remote outbound stats (from RTCP SR)
    for (auto &[ssrc, ro] : _remote_outbound_stats) {
        auto s = std::make_shared<rtc_remote_outbound_rtp_stream_stats>(ro);
        s->kind = "audio";
        auto ctx_it = _ssrc_set.find(ssrc);
        if (ctx_it != _ssrc_set.end()) {
            auto t = ctx_it->receiver().track();
            s->kind = (t && t->kind() == media_kind::video) ? "video" : "audio";
        }
        s->transport_id = _transport_stats_id;
        s->id = "remote-outbound-rtp_" + std::to_string(ssrc);
        report[s->id] = s;
    }

    // Remote inbound stats (from RTCP RR)
    for (auto &[ssrc, ri] : _remote_inbound_stats) {
        auto s = std::make_shared<rtc_remote_inbound_rtp_stream_stats>(ri);
        s->kind = "audio";
        auto ctx_it = _ssrc_set.find(ssrc);
        if (ctx_it != _ssrc_set.end()) {
            auto t = ctx_it->receiver().track();
            s->kind = (t && t->kind() == media_kind::video) ? "video" : "audio";
        }
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

        _twcc.set_media_ssrc(pkt->ssrc);
        _twcc.handle_incoming(pkt->extension_data,
                              [this](std::vector<uint8_t> report) {
                                  this->sync_send_rtcp(report);
                              });

        dispatch_rtp(*pkt);
    } else {
        // Handle compound RTCP
        auto cps = rtcp::parse_compound(buf->data(), buf->size());
        for (const auto &cp : cps) {
            if (cp.type == rtcp::packet_type::SR) {
                auto ctx_it = _ssrc_set.find(cp.ssrc);
                if (ctx_it != _ssrc_set.end())
                    ctx_it->record_sr(cp.ntp_timestamp);

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
                        auto ctx_it = _ssrc_set.find(b.ssrc);
                        if (ctx_it != _ssrc_set.end() &&
                            ctx_it->lsr_time().time_since_epoch().count() > 0) {
                            auto rtt_us = std::chrono::duration_cast<
                                              std::chrono::microseconds>(
                                              now - ctx_it->lsr_time())
                                              .count() -
                                          (static_cast<int64_t>(b.dlsr) *
                                           1000000 / 65536);
                            if (rtt_us > 0) {
                                ri.round_trip_time = rtt_us / 1000000.0;
                                ctx_it->nack_gen().update_rtt(rtt_us / 1000);
                            }
                        }
                    }
                }
            } else if (cp.type == rtcp::packet_type::PSFB) {
                if (cp.report_count == rtcp::packet_type::PSFB_PLI ||
                    cp.report_count == rtcp::packet_type::PSFB_FIR) {
                    // PLI/FIR → request keyframe for matching SSRC
                } else if (cp.report_count == rtcp::packet_type::PSFB_APP &&
                           !cp.payload.empty()) {
                    (void)cp;
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
                                this->sync_send_rtp(rtx);
                                break;
                            }
                        }
                    }
                }
            } else if (cp.type == rtcp::packet_type::RTPFB &&
                       cp.report_count == rtcp::packet_type::RTPFB_TCC) {
                // TODO:
            }
        }
    }
}

void connection_impl::rewrite_rtx_packet(rtp::rtp_packet &pkt) const noexcept {
    if (pkt.payload.size() <= 2)
        return;
    auto apt_it = _rtx_pt_to_pt.find(pkt.payload_type);
    if (apt_it == _rtx_pt_to_pt.end())
        return;
    auto ssrc_it = _rtx_ssrc_to_ssrc.find(pkt.ssrc);
    if (ssrc_it == _rtx_ssrc_to_ssrc.end())
        return;
    auto seq = asioice::binary::read_big<uint16_t>(pkt.payload.data());
    pkt.payload_type = apt_it->second;
    pkt.ssrc = ssrc_it->second;
    pkt.sequence_number = seq;
    pkt.payload.erase(pkt.payload.begin(), pkt.payload.begin() + 2);
}

bool connection_impl::dispatch_rtp(rtp::rtp_packet &pkt) noexcept {
    rewrite_rtx_packet(pkt);
    rtp_receiver *receiver = nullptr;
    if (!pkt.extension_data.empty() && _mid_ext_id) {
        const auto &ext = pkt.extension_data;
        rtp::rtp_ext_iterator iter{ext.data()};
        rtp::rtp_ext_sentinel end{ext.data() + ext.size()};

        std::string_view mid{};
        for (; iter != end; ++iter) {
            auto e = *iter;
            if ((uint16_t)e.id == *_mid_ext_id) {
                mid = std::string_view(reinterpret_cast<const char *>(e.data),
                                       e.length);
                break;
            }
        }
        if (!mid.empty()) {
            auto it = std::ranges::find_if(_transceivers, [&](const auto &tr) {
                return tr->mid() == mid;
            });
            if (it != _transceivers.end())
                receiver = (*it)->_receiver.get();
        }
    }

    auto ctx_it = _ssrc_set.end();
    if (!receiver) {
        ctx_it = _ssrc_set.find(pkt.ssrc);
        if (ctx_it != _ssrc_set.end())
            receiver = &ctx_it->receiver();
    }
    if (!receiver)
        return false;

    if (ctx_it == _ssrc_set.end())
        ctx_it = _ssrc_set.find(pkt.ssrc);
    if (ctx_it != _ssrc_set.end()) {
        auto &ctx = *ctx_it;
        uint64_t prev = ctx.packets_expected_count();
        ctx.track_packet(pkt.sequence_number, pkt.timestamp);

        if (ctx.check_gap(prev)) {
            ctx.inc_consecutive_lost();
            ctx.nack_gen().receive_packet(pkt.sequence_number);

            if (ctx.consecutive_lost() >= 3 && _srtp_transport) {
                auto pli =
                    rtcp::rtcp_psfb{rtcp::packet_type::PSFB_PLI, 0, pkt.ssrc}
                        .bytes();
                bool sent = this->sync_send_rtcp(pli);
                SAMLOG_TRACE(auto sink) {
                    sink("sent pli rtcp feedback {}, {} bytes\n",
                         sent ? "success" : "failed", pli.size());
                };
                ctx.reset_consecutive_lost();
            }
        } else {
            ctx.reset_consecutive_lost();
            ctx.nack_gen().receive_packet(pkt.sequence_number);
        }
    }

    if (receiver->_on_rtp_cb && !receiver->_on_rtp_cb(pkt))
        return false;

    const auto &track = receiver->track();
    if (!track)
        return false;

    if (pkt.payload_type != track->rtpmap().payload_type) {
        SAMLOG_WARN(auto sink) {
            sink("pkt.payload_type({}) != track->rtpmap().payload_type({})\n",
                 (int)pkt.payload_type, (int)track->rtpmap().payload_type);
        };
    }
    pkt.payload = depayload(track->rtpmap().name, std::move(pkt.payload));

    if (pkt.payload.empty())
        return false;

    track->push_frame(std::move(pkt));
    return true;
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

asiortc::task<void> connection_impl::ice_send_loop() {
    while (true) {
        if (!_send_buf.readable())
            co_await _send_buf.wait_readable();
        auto pkt = _send_buf.peek();
        asioice::utils::scope_guard on_exit(
            [&]() noexcept { _send_buf.pop(); });
        // TODO: avoid heap allocation
        co_await _agent.sendto(pkt, 1);
    }
}

bool connection_impl::sync_send_rtp(std::span<const uint8_t> data) noexcept {
    if (!_srtp_transport)
        return false;
    std::array<uint8_t, 2000> buf;
    auto enc = _srtp_transport->protect_rtp(data, buf);
    if (enc.empty())
        return false;
    return _send_buf.try_write(enc);
}

bool connection_impl::sync_send_rtcp(std::span<const uint8_t> data) noexcept {
    if (!_srtp_transport)
        return false;
    std::array<uint8_t, 1200> buf;
    auto enc = _srtp_transport->protect_rtcp(data, buf);
    if (enc.empty())
        return false;
    return _send_buf.try_write(enc);
}

asiortc::task<void> connection_impl::periodic_cleaning_loop() { co_return; }

} // namespace asiortc

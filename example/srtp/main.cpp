#include "connection_impl.hpp"
#include "rtp.hpp"
#include "sdp.hpp"
#include "srtp_transport.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
namespace asioice {
namespace net = boost::asio;
}
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
#else
#error "Requires Boost.Asio"
#endif

#include "json.hpp"

#include <chrono>
#include <cstring>
#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>
#include <iostream>
#include <memory>
#include <stdexec/execution.hpp>
#include <string>
#include <vector>

using namespace asioice;
using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8084;

static task<void> ws_send(ws_t &ws, const nlohmann::json &msg) {
    ws.text(true);
    auto d = msg.dump();
    auto [ec, n] = co_await ws.async_write(net::buffer(d),
                                           net::as_tuple(utils::use_sender));
    if (ec)
        std::cerr << "ws err: " << ec.message() << '\n';
}

static task<nlohmann::json> ws_recv(ws_t &ws) {
    beast::flat_buffer buf;
    auto [ec, n] =
        co_await ws.async_read(buf, net::as_tuple(utils::use_sender));
    if (ec)
        throw std::runtime_error("ws recv: " + ec.message());
    auto j = nlohmann::json::parse(beast::buffers_to_string(buf.data()));
    buf.clear();
    co_return j;
}

static std::string srtp_suite_name(ssl::srtp_protection_profile profile) {
    switch (profile) {
    case ssl::srtp_protection_profile::srtp_aes128_cm_sha1_80:
        return "AES_CM_128_HMAC_SHA1_80";
    case ssl::srtp_protection_profile::srtp_aes128_cm_sha1_32:
        return "AES_CM_128_HMAC_SHA1_32";
    case ssl::srtp_protection_profile::srtp_aead_aes_128_gcm:
        return "AES_128_GCM";
    case ssl::srtp_protection_profile::srtp_aead_aes_256_gcm:
        return "AES_256_GCM";
    default:
        return "unknown";
    }
}

struct sending_video_track : public media_track {
    sending_video_track(net::io_context &ctx)
        : _kind(media_kind::video), _id("sender-video"), _timer(ctx) {}

    media_kind kind() const noexcept override { return _kind; }
    std::string id() const noexcept override { return _id; }
    track_state ready_state() const noexcept override { return _state; }
    void stop() override { _state = track_state::ended; }

    asioice::task<std::optional<media_frame>> recv() override {
        _timer.expires_after(std::chrono::milliseconds(500));
        auto [ec] =
            co_await _timer.async_wait(net::as_tuple(utils::use_sender));
        if (ec || _state == track_state::ended)
            co_return std::nullopt;

        media_frame frame;
        frame.kind = media_kind::video;
        frame.timestamp = _ts;

        static const uint8_t vp8_keyframe[] = {
            0xd0, 0x42, 0x00, 0x9d, 0x01, 0x2a, 0x80, 0x02, 0xe0, 0x01, 0x02,
            0xc7, 0x08, 0x85, 0x85, 0x88, 0x99, 0x84, 0x88, 0x0f, 0x02, 0x02,
            0x75, 0xaa, 0x02, 0x06, 0x66, 0x65, 0xa8, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9,
            0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3,
            0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39,
            0xc3, 0xd9, 0x39, 0xc3, 0xd9, 0x39, 0x70, 0x00, 0xfe, 0xf8, 0x41,
            0xcb, 0xff, 0x7d, 0x00, 0xff, 0xbd, 0x00, 0xff, 0xbd, 0x00, 0xff,
            0x17, 0xfd, 0x77, 0x8b, 0x49, 0x48, 0x00,
        };
        const size_t vp8_len = sizeof(vp8_keyframe);

        frame.data.resize(1 + vp8_len);
        frame.data[0] = 0x10; // VP8 descriptor: S=1, PID=0
        std::memcpy(frame.data.data() + 1, vp8_keyframe, vp8_len);

        _ts += 3000;
        co_return frame;
    }

  private:
    media_kind _kind;
    std::string _id;
    track_state _state = track_state::live;
    uint32_t _ts = 90000;
    net::steady_timer _timer;
};

static task<void> srtp_session(net::io_context &ctx, ws_ptr ws) {
    std::cout << "WS connected (asiortc SRTP demo)\n";
    utils::scheduler sched{ctx};

    exec::async_scope scope;
    net::steady_timer timer(ctx);

    auto msg = co_await ws_recv(*ws);
    auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer");
    std::cout << "Offer: ufrag=" << offer.ice_ufrag
              << " fp=" << offer.fingerprint
              << " cands=" << offer.candidates.size()
              << " medias=" << offer.medias.size() << '\n';
    for (auto &m : offer.medias)
        std::cout << "  media: " << m.media_type << " " << m.proto
                  << " mid=" << m.mid << " pts=" << m.payload_types.size()
                  << '\n';

    auto conn = std::make_shared<connection_impl>(ctx.get_executor());

    conn->on_track([](std::shared_ptr<rtp_receiver> receiver,
                      std::shared_ptr<media_track> track,
                      std::vector<std::string> msids,
                      std::shared_ptr<rtp_transceiver> transceiver) {
        std::cout << "New track: kind="
                  << (track->kind() == media_kind::audio ? "audio" : "video")
                  << " id=" << track->id() << " mid=" << transceiver->mid()
                  << " msids=" << msids.size() << '\n';
    });

    co_await conn->set_remote_description(std::move(offer));

    // Find video transceiver and set up sending
    auto send_track = std::make_shared<sending_video_track>(ctx);
    for (auto &t : conn->transceivers()) {
        if (!t->codecs().empty() && t->codecs()[0].name.starts_with("VP")) {
            t->set_direction(sdp_direction::sendrecv);
            t->sender()->set_track(send_track);
            std::cout << "Set send track on transceiver mid=" << t->mid()
                      << '\n';
            break;
        }
    }

    auto answer = co_await conn->create_answer();
    co_await conn->set_local_description(std::move(answer));
    if (!conn->can_trickle_ice_candidates()) {
        std::cout << "Peer does not support trickle ice\n";
        while (conn->ice_gathering_state() != ice_gathering_state_t::complete)
            co_await conn->on_ice_gathering_state_changed();
        std::cout << "Gathering complete\n";
    } else {
        std::cout << "Peer supports trickle ice\n";
    }
    const auto *local_desc = conn->local_description();
    if (!local_desc) {
        conn->close();
        co_return;
    }
    auto answer_sdp_str = local_desc->to_string();
    co_await ws_send(*ws, {{"type", "answer"}, {"sdp", answer_sdp_str}});
    std::cout << "Sent answer\n";

    auto srtp = conn->srtp();

    int recv_count{0};
    conn->on_new_ssrc([&](uint32_t ssrc, std::span<const uint8_t>) -> bool {
        std::cout << "New SSRC: 0x" << std::hex << ssrc << std::dec << '\n';
        return true;
    });

    conn->on_rtp_rtcp_packet([&](io_buffer_ptr buf) {
        auto pkt = rtp::rtp_packet::parse(buf->data(), buf->size());
        if (pkt) {
            int n = ++recv_count;
            std::cout << "RTP recv #" << n << " SSRC=0x" << std::hex
                      << pkt->ssrc << std::dec
                      << " PT=" << (int)pkt->payload_type
                      << " seq=" << pkt->sequence_number
                      << " ts=" << pkt->timestamp
                      << " marker=" << (int)pkt->marker
                      << " payload=" << pkt->payload.size() << "B\n";
        } else {
            std::cout << "RTP parse failed\n";
        }
    });

    std::cout << "Waiting for ICE+DTLS+SRTP setup...\n";
    for (int i = 0; i < 200 && !srtp; ++i) {
        net::steady_timer t(ctx, std::chrono::milliseconds(50));
        co_await t.async_wait(utils::use_sender);
        srtp = conn->srtp();
    }
    if (!srtp) {
        std::cerr << "SRTP setup timeout\n";
        conn->close();
        co_return;
    }

    std::cout << "SRTP profile: " << srtp_suite_name(srtp->profile()) << '\n';
    std::cout << "Sender loops active, RTP will flow via track API\n";

    timer.expires_after(std::chrono::seconds(15));
    co_await timer.async_wait(utils::use_sender);

    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));
    std::cout << "Done. RTP received: " << recv_count << '\n';
    conn->close();
}

static task<void> http_session(net::io_context &ctx,
                               net::ip::tcp::socket sock) {
    beast::flat_buffer buf;
    http::request<http::string_body> req;
    auto [ec, n] = co_await http::async_read(sock, buf, req,
                                             net::as_tuple(utils::use_sender));
    if (ec)
        co_return;
    if (websocket::is_upgrade(req)) {
        auto ws = std::make_shared<ws_t>(std::move(sock));
        ws->set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::server));
        auto [wec] =
            co_await ws->async_accept(req, net::as_tuple(utils::use_sender));
        if (wec) {
            std::cerr << "WS handshake failed: " << wec.message() << '\n';
            co_return;
        }
        try {
            co_await srtp_session(ctx, ws);
        } catch (const std::exception &e) {
            std::cerr << "Session error: " << e.what() << '\n';
        }
        co_return;
    }
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "asiortc");
    res.set(http::field::content_type, "text/plain");
    res.body() = "OK";
    res.prepare_payload();
    co_await http::async_write(sock, res, net::as_tuple(utils::use_sender));
}

static task<void> listener(net::io_context &ctx) {
    net::ip::tcp::acceptor acc(
        ctx, net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), PORT));
    std::cout << "Server on ws://localhost:" << PORT << "/ws\n";
    while (true) {
        auto [ec, sock] =
            co_await acc.async_accept(net::as_tuple(utils::use_sender));
        if (ec)
            continue;
        exec::start_detached(http_session(ctx, std::move(sock)));
    }
}

int main() {
    std::cout << std::unitbuf;
    net::io_context ctx;
    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, listener(ctx)));
    ctx.run();
}

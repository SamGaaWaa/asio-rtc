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

static task<void> srtp_session(net::io_context &ctx, ws_ptr ws) {
    std::cout << "WS connected (asiortc SRTP demo)\n";
    utils::scheduler sched{ctx};

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

    co_await conn->set_remote_description(std::move(offer));

    auto answer = co_await conn->create_answer();
    auto answer_sdp_str = answer.to_string();
    co_await ws_send(*ws, {{"type", "answer"}, {"sdp", answer_sdp_str}});
    std::cout << "Sent answer\n";

    co_await conn->set_local_description(std::move(answer));

    auto srtp = conn->srtp();

    std::atomic<int> recv_count{0};
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
        co_return;
    }

    std::cout << "SRTP profile: " << srtp_suite_name(srtp->profile()) << '\n';

    exec::async_scope scope;

    scope.spawn([&](net::io_context &ctx, std::shared_ptr<connection_impl> conn,
                    std::shared_ptr<connection_impl::srtp_transport_type> srtp)
                    -> task<void> {
        auto ssrc = 0xDEADBEEF;
        uint16_t seq = 0;
        uint32_t ts = 0;
        uint8_t pt = 97;
        std::vector<uint8_t> original(100);
        std::memset(original.data(), 0, 12);
        original[0] = 0x80;
        original[1] = pt & 0x7F;
        for (size_t i = 12; i < 100; ++i)
            original[i] = static_cast<uint8_t>(i & 0xFF);

        std::vector<uint8_t> protected_buf(
            original.size() + srtp_transport_base::max_protect_rtp_overhead());

        net::steady_timer send_timer(ctx);
        int send_count = 0;

        while (true) {
            send_timer.expires_after(std::chrono::milliseconds(500));
            auto [ec] = co_await send_timer.async_wait(
                net::as_tuple(utils::use_sender));
            if (ec)
                break;

            seq++;
            ts += 3000;

            auto orig = original;
            orig[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
            orig[3] = static_cast<uint8_t>(seq & 0xFF);
            orig[4] = static_cast<uint8_t>((ts >> 24) & 0xFF);
            orig[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
            orig[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
            orig[7] = static_cast<uint8_t>(ts & 0xFF);
            orig[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
            orig[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
            orig[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
            orig[11] = static_cast<uint8_t>(ssrc & 0xFF);

            auto enc = srtp->protect_rtp(orig, protected_buf);
            if (enc.empty()) {
                std::cerr << "protect_rtp failed\n";
                break;
            }

            auto [sec, sn] =
                co_await conn->sendto(net::buffer(enc.data(), enc.size()), 1);
            if (sec) {
                std::cerr << "ICE sendto error: " << sec.message() << '\n';
                break;
            }
            send_count++;
            if (send_count % 10 == 0)
                std::cout << "RTP sent " << send_count << '\n';
        }
    }(ctx, conn, srtp));

    std::cout << "SRTP session active (ctrl-c to stop)\n";
    net::steady_timer timer(ctx, std::chrono::seconds(30));
    co_await timer.async_wait(utils::use_sender);

    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));
    std::cout << "Done. RTP received: " << recv_count << '\n';

    co_await conn->close();
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

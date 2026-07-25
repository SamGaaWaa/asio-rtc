#include "connection_impl.hpp"
#include "asiortc/rtp.hpp"
#include "rtcp.hpp"
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

using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8085;

static task<void> ws_send(ws_t &ws, const nlohmann::json &msg) {
    ws.text(true);
    auto d = msg.dump();
    auto [ec, n] = co_await ws.async_write(net::buffer(d),
                                           net::as_tuple(utils::use_sender));
    if (ec)
        std::cerr << "ws send err: " << ec.message() << '\n';
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

static std::string
srtp_suite_name(asioice::ssl::srtp_protection_profile profile) {
    switch (profile) {
    case asioice::ssl::srtp_protection_profile::srtp_aes128_cm_sha1_80:
        return "AES_CM_128_HMAC_SHA1_80";
    case asioice::ssl::srtp_protection_profile::srtp_aes128_cm_sha1_32:
        return "AES_CM_128_HMAC_SHA1_32";
    case asioice::ssl::srtp_protection_profile::srtp_aead_aes_128_gcm:
        return "AES_128_GCM";
    case asioice::ssl::srtp_protection_profile::srtp_aead_aes_256_gcm:
        return "AES_256_GCM";
    default:
        return "unknown";
    }
}

int recv_count{0};

static void setup_transceivers(connection_impl &conn) {
    {
        auto audio = conn.add_transceiver(
            media_kind::audio, {.direction = sdp_direction::sendrecv,
                                .streams = {"asiortc-audio audio0"}});
        audio->set_codecs({
            {111, "opus", 48000, "2"},
            {63, "telephone-event", 8000, ""},
        });
        std::cout << "Added audio transceiver mid=" << audio->mid()
                  << " dir=" << (int)audio->direction()
                  << " codecs=" << audio->codecs().size() << '\n';
        audio->receiver()->on_rtp([&](rtp::rtp_packet &pkt) {
            int n = ++recv_count;
            std::cout << "audio recv RTP #" << n << " SSRC=0x" << std::hex
                      << pkt.ssrc << std::dec << " PT=" << (int)pkt.payload_type
                      << " seq=" << pkt.sequence_number
                      << " ts=" << pkt.timestamp
                      << " payload=" << pkt.payload.size() << "B\n";
            return true;
        });
    }

    {
        auto video = conn.add_transceiver(
            media_kind::video, {.direction = sdp_direction::sendrecv,
                                .streams = {"asiortc-video video0"}});
        video->set_codecs({
            {96, "VP8", 90000, ""},
            {97, "rtx", 90000, "apt=96"},
        });
        std::cout << "Added video transceiver mid=" << video->mid()
                  << " dir=" << (int)video->direction()
                  << " codecs=" << video->codecs().size() << '\n';
        video->receiver()->on_rtp([&](rtp::rtp_packet &pkt) {
            int n = ++recv_count;
            std::cout << "video recv RTP #" << n << " SSRC=0x" << std::hex
                      << pkt.ssrc << std::dec << " PT=" << (int)pkt.payload_type
                      << " seq=" << pkt.sequence_number
                      << " ts=" << pkt.timestamp
                      << " payload=" << pkt.payload.size() << "B\n";
            return true;
        });
    }

    for (auto &t : conn.transceivers()) {
        auto s = t->sender();
        auto r = t->receiver();
        std::cout << "  transceiver " << t->mid()
                  << ": sender=" << (s ? s->mid() : "null")
                  << " receiver=" << (r ? r->mid() : "null")
                  << " sender->transceiver="
                  << (s && s->transceiver() == t ? "ok" : "FAIL")
                  << " receiver->transceiver="
                  << (r && r->transceiver() == t ? "ok" : "FAIL") << '\n';
    }
}

static void verify_cyclic_ownership(connection_impl &conn) {
    std::cout << "Ownership check (shared_ptr use counts):\n";
    for (auto &t : conn.transceivers()) {
        auto s = t->sender();
        auto r = t->receiver();
        std::cout << "  transceiver " << t->mid()
                  << " use_count=" << t.use_count()
                  << " sender use_count=" << s.use_count()
                  << " receiver use_count=" << r.use_count()
                  << " sender->transceiver() use_count="
                  << (s->transceiver() ? s->transceiver().use_count() : 0)
                  << '\n';
    }
    std::cout << "  conn->transceiver is acyclic: "
              << (conn.transceivers().size() == 2 ? "ok" : "FAIL") << '\n';
}

static task<void> transceiver_session(net::io_context &ctx, ws_ptr ws) {
    std::cout << "WS connected (asiortc transceiver test)\n";
    utils::scheduler sched{ctx};

    exec::async_scope scope;
    net::steady_timer timer(ctx);

    auto conn = std::make_shared<connection_impl>(ctx.get_executor());
    asioice::utils::scope_guard auto_close([&]() noexcept { conn->close(); });
    setup_transceivers(*conn);

    auto offer = co_await conn->create_offer();
    co_await conn->set_local_description(parse_sdp(offer.to_string(), "offer"));

    if (!conn->can_trickle_ice_candidates()) {
        std::cout << "Waiting for ICE gathering...\n";
        while (conn->ice_gathering_state() != ice_gathering_state_t::complete)
            co_await conn->on_ice_gathering_state_changed();
        std::cout << "Gathering complete\n";
    }

    const auto *local_desc = conn->local_description();
    if (!local_desc) {
        std::cerr << "No local description\n";
        co_return;
    }

    auto offer_sdp = local_desc->to_string();
    std::cout << "\n=== OFFER SDP ===" << "\n"
              << offer_sdp << "\n=== END OFFER ===\n\n";
    co_await ws_send(*ws, {{"type", "offer"}, {"sdp", offer_sdp}});
    std::cout << "Sent offer, waiting for answer...\n";

    auto msg = co_await ws_recv(*ws);
    auto answer = parse_sdp(msg["sdp"].get<std::string>(), "answer");
    std::cout << "Answer: ufrag=" << answer.ice_ufrag
              << " medias=" << answer.medias.size() << '\n';
    for (auto &m : answer.medias)
        std::cout << "  " << m.media_type << " proto=" << m.proto
                  << " mid=" << m.mid << " port=" << m.port
                  << " dir=" << (int)m.direction << '\n';

    co_await conn->set_remote_description(std::move(answer));

    auto srtp = conn->srtp();

    conn->on_new_ssrc([&](uint32_t ssrc, std::span<const uint8_t>) -> bool {
        std::cout << "New SSRC: 0x" << std::hex << ssrc << std::dec << '\n';
        return true;
    });

    std::cout << "Waiting for ICE+DTLS+SRTP setup...\n";
    for (int i = 0; i < 200 && !srtp; ++i) {
        timer.expires_after(std::chrono::milliseconds(50));
        co_await timer.async_wait(utils::use_sender);
        srtp = conn->srtp();
    }
    if (!srtp) {
        std::cerr << "SRTP setup timeout\n";
        co_return;
    }

    std::cout << "SRTP profile: " << srtp_suite_name(srtp->profile()) << '\n';
    std::cout << "Transceivers after connect: " << conn->transceivers().size()
              << '\n';
    verify_cyclic_ownership(*conn);

    std::cout << "\nSRTP session active (ctrl-c to stop)\n";
    std::cout << "RTP received: " << recv_count << " packets\n";
    timer.expires_after(std::chrono::seconds(15));
    co_await timer.async_wait(utils::use_sender);

    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));
    std::cout << "Done. Total RTP received: " << recv_count << '\n';
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
            co_await transceiver_session(ctx, ws);
        } catch (const std::exception &e) {
            std::cerr << "Session error: " << e.what() << '\n';
        }
        std::cout << "Session ended\n";
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

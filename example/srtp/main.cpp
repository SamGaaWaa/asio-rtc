#include "srtp_transport.hpp"

#include "asioice/agent.hpp"
#include "asioice/agent_config.hpp"
#include "asioice/candidate.hpp"
#include "asioice/detail/stop_when.hpp"
#include "asioice/dtls_transport.hpp"
#include "asioice/ssl/dtls_config.hpp"
#include "asioice/socket_transport.hpp"
#include "json.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/as_tuple.hpp>
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

#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace asioice;
using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8084;

static task<void> ws_send(ws_t &ws, const nlohmann::json &msg) {
    ws.text(true);
    auto d = msg.dump();
    auto [ec, n] =
        co_await ws.async_write(net::buffer(d), net::as_tuple(utils::use_sender));
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

static std::pair<ssl::hash_algorithm, std::string>
parse_fingerprint(const std::string &fp_str) {
    auto space = fp_str.find(' ');
    if (space == std::string::npos)
        throw std::runtime_error("bad fingerprint format: " + fp_str);
    auto algo_name = fp_str.substr(0, space);
    auto value = fp_str.substr(space + 1);
    ssl::hash_algorithm algo;
    if (algo_name == "sha-256")
        algo = ssl::hash_algorithm::sha256;
    else if (algo_name == "sha-384")
        algo = ssl::hash_algorithm::sha384;
    else if (algo_name == "sha-512")
        algo = ssl::hash_algorithm::sha512;
    else if (algo_name == "sha-1")
        algo = ssl::hash_algorithm::sha1;
    else
        throw std::runtime_error("unknown hash: " + algo_name);
    return {algo, value};
}

static std::string
srtp_suite_name(ssl::srtp_protection_profile profile) {
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

static std::string find_rtpmap_line(std::string_view sdp) {
    std::string_view r = sdp;
    while (!r.empty()) {
        auto p = r.find("\r\n");
        if (p == std::string_view::npos) p = r.find('\n');
        auto l = r.substr(0, p);
        r.remove_prefix(p == std::string_view::npos
                            ? r.size()
                            : p + (l.size() < r.size() && r[l.size()] == '\r'
                                       ? 2 : 1));
        if (l.starts_with("a=rtpmap:"))
            return std::string(l);
    }
    return "a=rtpmap:97 VP8/90000";
}

static task<void> srtp_session(net::io_context &ctx, ws_ptr ws) {
    std::cout << "WS connected (asiortc SRTP demo)\n";
    utils::scheduler sched{ctx};

    ssl::dtls_certificate cert;
    auto local_fp = cert.get_fingerprint(ssl::hash_algorithm::sha256);
    std::cout << "DTLS fp: " << local_fp.value << '\n';

    auto msg = co_await ws_recv(*ws);
    auto offer = parse_remote_sdp(msg["sdp"].get<std::string>());
    std::cout << "Offer: ufrag=" << offer.ice_ufrag
              << " fp=" << offer.fingerprint
              << " cands=" << offer.candidates.size() << '\n';

    agent_config cfg = {
        .username = "asiortc_srtp",
        .password = "srtp_pwd",
        .ice_controlling = false,
        .use_loopback = true,
        .component_count = 1,
    };
    cfg.trickle_ice = false;

    agent ag(ctx.get_executor(), cfg);
    ag.set_remote_username(offer.ice_ufrag);
    ag.set_remote_password(offer.ice_pwd);

    using IceT = agent::ice_transport_type;
    using DtlsT = ssl::dtls_transport<IceT>;

    auto ice = ag.create_ice_transport(1);
    auto dtls = std::make_shared<DtlsT>(ice, std::move(cert));
    auto [algo, fp_val] = parse_fingerprint(offer.fingerprint);
    dtls->set_expected_remote_fingerprint(ssl::fingerprint{algo, fp_val});

    auto rtpmap_line = find_rtpmap_line(msg["sdp"].get<std::string>());

    for (const auto &line : offer.candidates) {
        auto c = candidate::from_sdp(line);
        if (c)
            co_await ag.add_remote_candidate(std::move(*c));
    }
    co_await ag.add_remote_candidate();

    auto srtp = std::make_shared<srtp_transport<IceT>>(ice);

    net::steady_timer timer(ctx, std::chrono::seconds(5));
    co_await utils::stop_when(ag.gather_candidates(),
                              timer.async_wait(utils::use_sender));

    std::ostringstream sdp;
    sdp << "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        << "a=group:BUNDLE 0\r\n"
        << "m=video 9 UDP/TLS/RTP/SAVPF 97\r\n"
        << "c=IN IP4 0.0.0.0\r\na=mid:0\r\n"
        << "a=sendrecv\r\n"
        << "a=rtcp-mux\r\n"
        << rtpmap_line << "\r\n"
        << "a=ice-ufrag:" << ag.local_username() << "\r\n"
        << "a=ice-pwd:" << ag.local_password() << "\r\n"
        << "a=fingerprint:" << local_fp.to_sdp() << "\r\n"
        << "a=setup:active\r\n";
    for (const auto &c : ag.local_candidates())
        sdp << "a=" << c.to_sdp() << "\r\n";
    co_await ws_send(*ws, {{"type", "answer"}, {"sdp", sdp.str()}});
    std::cout << "Sent answer, connecting\n";

    bool connected = co_await ag.connect();
    if (!connected) {
        std::cerr << "ICE failed to connect\n";
        co_return;
    }
    std::cout << "ICE connected!\n";

    std::cout << "DTLS handshake (client)...\n";
    auto hs_ec = co_await dtls->async_handshake(DtlsT::handshake_type::client);
    if (hs_ec) {
        std::cerr << "DTLS failed: " << hs_ec.message() << '\n';
        co_return;
    }
    auto remote_fp = dtls->get_remote_fingerprint(ssl::hash_algorithm::sha256);
    std::cout << "DTLS OK, remote fp: " << remote_fp.value << '\n';

    auto keys = dtls->export_srtp_key_material();
    if (!keys || keys->profile == ssl::srtp_protection_profile::none) {
        std::cerr << "No SRTP key material exported\n";
        co_return;
    }
    std::cout << "SRTP profile: " << srtp_suite_name(keys->profile) << '\n'
              << "  client_write_key: " << keys->client_write_key.size() << "B\n"
              << "  client_write_salt: " << keys->client_write_salt.size() << "B\n"
              << "  server_write_key: " << keys->server_write_key.size() << "B\n"
              << "  server_write_salt: " << keys->server_write_salt.size() << "B\n";

    srtp->setup(*keys, ssl::dtls_role::client);

    std::atomic<int> recv_count{0};
    srtp->on_new_ssrc([&](uint32_t ssrc,
                           std::span<const uint8_t> data) -> bool {
        std::cout << "New SSRC: 0x" << std::hex << ssrc << std::dec << '\n';
        return true;
    });

    srtp->on_rtp_rtcp_packet([&](io_buffer_ptr buf) {
        auto pkt = rtp::rtp_packet::parse(buf->data(), buf->size());
        if (pkt) {
            int n = ++recv_count;
            std::cout << "RTP recv #" << n
                      << " SSRC=0x" << std::hex << pkt->ssrc << std::dec
                      << " PT=" << (int)pkt->payload_type
                      << " seq=" << pkt->sequence_number
                      << " ts=" << pkt->timestamp
                      << " marker=" << (int)pkt->marker
                      << " payload=" << pkt->payload.size() << "B\n";
        } else {
            std::cout << "RTP parse failed\n";
        }
    });

    exec::async_scope scope;

    scope.spawn([&](agent &ag,
                    std::shared_ptr<srtp_transport<IceT>> srtp) -> task<void> {
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

        std::vector<uint8_t> protected_buf(original.size() +
                                           srtp_transport_base::max_protect_rtp_overhead());

        net::steady_timer send_timer(ctx);
        int send_count = 0;

        while (true) {
            send_timer.expires_after(std::chrono::milliseconds(500));
            auto [ec] =
                co_await send_timer.async_wait(net::as_tuple(utils::use_sender));
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
                co_await ag.sendto(net::buffer(enc.data(), enc.size()), 1);
            if (sec) {
                std::cerr << "ICE sendto error: " << sec.message() << '\n';
                break;
            }
            send_count++;
            if (send_count % 10 == 0)
                std::cout << "RTP sent " << send_count << '\n';
        }
    }(ag, srtp));

    std::cout << "SRTP session active, rtp send/rcv in progress (ctrl-c to stop)\n";
    timer.expires_after(std::chrono::seconds(30));
    co_await timer.async_wait(utils::use_sender);

    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));
    std::cout << "Done. RTP received: " << recv_count << '\n';
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

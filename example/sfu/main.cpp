#include "asiortc.hpp"
#include "asioice/detail/async_queue.hpp"
#include "asiortc/rtp.hpp"

#include "asioice/detail/scope_guard.hpp"

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
#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8086;

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

static task<void> sfw_session(net::io_context &ctx, ws_ptr ws) {
    asioice::utils::scope_guard on_exit(
        []() noexcept { std::cout << "sfu_session: exited\n"; });
    std::cout << "WS connected (asiortc sfu demo)\n";

    utils::scheduler sched{ctx};
    exec::async_scope scope;
    net::steady_timer timer(ctx);

    peer_connection conn(
        ctx.get_executor(),
        configuration{.ice_servers{.urls = {"stun:14.29.112.241:20002"}}});

    std::cout << "Waiting for browser offer...\n";
    auto msg = co_await ws_recv(*ws);
    auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer").value();
    std::cout << "Offer: medias=" << offer.medias.size() << '\n';

    auto tr = conn.add_transceiver(
        media_kind::video,
        {.direction = sdp_direction::sendrecv, .streams = {"sfu-loopback"}});
    std::cout << "Created video sendrecv mid=" << tr.mid() << '\n';

    co_await conn.set_remote_description(std::move(offer));

    std::cout << "Transceiver: direction="
              << (tr.direction() == sdp_direction::sendrecv   ? "sendrecv"
                  : tr.direction() == sdp_direction::sendonly ? "sendonly"
                  : tr.direction() == sdp_direction::recvonly ? "recvonly"
                                                              : "inactive")
              << " mid=" << tr.mid() << " codecs=" << tr.codecs().size()
              << " sender_ssrc=" << tr.sender().ssrc(0)
              << " num_streams=" << tr.sender().num_streams() << '\n';

    {
        auto answer = co_await conn.create_answer();
        co_await conn.set_local_description(std::move(answer));
    }

    std::cout << "After set_local: direction="
              << (tr.direction() == sdp_direction::sendrecv   ? "sendrecv"
                  : tr.direction() == sdp_direction::sendonly ? "sendonly"
                  : tr.direction() == sdp_direction::recvonly ? "recvonly"
                                                              : "inactive")
              << " sender_ssrc=" << tr.sender().ssrc(0) << '\n';

    {
        for (int i = 0; i < 20 && conn.ice_gathering_state() !=
                                      ice_gathering_state_t::complete;
             ++i) {
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(utils::use_sender);
        }
    }

    const auto *local_desc = conn.local_description();
    if (!local_desc) {
        std::cerr << "No local description\n";
        co_return;
    }
    auto answer_sdp = local_desc->to_string();
    std::cout << "\n=== ANSWER SDP ===\n"
              << answer_sdp << "=== END ANSWER ===\n\n";
    co_await ws_send(*ws, {{"type", "answer"}, {"sdp", answer_sdp}});
    std::cout << "Sent answer, waiting for connection...\n";

    while (conn.connection_state() != connection_state_t::connected &&
           conn.connection_state() != connection_state_t::failed)
        co_await conn.on_connection_state_changed();
    if (conn.connection_state() != connection_state_t::connected) {
        std::cerr << "Failed to connect\n";
        co_return;
    }
    std::cout << "Connected: SFU forwarding active (ctrl-c to stop)\n";

    // SFU forwarding: intercept RTP, forward via connection
    asioice::async_queue<rtp::rtp_packet> sfw_queue(128);
    auto send = tr.sender();
    std::cout << "SFU: sender ssrc=" << send.ssrc(0)
              << " num_streams=" << send.num_streams() << '\n';

    tr.receiver().on_rtp([&](rtp::rtp_packet &pkt) {
        sfw_queue.push(pkt);
        return false;
    });

    scope.spawn(stdexec::starts_on(
        sched,
        [](peer_connection conn, rtp_sender_interface send,
           auto &sfw_queue) -> task<void> {
            while (true) {
                auto pkt = sfw_queue.try_pop();
                if (!pkt) {
                    pkt = co_await sfw_queue.async_pop_stoppable();
                    if (!pkt)
                        co_return;
                }
                co_await conn.send_rtp(send, *pkt);
            }
        }(std::move(conn), send, sfw_queue)));

    timer.expires_after(std::chrono::seconds(3600));
    auto [ec] = co_await timer.async_wait(net::as_tuple(utils::use_sender));
    if (ec)
        co_return;

    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));
    std::cout << "Done\n";
}

static task<void> http_session(net::io_context &ctx,
                               net::ip::tcp::socket sock) {
    beast::flat_buffer buf;
    http::request<http::string_body> req;
    auto [ec, n] = co_await http::async_read(sock, buf, req, utils::use_sender);
    if (ec) {
        std::cerr << "http read: " << ec.message() << '\n';
        co_return;
    }

    if (websocket::is_upgrade(req)) {
        auto ws = std::make_shared<ws_t>(beast::tcp_stream(std::move(sock)));
        auto [wec] =
            co_await ws->async_accept(req, net::as_tuple(utils::use_sender));
        if (wec) {
            std::cerr << "ws accept: " << wec.message() << '\n';
            co_return;
        }
        co_await sfw_session(ctx, std::move(ws));
    } else {
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "text/html");
        static constexpr char html[] = {
#embed "index.html"
            , '\0'};
        res.body() = html;
        res.prepare_payload();
        auto [sec, _] = co_await http::async_write(
            sock, res, net::as_tuple(utils::use_sender));
        if (sec)
            std::cerr << "http write: " << sec.message() << '\n';
    }
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
    asiortc::set_logger(std::make_shared<logger_interface>(),
                        ctx.get_executor());
    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, listener(ctx)));
    ctx.run();
}

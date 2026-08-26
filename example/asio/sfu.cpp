#include "asiortc.hpp"
#include "asiortc/queue_track.hpp"
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
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
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
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8086;

static net::awaitable<void> ws_send(ws_t &ws, const nlohmann::json &msg) {
    ws.text(true);
    auto d = msg.dump();
    auto [ec, n] = co_await ws.async_write(net::buffer(d),
                                           net::as_tuple(net::use_awaitable));
    if (ec)
        std::cerr << "ws err: " << ec.message() << '\n';
}

static net::awaitable<nlohmann::json> ws_recv(ws_t &ws) {
    beast::flat_buffer buf;
    co_await ws.async_read(buf, net::use_awaitable);
    auto j = nlohmann::json::parse(beast::buffers_to_string(buf.data()));
    co_return j;
}

static net::awaitable<void> sfu_session(net::io_context &ctx, ws_ptr ws) {
    net::cancellation_signal signal;
    asioice::utils::scope_guard on_exit([&]() noexcept {
        std::cout << "sfu_session: exited\n";
        signal.emit(net::cancellation_type::all);
    });
    std::cout << "WS connected (asiortc sfu demo)\n";

    net::steady_timer timer(ctx);

    peer_connection conn(
        ctx.get_executor(),
        configuration{.ice_servers{.urls = {"stun:14.29.112.241:20002"}}});

    std::cout << "Waiting for browser offer...\n";
    auto msg = co_await ws_recv(*ws);
    std::cout << "\n=== OFFER SDP ===\n"
              << msg["sdp"].get<std::string>() << "=== END OFFER ===\n\n";
    auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer");
    if (!offer) {
        std::cerr << "parse sdp failed:" << msg["sdp"].get<std::string>()
                  << '\n';
        co_return;
    }

    auto send_track = std::make_shared<queue_track>(
        media_description::make_default(media_format::h264));
    auto tr =
        conn.add_transceiver(send_track, {.direction = sdp_direction::sendrecv,
                                          .streams = {"sfu-loopback"}});
    std::cout << "Created video sendrecv mid=" << tr.mid() << '\n';

    std::shared_ptr<media_track> recv_track = nullptr;
    conn.on_track([&recv_track](rtp_receiver_interface,
                                std::shared_ptr<media_track> track,
                                std::vector<std::string> streams,
                                rtp_transceiver_interface transceiver) {
        recv_track = std::move(track);
    });
    co_await conn.set_remote_description(std::move(offer), net::use_awaitable);
    if (!recv_track) {
        std::cerr << "No tracks\n";
        co_return;
    }

    net::co_spawn(
        ctx,
        [](auto recv_track, auto send_track) -> net::awaitable<void> {
            asioice::utils::scope_guard on_exit(
                []() noexcept { std::cout << "forward loop exit\n"; });
            auto state = co_await net::this_coro::cancellation_state;
            co_await net::this_coro::throw_if_cancelled(true);
            while (state.cancelled() == net::cancellation_type::none) {
                std::vector<media_frame> frames = co_await recv_track->recv(
                    {}, net::bind_cancellation_slot(state.slot(),
                                                    net::use_awaitable));
                for (auto &frame : frames) {
                    send_track->push_frame(std::move(frame));
                }
            }
        }(recv_track, send_track),
        net::bind_cancellation_slot(signal.slot(), [](std::exception_ptr p) {
            if (!p)
                return;
            try {
                std::rethrow_exception(p);
            } catch (const std::exception &e) {
                std::cerr << "Exception in forward loop: " << e.what() << '\n';
            }
        }));

    std::cout << "Transceiver: direction="
              << (tr.direction() == sdp_direction::sendrecv   ? "sendrecv"
                  : tr.direction() == sdp_direction::sendonly ? "sendonly"
                  : tr.direction() == sdp_direction::recvonly ? "recvonly"
                                                              : "inactive")
              << " mid=" << tr.mid() << " codecs=" << 1
              << " sender_ssrc=" << tr.sender().ssrc(0)
              << " num_streams=" << tr.sender().num_streams() << '\n';

    co_await conn.set_local_description(
        co_await conn.create_answer(net::use_awaitable), net::use_awaitable);

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
            co_await timer.async_wait(net::use_awaitable);
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
        co_await conn.on_connection_state_changed(net::use_awaitable);
    if (conn.connection_state() != connection_state_t::connected) {
        std::cerr << "Failed to connect\n";
        co_return;
    }
    std::cout << "Connected: SFU forwarding active (ctrl-c to stop)\n";

    timer.expires_after(std::chrono::seconds(3600));
    co_await timer.async_wait(net::use_awaitable);

    std::cout << "Done\n";
}

static net::awaitable<void> http_session(net::io_context &ctx,
                                         net::ip::tcp::socket sock) {
    beast::flat_buffer buf;
    http::request<http::string_body> req;
    auto n = co_await http::async_read(sock, buf, req, net::use_awaitable);

    if (websocket::is_upgrade(req)) {
        auto ws = std::make_shared<ws_t>(beast::tcp_stream(std::move(sock)));
        auto [wec] =
            co_await ws->async_accept(req, net::as_tuple(net::use_awaitable));
        if (wec) {
            std::cerr << "ws accept: " << wec.message() << '\n';
            co_return;
        }
        co_await sfu_session(ctx, std::move(ws));
    } else {
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::content_type, "text/html");
        static constexpr char html[] = {
#embed "sfu.html"
            , '\0'};
        res.body() = html;
        res.prepare_payload();
        auto [sec, _] = co_await http::async_write(
            sock, res, net::as_tuple(net::use_awaitable));
        if (sec)
            std::cerr << "http write: " << sec.message() << '\n';
    }
}

static net::awaitable<void> listener(net::io_context &ctx) {
    net::ip::tcp::acceptor acc(
        ctx, net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), PORT));
    std::cout << "Server on ws://localhost:" << PORT << "/ws\n";
    while (true) {
        auto [ec, sock] =
            co_await acc.async_accept(net::as_tuple(net::use_awaitable));
        if (ec)
            continue;
        net::co_spawn(ctx, http_session(ctx, std::move(sock)), net::detached);
    }
}

int main() {
    net::io_context ctx;
    net::co_spawn(ctx, listener(ctx), net::detached);
    ctx.run();
}

#include "asiortc.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/steady_timer.hpp>
#include <asio/detached.hpp>
namespace asiortc {
namespace net = asio;
}
#else
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/detached.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#endif

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

static asiortc::net::awaitable<void>
wait_connected(asiortc::peer_connection &pc) {
    while (pc.connection_state() != asiortc::connection_state_t::connected &&
           pc.connection_state() != asiortc::connection_state_t::failed)
        co_await pc.on_connection_state_changed(asiortc::net::use_awaitable);
    if (pc.connection_state() != asiortc::connection_state_t::connected)
        throw std::runtime_error{"connection failed"};
}

asiortc::net::awaitable<void> run() {
    auto ex = co_await asiortc::net::this_coro::executor;

    asiortc::peer_connection peer1(ex);
    asiortc::peer_connection peer2(ex);

    asiortc::data_channel_interface ch2;
    peer2.on_data_channel(
        [&](asiortc::data_channel_interface dc) { ch2 = dc; });

    auto ch1 = peer1.create_data_channel("chat");

    peer1.on_candidates([&](std::vector<asiortc::candidate> cs) {
        asiortc::net::co_spawn(
            ex,
            [](std::vector<asiortc::candidate> cs,
               asiortc::peer_connection &peer2)
                -> asiortc::net::awaitable<void> {
                if (cs.empty()) {
                    co_await peer2.add_ice_candidate(
                        asiortc::net::use_awaitable);
                    co_return;
                }
                for (auto &c : cs)
                    co_await peer2.add_ice_candidate(
                        std::move(c), asiortc::net::use_awaitable);
            }(std::move(cs), peer2),
            [](std::exception_ptr p) {
                if (!p)
                    return;
                try {
                    std::rethrow_exception(p);
                } catch (const std::exception &e) {
                    std::cerr << "Add candidates to peer2 failed: " << e.what()
                              << '\n';
                }
            });
    });

    peer2.on_candidates([&](std::vector<asiortc::candidate> cs) {
        asiortc::net::co_spawn(
            ex,
            [](std::vector<asiortc::candidate> cs,
               asiortc::peer_connection &peer1)
                -> asiortc::net::awaitable<void> {
                if (cs.empty()) {
                    co_await peer1.add_ice_candidate(
                        asiortc::net::use_awaitable);
                    co_return;
                }
                for (auto &c : cs)
                    co_await peer1.add_ice_candidate(
                        std::move(c), asiortc::net::use_awaitable);
            }(std::move(cs), peer1),
            [](std::exception_ptr p) {
                if (!p)
                    return;
                try {
                    std::rethrow_exception(p);
                } catch (const std::exception &e) {
                    std::cerr << "Add candidates to peer1 failed: " << e.what()
                              << '\n';
                }
            });
    });

    std::unique_ptr<asiortc::session_description_interface> offer =
        co_await peer1.create_offer(asiortc::net::use_awaitable);
    if (!offer)
        throw std::runtime_error{"create_offer failed"};
    std::string offer_str = offer->to_string();
    std::cout << "[OFFER]:\n" << offer_str << "\n\n";
    co_await peer1.set_local_description(std::move(offer),
                                         asiortc::net::use_awaitable);

    std::unique_ptr<asiortc::session_description_interface> remote_offer =
        asiortc::parse_sdp(offer_str, "offer");
    if (!remote_offer)
        throw std::runtime_error{"parse_sdp(remote_offer) failed"};
    co_await peer2.set_remote_description(std::move(remote_offer),
                                          asiortc::net::use_awaitable);
    auto answer = co_await peer2.create_answer(asiortc::net::use_awaitable);
    if (!answer)
        throw std::runtime_error{"create_answer failed"};
    std::string answer_str = answer->to_string();
    std::cout << "[ANSWER]:\n" << offer_str << "\n\n";
    co_await peer2.set_local_description(std::move(answer),
                                         asiortc::net::use_awaitable);

    auto remote_answer = asiortc::parse_sdp(answer_str, "answer");
    co_await peer1.set_remote_description(std::move(remote_answer),
                                          asiortc::net::use_awaitable);

    co_await wait_connected(peer1);
    co_await wait_connected(peer2);

    co_await ch1.open(asiortc::net::use_awaitable);

    // wait for the remote data channel to arrive
    asiortc::net::steady_timer timer{ex};
    for (int i = 0; i < 20 && !ch2; ++i) {
        timer.expires_after(std::chrono::milliseconds(500));
        co_await timer.async_wait(asiortc::net::use_awaitable);
    }
    if (!ch2)
        throw std::runtime_error{"no remote data channel"};

    // peer1 → peer2
    co_await ch1.send("hello from peer1", asiortc::net::use_awaitable);
    auto msg = co_await ch2.read(asiortc::net::use_awaitable);
    std::cout << "peer2 received: " << msg.text_data() << '\n';

    // peer2 → peer1
    co_await ch2.send("hello from peer2", asiortc::net::use_awaitable);
    msg = co_await ch1.read(asiortc::net::use_awaitable);
    std::cout << "peer1 received: " << msg.text_data() << '\n';

    std::cout << "datachannel communication OK\n";
}

int main() {
    asiortc::net::io_context ctx;
    asiortc::net::co_spawn(ctx, run(), [](std::exception_ptr p) {
        if (!p)
            return;
        try {
            std::rethrow_exception(p);
        } catch (const std::exception &e) {
            std::cerr << "Unhandled exception: " << e.what() << '\n';
        }
    });
    ctx.run();
}

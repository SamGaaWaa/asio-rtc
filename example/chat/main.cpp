#include "asioice/detail/on_scope_empty.hpp"
#include "asiortc.hpp"

#if ASIORTC_USE_STANDALONE_ASIO == 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#else
#include <asio/as_tuple.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#endif

#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>

asiortc::task<void> co_main(asiortc::net::io_context &ctx) {
    using namespace asiortc;
    exec::async_scope scope;

    configuration cfg;
    peer_connection offerer(ctx.get_executor(), cfg);
    peer_connection answerer(ctx.get_executor(), cfg);

    std::deque<std::string> offer_cands;
    std::deque<std::string> answer_cands;

    offerer.on_candidates([&](std::vector<candidate> cs) {
        if (cs.empty()) {
            offer_cands.push_back("");
            return;
        }
        for (const auto &c : cs)
            offer_cands.push_back(c.to_sdp());
    });
    answerer.on_candidates([&](std::vector<candidate> cs) {
        if (cs.empty()) {
            answer_cands.push_back("");
            return;
        }
        for (const auto &c : cs)
            answer_cands.push_back(c.to_sdp());
    });

    auto offerer_signal_task = [&]() -> asiortc::task<void> {
        net::steady_timer t(ctx);
        while (true) {
            while (offer_cands.empty()) {
                t.expires_after(std::chrono::seconds(1));
                co_await t.async_wait(utils::use_sender);
            }
            while (!offer_cands.empty()) {
                auto sdp = std::move(offer_cands.front());
                offer_cands.pop_front();
                if (sdp.empty()) {
                    std::cout << "offerer finished gathering\n";
                    co_await answerer.add_ice_candidate();
                    co_return;
                }
                auto c = candidate::from_sdp(sdp);
                if (c)
                    co_await answerer.add_ice_candidate(*std::move(c));
                else {
                    std::cerr << "Parse candidate sdp failed: " << sdp << '\n';
                }
            }
        }
    };

    auto answerer_signal_task = [&]() -> asiortc::task<void> {
        net::steady_timer t(ctx);
        while (true) {
            while (answer_cands.empty()) {
                t.expires_after(std::chrono::seconds(1));
                co_await t.async_wait(utils::use_sender);
            }
            while (!answer_cands.empty()) {
                auto sdp = std::move(answer_cands.front());
                answer_cands.pop_front();
                if (sdp.empty()) {
                    std::cout << "answerer finished gathering\n";
                    co_await offerer.add_ice_candidate();
                    co_return;
                }
                auto c = candidate::from_sdp(sdp);
                if (c)
                    co_await offerer.add_ice_candidate(*std::move(c));
                else {
                    std::cerr << "Parse candidate sdp failed: " << sdp << '\n';
                }
            }
        }
    };

    scope.spawn(offerer_signal_task());
    scope.spawn(answerer_signal_task());

    answerer.on_data_channel([&scope](data_channel_interface rch) {
        std::cout << "Answerer got remote dc: " << rch.label()
                  << " stream=" << rch.stream_id() << "\n";
        scope.spawn([](auto ch) -> task<void> {
            data_channel_message msg = co_await ch.read();
            if (!msg.is_binary()) {
                std::cout << "Answerer recv: " << msg.text_data() << "\n";
            }
            co_await ch.send("hello from answerer");
            std::cout << "Answerer sent: hello from answerer\n";
        }(std::move(rch)));
    });

    data_channel_interface chat_ch = offerer.create_data_channel("chat");

    try {
        auto offer = co_await offerer.create_offer();
        std::string offer_sdp = offer.to_string();
        co_await offerer.set_local_description(std::move(offer));
        co_await answerer.set_remote_description(parse_sdp(offer_sdp, "offer"));
        auto answer = co_await answerer.create_answer();
        std::string answer_sdp = answer.to_string();
        co_await answerer.set_local_description(std::move(answer));
        co_await offerer.set_remote_description(
            parse_sdp(answer_sdp, "answer"));
    } catch (const std::exception &e) {
        std::cout << "Unhandle exception: " << e.what() << '\n';
        goto END;
    }

    while (offerer.connection_state() != connection_state_t::connected &&
           offerer.connection_state() != connection_state_t::failed)
        co_await offerer.on_connection_state_changed();
    while (answerer.connection_state() != connection_state_t::connected &&
           answerer.connection_state() != connection_state_t::failed)
        co_await answerer.on_connection_state_changed();
    if (offerer.connection_state() == connection_state_t::connected &&
        answerer.connection_state() == connection_state_t::connected) {
        std::cout << "Peer connection connected\n";
    } else {
        std::cerr << "Peer connection failed to connect\n";
    }

    {
        auto &dc = chat_ch;
        if (!co_await dc.open()) {
            std::cerr << "Failed to open datachannel\n";
            goto END;
        }
        std::cout << "Local DataChannel created: " << dc.label() << "\n";
        co_await dc.send("hello from offerer");
        std::cout << "Sent: hello from offerer\n";
        auto msg = co_await dc.read();
        if (!msg.is_binary()) {
            std::cout << "Offerer recv: " << msg.text_data() << "\n";
        }
        scope.request_stop();
    }
END:
    co_await (asioice::utils::on_scope_empty(scope) |
              stdexec::continues_on(offerer.get_scheduler()));
}

void test() {
    asiortc::net::io_context ctx;
    exec::start_detached(co_main(ctx));
    ctx.run();
}

int main() { test(); }
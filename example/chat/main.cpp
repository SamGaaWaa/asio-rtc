#include "asioice/detail/on_scope_empty.hpp"
#include "connection_impl.hpp"
#include "sdp.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
namespace asioice {
namespace net = boost::asio;
}
#else
#include <asio/as_tuple.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
namespace asioice {
namespace net = asio;
}
#endif

#include <chrono>
#include <deque>
#include <exec/async_scope.hpp>
#include <iostream>
#include <memory>
#include <stdexec/execution.hpp>
#include <string>
#include <vector>

using namespace asioice;
using namespace asiortc;

asioice::task<void> co_main(net::io_context &ctx) {
    exec::async_scope scope;

    configuration cfg;
    auto offerer = std::make_shared<connection_impl>(ctx.get_executor(), cfg);
    auto answerer = std::make_shared<connection_impl>(ctx.get_executor(), cfg);

    std::deque<std::string> offer_cands;
    std::deque<std::string> answer_cands;

    offerer->on_candidates([&](std::span<const asioice::candidate> cs) {
        if (cs.empty()) {
            offer_cands.push_back("");
            return;
        }
        for (const auto &c : cs)
            offer_cands.push_back(c.to_sdp());
    });
    answerer->on_candidates([&](std::span<const asioice::candidate> cs) {
        if (cs.empty()) {
            answer_cands.push_back("");
            return;
        }
        for (const auto &c : cs)
            answer_cands.push_back(c.to_sdp());
    });

    auto offerer_signal_task = [&]() -> asioice::task<void> {
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
                    co_await answerer->add_ice_candidate();
                    co_return;
                }
                auto c = candidate::from_sdp(sdp);
                if (c)
                    co_await answerer->add_ice_candidate(*std::move(c));
                else {
                    std::cerr << "Parse candidate sdp failed: " << sdp << '\n';
                }
            }
        }
    };

    auto answerer_signal_task = [&]() -> asioice::task<void> {
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
                    co_await offerer->add_ice_candidate();
                    co_return;
                }
                auto c = candidate::from_sdp(sdp);
                if (c)
                    co_await offerer->add_ice_candidate(*std::move(c));
                else {
                    std::cerr << "Parse candidate sdp failed: " << sdp << '\n';
                }
            }
        }
    };

    scope.spawn(offerer_signal_task());
    scope.spawn(answerer_signal_task());

    answerer->on_remote_channel(
        [&scope](std::shared_ptr<asiortc::data_channel> rch) {
            std::cout << "Answerer got remote dc: " << rch->label()
                      << " stream=" << rch->stream_id() << "\n";
            scope.spawn([](auto ch) -> task<void> {
                auto msg = co_await ch->read();
                if (!msg.binary) {
                    std::cout << "Answerer recv: "
                              << std::string_view{(const char *)msg.data.data(),
                                                  msg.data.size()}
                              << "\n";
                }
                co_await ch->send_text("hello from answerer");
                std::cout << "Answerer sent: hello from answerer\n";
            }(std::move(rch)));
        });

    auto chat_ch = offerer->create_data_channel("chat");

    try {
        auto offer = co_await offerer->create_offer();
        std::string offer_sdp = offer.to_string();
        co_await offerer->set_local_description(std::move(offer));
        co_await answerer->set_remote_description(
            parse_sdp(offer_sdp, "offer"));
        auto answer = co_await answerer->create_answer();
        std::string answer_sdp = answer.to_string();
        co_await answerer->set_local_description(std::move(answer));
        co_await offerer->set_remote_description(
            parse_sdp(answer_sdp, "answer"));
    } catch (const std::exception &e) {
        std::cout << "Unhandle exception: " << e.what() << '\n';
        goto END;
    }

    while (offerer->connection_state() != connection_state_t::connected &&
           offerer->connection_state() != connection_state_t::failed)
        co_await offerer->on_connection_state_changed();
    while (answerer->connection_state() != connection_state_t::connected &&
           answerer->connection_state() != connection_state_t::failed)
        co_await answerer->on_connection_state_changed();
    if (offerer->connection_state() == connection_state_t::connected &&
        answerer->connection_state() == connection_state_t::connected) {
        std::cout << "Peer connection connected\n";
    } else {
        std::cerr << "Peer connection failed to connect\n";
    }

    {
        auto &dc = chat_ch;
        if (!co_await dc->open()) {
            std::cerr << "Failed to open datachannel\n";
            goto END;
        }
        std::cout << "Local DataChannel created: " << dc->label() << "\n";
        co_await dc->send_text("hello from offerer");
        std::cout << "Sent: hello from offerer\n";
        auto msg = co_await dc->read();
        if (!msg.binary) {
            std::cout << "Offerer recv: "
                      << std::string_view{(const char *)msg.data.data(),
                                          msg.data.size()}
                      << "\n";
        }
        scope.request_stop();
    }
END:
    co_await offerer->close();
    co_await answerer->close();
    co_await (utils::on_scope_empty(scope) |
              stdexec::continues_on(utils::scheduler{ctx}));
}

void test() {
    net::io_context ctx;
    exec::start_detached(co_main(ctx));
    ctx.run();
}

int main() { test(); }
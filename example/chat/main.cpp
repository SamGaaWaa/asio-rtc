#include "asiortc.hpp"
#include "asioice/detail/on_scope_empty.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <algorithm>

using namespace asiortc;

namespace net = boost::asio;

// ── colors ────────────────────────────────────────────────────

static const char *GREEN = "\033[32m";
static const char *WHITE = "\033[37m";
static const char *RESET = "\033[0m";

// ── global stdin pool ──────────────────────────────────────────

net::thread_pool stdin_pool{1};

// ── SDP escaping ───────────────────────────────────────────────

static std::string enc(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '\r')
            r += "\\r";
        else if (c == '\n')
            r += "\\n";
        else if (c == '\\')
            r += "\\\\";
        else
            r += c;
    }
    return r;
}

static std::string dec(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'r') {
                r += '\r';
                ++i;
            } else if (s[i + 1] == 'n') {
                r += '\n';
                ++i;
            } else if (s[i + 1] == '\\') {
                r += '\\';
                ++i;
            } else {
                r += s[i];
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

// ── random username ────────────────────────────────────────────

static std::string random_username() {
    static const char *adjs[] = {
        "blue", "red",  "green", "silver", "gold",  "dark",  "light", "wild",
        "calm", "bold", "quick", "sharp",  "happy", "lucky", "sunny", "cool"};
    static const char *nouns[] = {"cat",  "dog",  "fox",  "owl",
                                  "bear", "hawk", "wolf", "deer",
                                  "fish", "bird", "lion", "tiger"};
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> adj_dist(0, 15);
    std::uniform_int_distribution<int> noun_dist(0, 11);
    std::uniform_int_distribution<int> num_dist(10, 99);
    return std::string(adjs[adj_dist(rng)]) + "_" + nouns[noun_dist(rng)] +
           std::to_string(num_dist(rng));
}

// ── stdin read ─────────────────────────────────────────────────

static task<std::string> read_line(auto &ctx) {
    std::string line;
    auto guard = net::make_work_guard(ctx);
    co_await stdexec::starts_on(asiortc::utils::scheduler{ctx},
                                net::post(
                                    [&line] {
                                        std::getline(std::cin, line);
                                        if (!line.empty() &&
                                            line.back() == '\r')
                                            line.pop_back();
                                    },
                                    stdin_pool, asiortc::utils::use_sender));
    co_return line;
}

static task<std::string> prompt(auto &ctx, std::string_view msg) {
    std::cout << msg << std::flush;
    return read_line(ctx);
}

// ── multi-line SDP reader ──────────────────────────────────────

static task<std::string> read_sdp(auto &ctx) {
    std::cout << "\nPaste SDP (empty line to finish):\n";
    std::ostringstream oss;
    while (true) {
        auto line = co_await read_line(ctx);
        if (line.empty())
            break;
        oss << line << "\r\n";
    }
    std::string raw = std::move(oss).str();
    if (raw.empty())
        co_return "";
    co_return dec(raw);
}

// ── ICE gathering helper ───────────────────────────────────────

static task<void> wait_ice_gather(peer_connection &conn, net::io_context &ctx) {
    net::steady_timer t(ctx);
    for (int i = 0; i < 20 && conn.ice_gathering_state() !=
                                  ice_gathering_state_t::complete;
         ++i) {
        t.expires_after(std::chrono::seconds(1));
        co_await t.async_wait(asiortc::utils::use_sender);
    }
}

// ── chat session ───────────────────────────────────────────────

static task<void> chat_session(net::io_context &ctx) try {
    auto sched = asiortc::utils::scheduler{ctx};

    auto role_str = co_await prompt(ctx, "\nRole? [o]ffer / [a]nswer: ");
    bool is_offer =
        !role_str.empty() && (role_str[0] == 'o' || role_str[0] == 'O');

    auto username = co_await prompt(ctx, "Username (enter for random): ");
    if (username.empty())
        username = random_username();
    std::cout << "\nYou: " << GREEN << username << RESET << '\n';

    peer_connection conn(ctx.get_executor());
    data_channel_interface chat_ch;

    if (!is_offer) {
        conn.on_data_channel([&](data_channel_interface dc) { chat_ch = dc; });
    }

    std::string local_sdp_str;
    if (is_offer) {
        chat_ch = conn.create_data_channel("chat");
        auto offer = co_await conn.create_offer();
        co_await conn.set_local_description(std::move(offer));

        co_await wait_ice_gather(conn, ctx);
        local_sdp_str = conn.local_description()->to_string();
        std::cout << "\n--- Copy offer to answer side ---\n"
                  << enc(local_sdp_str) << "\n\n";
    }

    auto remote_sdp_str = co_await read_sdp(ctx);
    if (remote_sdp_str.empty()) {
        std::cout << "No SDP received, exiting\n";
        co_return;
    }

    std::string remote_type = is_offer ? "answer" : "offer";
    co_await conn.set_remote_description(
        parse_sdp(remote_sdp_str, remote_type).value());

    if (!is_offer) {
        auto answer = co_await conn.create_answer();
        co_await conn.set_local_description(std::move(answer));

        co_await wait_ice_gather(conn, ctx);
        std::cout << "\n--- Copy answer to offer side ---\n"
                  << enc(conn.local_description()->to_string()) << "\n\n";
    }

    std::cout << "Connecting...\n";
    while (conn.connection_state() != connection_state_t::connected &&
           conn.connection_state() != connection_state_t::failed)
        co_await conn.on_connection_state_changed();

    if (conn.connection_state() != connection_state_t::connected) {
        std::cout << "Connection failed\n";
        co_return;
    }
    std::cout << "Connected!\n";

    if (!chat_ch) {
        if (!is_offer) {
            net::steady_timer dc_timer(ctx);
            for (int i = 0; i < 10 && !chat_ch; ++i) {
                dc_timer.expires_after(std::chrono::milliseconds(500));
                co_await dc_timer.async_wait(asiortc::utils::use_sender);
            }
        }
        if (!chat_ch) {
            std::cout << "No DataChannel\n";
            co_return;
        }
    }
    if (!co_await chat_ch.open()) {
        std::cout << "DataChannel open failed\n";
        co_return;
    }

    std::cout << "Chat started.  Type /quit to exit.\n"
              << GREEN << "[" << username << "] >>> " << RESET << std::flush;

    exec::async_scope scope;
    scope.spawn(stdexec::starts_on(
        sched, [](data_channel_interface ch, std::string name) -> task<void> {
            while (true) {
                auto msg = co_await ch.read();
                std::cout << WHITE << "\n[peer] " << RESET << msg.text_data()
                          << '\n'
                          << GREEN << "[" << name << "] >>> " << RESET
                          << std::flush;
            }
        }(chat_ch, username)));

    while (true)
        try {
            auto line = co_await read_line(ctx);
            if (line == "/quit")
                break;
            if (line.empty()) {
                std::cout << GREEN << "[" << username << "] >>> " << RESET
                          << std::flush;
                continue;
            }
            co_await chat_ch.send(line);
            std::cout << GREEN << "[" << username << "] >>> " << RESET
                      << std::flush;
        } catch (const std::exception &e) {
            std::cout << "\nError: " << e.what() << "\n";
            break;
        }

    scope.request_stop();
    co_await (asioice::utils::on_scope_empty(scope) |
              stdexec::continues_on(sched));
    std::cout << "\nChat ended.\n";
} catch (const std::exception &e) {
    std::cout << "\nError: " << e.what() << "\n";
}

int main(int argc, char **argv) {
    net::io_context ctx;
    if (argc > 1 && std::any_of(argv + 1, argv + argc, [](const char *cmd) {
            return std::string_view{cmd} == "--log";
        })) {
        asiortc::set_logger(std::make_shared<asiortc::logger_interface>(
                                asiortc::log_level::info),
                            ctx.get_executor());
    }
    std::cout << std::unitbuf;
    exec::start_detached(
        stdexec::starts_on(asiortc::utils::scheduler{ctx}, chat_session(ctx)));
    ctx.run();
    stdin_pool.join();
}

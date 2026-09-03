#include "asiortc.hpp"
#include "asioice/detail/on_scope_empty.hpp"
#include "asioice/detail/stack_resource.hpp"

#include "hash.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <boost/hash2/sha2.hpp>

#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace asiortc;

namespace net = boost::asio;

namespace {

constexpr std::size_t DIGEST_SIZE = 32;

struct bench_config {
    uint64_t total_bytes = 1ULL * 1024 * 1024 * 1024;
    std::size_t message_size = 128 * 1024;
};

struct bench_result {
    double send_elapsed = 0.0;
    double recv_elapsed = 0.0;
    std::size_t recv_bytes = 0;
};

boost::hash2::sha2_256::result_type send_hash;
boost::hash2::sha2_256::result_type recv_hash;

task<void> wait_ice_gather(peer_connection &conn, net::io_context &ctx) {
    net::steady_timer t(ctx);
    for (int i = 0; i < 20 && conn.ice_gathering_state() !=
                                  ice_gathering_state_t::complete;
         ++i) {
        t.expires_after(std::chrono::seconds(1));
        co_await t.async_wait(utils::use_sender);
    }
    if (conn.ice_gathering_state() != ice_gathering_state_t::complete)
        throw std::runtime_error{"ice gathering timeout"};
}

task<void> wait_connected(peer_connection &conn, net::io_context &ctx) {
    net::steady_timer t(ctx);
    for (int i = 0; i < 20; ++i) {
        if (conn.connection_state() == connection_state_t::connected)
            co_return;
        if (conn.connection_state() == connection_state_t::failed)
            throw std::runtime_error{"connection failed"};
        t.expires_after(std::chrono::seconds(1));
        co_await t.async_wait(utils::use_sender);
    }
    throw std::runtime_error{"connection timeout"};
}

task<void> sender_task(data_channel_interface ch, const bench_config &cfg,
                       uint64_t messages, double *elapsed) {
    std::vector<char> mem(16 * 1024);
    asioice::utils::stack_resource res{mem.data(), mem.size()};
    std::pmr::polymorphic_allocator<std::byte> alloc{&res};

    const auto bind_alloc = [&alloc]<class S>(S &&s) {
        return stdexec::write_env(std::forward<S>(s),
                                  stdexec::prop{stdexec::get_allocator, alloc});
    };

    boost::hash2::sha2_256 hash;
    std::vector<uint8_t> buf(cfg.message_size);
    asioice::hash::random_bytes(buf.data(), buf.size());

    auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < messages; ++i) {
        std::memcpy(buf.data(), &i, sizeof(i));
        hash.update(buf.data(), buf.size());
        if (!co_await bind_alloc(ch.send(std::span<const uint8_t>{buf})))
            throw std::runtime_error{"send failed"};
    }

    send_hash = hash.result();
    auto end = std::chrono::steady_clock::now();
    *elapsed = std::chrono::duration<double>(end - start).count();

    std::cout << "Allocated in heap: " << res.allocated_in_heap() << '\n';
    std::cout << "Allocated in stack: " << res.allocated_in_stack() << '\n';
}

task<void> receiver_task(data_channel_interface ch, uint64_t messages,
                         bench_result *result) {
    boost::hash2::sha2_256 hash;

    auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < messages; ++i) {
        auto msg = co_await ch.read();
        auto data = msg.binary_data();
        hash.update(data.data(), data.size());
        result->recv_bytes += data.size();
    }

    auto end = std::chrono::steady_clock::now();
    result->recv_elapsed = std::chrono::duration<double>(end - start).count();

    recv_hash = hash.result();
}

task<void> benchmark(net::io_context &ctx, bench_config cfg) {
    auto sched = utils::scheduler{ctx};

    peer_connection peer1(ctx.get_executor());
    peer_connection peer2(ctx.get_executor());

    data_channel_interface ch2;
    peer2.on_data_channel([&](data_channel_interface dc) { ch2 = dc; });

    auto ch1 = peer1.create_data_channel("benchmark");

    auto offer = co_await peer1.create_offer();
    co_await peer1.set_local_description(std::move(offer));
    co_await wait_ice_gather(peer1, ctx);
    std::string offer_str = peer1.local_description()->to_string();

    auto remote_offer = parse_sdp(offer_str, "offer");
    if (!remote_offer)
        throw std::runtime_error{"parse_sdp offer failed"};
    co_await peer2.set_remote_description(std::move(remote_offer));

    auto answer = co_await peer2.create_answer();
    co_await peer2.set_local_description(std::move(answer));
    co_await wait_ice_gather(peer2, ctx);
    std::string answer_str = peer2.local_description()->to_string();

    auto remote_answer = parse_sdp(answer_str, "answer");
    if (!remote_answer)
        throw std::runtime_error{"parse_sdp answer failed"};
    co_await peer1.set_remote_description(std::move(remote_answer));

    co_await wait_connected(peer1, ctx);
    co_await wait_connected(peer2, ctx);

    if (!co_await ch1.open())
        throw std::runtime_error{"data channel open failed"};

    net::steady_timer timer(ctx);
    for (int i = 0; i < 20 && !ch2; ++i) {
        timer.expires_after(std::chrono::milliseconds(500));
        co_await timer.async_wait(utils::use_sender);
    }
    if (!ch2)
        throw std::runtime_error{"no remote data channel"};

    uint64_t messages = cfg.total_bytes / cfg.message_size;
    uint64_t total = messages * cfg.message_size;
    if (messages == 0)
        throw std::runtime_error{"total_bytes must be >= message_size"};

    std::cout << "message size: " << cfg.message_size << " bytes\n";
    std::cout << "messages: " << messages << '\n';
    std::cout << "total bytes: " << total << '\n';

    bench_result result;

    exec::async_scope scope;
    scope.spawn(stdexec::starts_on(
        sched,
        [](data_channel_interface ch, bench_config cfg, uint64_t messages,
           bench_result *result) -> task<void> {
            co_await sender_task(ch, cfg, messages, &result->send_elapsed);
        }(ch1, cfg, messages, &result)));
    scope.spawn(
        stdexec::starts_on(sched,
                           [](data_channel_interface ch, uint64_t messages,
                              bench_result *result) -> task<void> {
                               co_await receiver_task(ch, messages, result);
                           }(ch2, messages, &result)));

    co_await (asioice::utils::on_scope_empty(scope) |
              stdexec::continues_on(sched));

    double send_mbps = result.send_elapsed > 0.0
                           ? (total / result.send_elapsed) / (1024.0 * 1024.0)
                           : 0.0;
    double recv_mbps =
        result.recv_elapsed > 0.0
            ? (result.recv_bytes / result.recv_elapsed) / (1024.0 * 1024.0)
            : 0.0;

    std::cout << "send: " << result.send_elapsed << " s (" << send_mbps
              << " MiB/s)\n";
    std::cout << "recv: " << result.recv_elapsed << " s (" << recv_mbps
              << " MiB/s)\n";
    std::cout << "data verified (boost::hash2): "
              << (send_hash == recv_hash ? "OK" : "MISMATCH") << '\n';

    if (send_hash != recv_hash)
        throw std::runtime_error{"data integrity check failed"};

    peer1.close();
    peer2.close();
}

} // namespace

int main(int argc, char **argv) {
    bench_config cfg;
    bool log = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--log") {
            log = true;
        } else if (arg == "--bytes" && i + 1 < argc) {
            cfg.total_bytes = std::stoull(argv[++i]);
        } else if (arg == "--message-size" && i + 1 < argc) {
            cfg.message_size = std::stoull(argv[++i]);
        } else {
            std::cerr << "usage: " << argv[0]
                      << " [--bytes N] [--message-size S] [--log]\n";
            return 1;
        }
    }

    std::cout << std::unitbuf;
    net::io_context ctx;
    if (log)
        asiortc::set_logger(std::make_shared<logger_interface>(log_level::info),
                            ctx.get_executor());

    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, benchmark(ctx, cfg)));
    ctx.run();
}

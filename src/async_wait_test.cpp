#include "asiortc/detail/async_wait.hpp"
#include "asiortc/detail/use_sender.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#include <asio/io_context.hpp>
#include <asio/bind_executor.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/use_future.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/redirect_error.hpp>
#include <asio/detached.hpp>
namespace asiortc {
namespace net = asio;
}
#else
#include <boost/asio/io_context.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/detached.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#endif
#include <boost/core/lightweight_test.hpp>

#include <exec/single_thread_context.hpp>
#include <exec/start_detached.hpp>

#include <future>
#include <iostream>
#include <cassert>

void finished_inline() {
    int res = 0;

    asiortc::net::io_context ctx;
    asiortc::utils::async_wait<void(int)>(
        stdexec::just(12), [&res](asiortc::error_code ec, int n) {
            if (!ec)
                res = n;
        });
    ctx.run();

    assert(res == 12);
}

void context_keep_running() {
    int res = 0;

    exec::single_thread_context th;
    auto work = stdexec::starts_on(th.get_scheduler(),
                                   stdexec::just() | stdexec::let_value([] {
                                       std::this_thread::sleep_for(
                                           std::chrono::milliseconds(10));
                                       return stdexec::just(12);
                                   }));

    asiortc::net::io_context ctx;
    asiortc::utils::async_wait<void(int)>(
        std::move(work),
        asiortc::net::bind_executor(ctx, [&res](asiortc::error_code ec, int n) {
            if (!ec)
                res = n;
        }));
    ctx.run();

    assert(res == 12);
}

void cancell_the_work() {
    bool cancelled = false;

    exec::single_thread_context th;
    auto work = stdexec::starts_on(
        th.get_scheduler(),
        stdexec::get_stop_token() |
            stdexec::let_value([&cancelled](auto token) {
                assert(!token.stop_requested());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                cancelled = token.stop_requested();
                return stdexec::just();
            }));

    asiortc::net::io_context ctx;
    asiortc::net::cancellation_signal signal;

    auto handler =
        asiortc::net::bind_executor(ctx, [](asiortc::error_code ec) {});
    auto handler1 =
        asiortc::net::bind_cancellation_slot(signal.slot(), std::move(handler));
    asiortc::utils::async_wait<void()>(std::move(work), std::move(handler1));

    std::thread([&signal] {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        signal.emit(asiortc::net::cancellation_type::terminal);
    }).detach();

    ctx.run();

    assert(cancelled == true);
}

void use_sender() {
    int res = 0;

    exec::single_thread_context th;
    auto work = stdexec::starts_on(th.get_scheduler(),
                                   stdexec::just() | stdexec::let_value([] {
                                       std::this_thread::sleep_for(
                                           std::chrono::milliseconds(10));
                                       return stdexec::just(12);
                                   }));

    asiortc::net::io_context ctx;
    auto sndr = asiortc::utils::async_wait<void(int)>(
        std::move(work),
        asiortc::net::bind_executor(ctx, asiortc::utils::use_sender));
    exec::start_detached(std::move(sndr) |
                         stdexec::then([&res](auto ec, int n) {
                             if (!ec)
                                 res = n;
                         }));
    ctx.run();

    assert(res == 12);
}

void use_future_token() {
    int res = 0;

    exec::single_thread_context th;
    auto work = stdexec::starts_on(th.get_scheduler(),
                                   stdexec::just() | stdexec::let_value([] {
                                       std::this_thread::sleep_for(
                                           std::chrono::milliseconds(10));
                                       return stdexec::just(12);
                                   }));

    asiortc::net::io_context ctx;
    std::future<int> f = asiortc::utils::async_wait<void(int)>(
        std::move(work),
        asiortc::net::bind_executor(ctx, asiortc::net::use_future));
    ctx.run();

    assert(f.get() == 12);
}

asiortc::net::awaitable<void> awaitable_impl(int &res) {
    res = co_await asiortc::utils::async_wait<void(int)>(
        stdexec::just(42), asiortc::net::use_awaitable);
}

void use_awaitable_token() {
    int res = 0;

    asiortc::net::io_context ctx;
    asiortc::net::co_spawn(ctx, awaitable_impl(res), asiortc::net::detached);
    ctx.run();

    assert(res == 42);
}

void deferred_token() {
    int res = 0;

    asiortc::net::io_context ctx;
    auto d = asiortc::utils::async_wait<void(int)>(stdexec::just(42),
                                                   asiortc::net::deferred);

    std::move(d)(
        asiortc::net::bind_executor(ctx, [&](asiortc::error_code ec, int n) {
            if (!ec)
                res = n;
        }));

    ctx.run();
    assert(res == 42);
}

void use_any_sender_token() {
    int res = 0;

    asiortc::net::io_context ctx;
    auto sndr = asiortc::utils::async_wait<void(int)>(
        stdexec::just(12),
        asiortc::net::bind_executor(ctx, asioice::utils::use_any_sender));
    exec::start_detached(std::move(sndr) | stdexec::then([&](auto ec, int n) {
                             if (!ec)
                                 res = n;
                         }));
    ctx.run();

    assert(res == 12);
}

void redirect_error_token() {
    asiortc::net::io_context ctx;

    int res = 0;
    asiortc::error_code success_ec;
    asiortc::utils::async_wait<void(int)>(
        stdexec::just(42),
        asiortc::net::redirect_error(
            asiortc::net::bind_executor(ctx, [&](int n) { res = n; }),
            success_ec));
    ctx.run();
    assert(!success_ec && res == 42);

    ctx.restart();
    res = 0;
    asiortc::error_code err_ec;
    asiortc::utils::async_wait<void(int)>(
        stdexec::just_error(
            std::make_exception_ptr(std::runtime_error("error"))),
        asiortc::net::redirect_error(
            asiortc::net::bind_executor(ctx, [&](int n) { res = n; }), err_ec));
    ctx.run();
    assert(err_ec && res == 0);
}

int main() {
    finished_inline();
    context_keep_running();
    cancell_the_work();
    use_sender();
    use_future_token();
    use_awaitable_token();
    deferred_token();
    use_any_sender_token();
    redirect_error_token();
    std::cout << "All test passed\n";
}
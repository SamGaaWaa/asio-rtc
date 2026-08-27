#pragma once

#include "asiortc/config.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#include <asio/associated_allocator.hpp>
#include <asio/associated_executor.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/async_result.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/dispatch.hpp>
#include <asio/bind_allocator.hpp>
namespace asiortc {
namespace net = asio;
}
#else
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/bind_allocator.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#endif

#include <stdexec/execution.hpp>

#include <memory>
#include <type_traits>
#include <concepts>

namespace asiortc::utils {

namespace __async_wait_detail {

constexpr ::asiortc::error_code __make_error_code(::asiortc::errc::errc_t e) {
#if ASIORTC_USE_STANDALONE_ASIO
    return std::make_error_code(e);
#else
    return boost::system::errc::make_error_code(e);
#endif
}

constexpr ::asiortc::error_code exception_to_error_code(std::exception_ptr ex) {
    try {
        std::rethrow_exception(std::move(ex));
    }
#if ASIORTC_USE_STANDALONE_ASIO
    catch (const std::system_error &e) {
        return e.code();
    }
#else
    catch (const boost::system::system_error &e) {
        return e.code();
    }
#endif
    catch (...) {
        return __make_error_code(::asiortc::errc::io_error);
    }
    return {};
}

template <class Executor, class Sndr, class Handler, class... Args>
struct async_wait_op_state;

template <class Executor, class Sndr, class Handler, class... Args>
struct asio_receiver {
    using receiver_concept = stdexec::receiver_tag;
    using op_state_t = async_wait_op_state<Executor, Sndr, Handler, Args...>;
    using allocator_type = ::asiortc::net::associated_allocator<Handler>::type;
    using op_allocator_type = std::allocator_traits<
        allocator_type>::template rebind_alloc<op_state_t>;

    void destroy() noexcept;

    template <class... _Args> void set_value(_Args &&...args) && noexcept;
    void set_error(std::exception_ptr e) && noexcept;
    void set_stopped() && noexcept;

    stdexec::env<
        stdexec::prop<stdexec::get_stop_token_t, stdexec::inplace_stop_token>>
    get_env() const noexcept;

    op_state_t *_op;
};

template <class Executor, class Sndr, class Handler, class... Args>
struct async_wait_op_state {
    using receiver_type = asio_receiver<Executor, Sndr, Handler, Args...>;
    using operation_type = stdexec::connect_result_t<Sndr, receiver_type>;
    using allocator_t = ::asiortc::net::associated_allocator<Handler>::type;

    async_wait_op_state(const Executor &ex, Sndr &&sndr, Handler &&handler,
                        allocator_t alloc)
        : handler(std::move(handler)),
          work_guard(::asiortc::net::make_work_guard(ex)),
          alloc(std::move(alloc)),
          operation(stdexec::connect(std::move(sndr), receiver_type{this})) {}

    void start() noexcept { stdexec::start(this->operation); }

    std::shared_ptr<async_wait_op_state> self{};
    Handler handler;
    ::asiortc::net::executor_work_guard<Executor> work_guard;
    allocator_t alloc;
    ::asiortc::net::cancellation_slot slot;
    stdexec::inplace_stop_source stop_source;
    operation_type operation;
};

template <class Executor, class Sndr, class Handler, class... Args>
void asio_receiver<Executor, Sndr, Handler, Args...>::destroy() noexcept {
    auto ptr = std::move(_op->self);
    (void)ptr;
}

template <class Executor, class Sndr, class Handler, class... Args>
template <class... _Args>
inline void asio_receiver<Executor, Sndr, Handler, Args...>::set_value(
    _Args &&...args) && noexcept {
    _op->slot.clear();
    auto h = std::move(_op->handler);
    auto guard = std::move(_op->work_guard);
    auto alloc = std::move(_op->alloc);
    std::tuple<::asiortc::error_code, Args...> tp(::asiortc::error_code{},
                                                  std::forward<_Args>(args)...);
    destroy();
    ::asiortc::net::dispatch(
        guard.get_executor(),
        ::asiortc::net::bind_allocator(
            alloc, [h = std::move(h), res = std::move(tp)]() mutable {
                std::apply(std::move(h), std::move(res));
            }));
}

template <class Executor, class Sndr, class Handler, class... Args>
inline void asio_receiver<Executor, Sndr, Handler, Args...>::set_error(
    std::exception_ptr e) && noexcept {
    _op->slot.clear();
    auto h = std::move(_op->handler);
    auto guard = std::move(_op->work_guard);
    auto alloc = std::move(_op->alloc);
    destroy();
    ::asiortc::net::dispatch(
        guard.get_executor(),
        ::asiortc::net::bind_allocator(
            alloc, [h = std::move(h), e = std::move(e)]() mutable {
                std::move(h)(exception_to_error_code(std::move(e)), Args()...);
            }));
}

template <class Executor, class Sndr, class Handler, class... Args>
inline void
asio_receiver<Executor, Sndr, Handler, Args...>::set_stopped() && noexcept {
    _op->slot.clear();
    auto h = std::move(_op->handler);
    auto guard = std::move(_op->work_guard);
    auto alloc = std::move(_op->alloc);
    destroy();
    ::asiortc::net::dispatch(
        guard.get_executor(),
        ::asiortc::net::bind_allocator(alloc, [h = std::move(h)]() mutable {
            std::move(h)(__make_error_code(::asiortc::errc::operation_canceled),
                         Args()...);
        }));
}

template <class Executor, class Sndr, class Handler, class... Args>
inline stdexec::env<
    stdexec::prop<stdexec::get_stop_token_t, stdexec::inplace_stop_token>>
asio_receiver<Executor, Sndr, Handler, Args...>::get_env() const noexcept {
    return stdexec::env{
        stdexec::prop{stdexec::get_stop_token, _op->stop_source.get_token()}};
}

template <stdexec::sender S, class Callback, class... Args>
    requires(true && ... && std::default_initializable<Args>)
inline void async_wait_impl(S &&sndr, Callback &&cb) {
    using executor_type = ::asiortc::net::associated_executor<Callback>::type;
    using callback_type = std::decay_t<Callback>;
    using op_state_t = async_wait_op_state<executor_type, std::decay_t<S>,
                                           callback_type, Args...>;
    using allocator_type =
        ::asiortc::net::associated_allocator<callback_type>::type;
    using op_allocator_type = std::allocator_traits<
        allocator_type>::template rebind_alloc<op_state_t>;

    auto ex = ::asiortc::net::get_associated_executor(cb);
    auto alloc = ::asiortc::net::get_associated_allocator(cb);
    op_allocator_type op_alloc{alloc};

    auto p = std::allocate_shared<op_state_t>(
        op_alloc, ex, std::forward<S>(sndr), std::forward<Callback>(cb), alloc);
    p->self = p;

    auto slot = ::asiortc::net::get_associated_cancellation_slot(p->handler);
    if (slot.is_connected()) {
        p->slot = std::move(slot);
        p->slot.assign([pp = p](::asiortc::net::cancellation_type) mutable {
            auto p = std::move(pp);
            if (p)
                p->stop_source.request_stop();
        });
    }
    p->start();
}

template <class Sig> struct async_wait_t;

template <class... Args> struct async_wait_t<void(Args...)> {
    template <stdexec::sender S, class Token>
    static constexpr auto operator()(S &&sndr, Token &&token) {
        auto initiation = []<::asiortc::net::completion_handler_for<void(
                              ::asiortc::error_code, Args...)>
                                 Handler>(Handler &&h, std::decay_t<S> sndr) {
            async_wait_impl<std::decay_t<S>, std::decay_t<Handler>, Args...>(
                std::move(sndr), std::forward<Handler>(h));
        };

        return ::asiortc::net::async_initiate<Token, void(::asiortc::error_code,
                                                          Args...)>(
            initiation, token, std::forward<S>(sndr));
    }
};

} // namespace __async_wait_detail

template <class Sig>
inline constexpr __async_wait_detail::async_wait_t<Sig> async_wait{};

} // namespace asiortc::utils
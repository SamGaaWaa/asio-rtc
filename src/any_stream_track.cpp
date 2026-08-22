#include "asiortc/any_stream_track.hpp"
#include "asiortc/detail/uuid.hpp"
#include "asioice/detail/async_mutex.hpp"
#include "samlog.hpp"

#include <vector>
#include <cstdint>
#include <cassert>
#include <deque>
#include <algorithm>

namespace asiortc {

struct any_stream_track::impl_t {
    impl_t(std::shared_ptr<void> any_pipe, read_func_t read_func,
           split_callback_t split_cb, media_kind kind, media_format format)
        : _pipe{std::move(any_pipe)}, _read_func{std::move(read_func)},
          _split_cb{std::move(split_cb)}, _id{utils::uuid()}, _kind{kind},
          _desc{format} {
        if (_split_cb == nullptr)
            throw std::invalid_argument{"_split_cb == nullptr"};
    }

    impl_t(const impl_t &) = delete;
    impl_t(impl_t &&) = delete;
    impl_t &operator=(const impl_t &) = delete;
    impl_t &operator=(impl_t &&) = delete;

    const std::string &id() const noexcept { return _id; }

    media_kind kind() const noexcept { return _kind; }

    media_description description() const noexcept { return _desc; }

    void set_description(const media_description &desc) noexcept {
        _desc = desc;
    }

    track_state state() const noexcept { return _state; }

    void stop() noexcept { _state = track_state::ended; }

    asiortc::task<std::vector<media_frame>> read_frame();

    std::size_t max_cache_size() const noexcept { return _max_cache_size; }
    void set_max_cache_size(std::size_t n) noexcept { _max_cache_size = n; }

  private:
    auto read_some() {
        return _read_func(_pipe, net::dynamic_buffer(_buffer, _max_cache_size));
    }

    std::shared_ptr<void> _pipe;
    read_func_t _read_func;
    split_callback_t _split_cb;
    asioice::utils::async_mutex _mtx{};
    std::vector<uint8_t> _buffer{};
    std::size_t _max_cache_size{128 * 1024};
    std::deque<media_frame> _q{};

    std::string _id;
    media_kind _kind;
    media_description _desc;
    track_state _state = track_state::live;
};

any_stream_track::any_stream_track(std::shared_ptr<void> any_pipe,
                                   any_stream_track::read_func_t read_func,
                                   any_stream_track::split_callback_t split_cb,
                                   media_kind kind, media_format format)
    : _impl{new any_stream_track::impl_t(std::move(any_pipe),
                                         std::move(read_func),
                                         std::move(split_cb), kind, format)} {}

any_stream_track::~any_stream_track() { delete _impl; }

media_kind any_stream_track::kind() const noexcept {
    assert(_impl);
    return _impl->kind();
}

media_description any_stream_track::description() const noexcept {
    assert(_impl);
    return _impl->description();
}

void any_stream_track::set_description(const media_description &desc) noexcept {
    assert(_impl);
    _impl->set_description(desc);
}

const std::string &any_stream_track::id() const noexcept {
    assert(_impl);
    return _impl->id();
}

track_state any_stream_track::ready_state() const noexcept {
    assert(_impl);
    return _impl->state();
}

void any_stream_track::stop() noexcept {
    if (_impl)
        _impl->stop();
}

std::size_t any_stream_track::max_cache_size() const noexcept {
    assert(_impl);
    return _impl->max_cache_size();
}

void any_stream_track::set_max_cache_size(std::size_t n) noexcept {
    assert(_impl);
    _impl->set_max_cache_size(n);
}

asiortc::task<std::vector<media_frame>>
any_stream_track::recv(std::span<const encode_target> layers) {
    assert(_impl);
    return _impl->read_frame();
}

asiortc::task<std::vector<media_frame>> any_stream_track::impl_t::read_frame() {
    if (_state == track_state::ended)
        co_return std::vector<media_frame>{};
    if (!_q.empty()) {
        auto f = std::move(_q.front());
        _q.pop_front();
        std::vector<media_frame> result;
        result.push_back(std::move(f));
        co_return result;
    }
    auto lk = co_await _mtx.lock();
    while (true) {
        auto [ec, n] = co_await this->read_some();
        if (ec) {
            SAMLOG_WARN(auto sink) {
                sink("any_stream_track::impl_t::read_frame failed: {}\n",
                     ec.message());
            };
            _state = track_state::ended;
            co_return std::vector<media_frame>{};
        }
        if (n == 0) {
            SAMLOG_WARN(auto sink) {
                sink("any_stream_track::impl_t::read_frame failed: "
                     "file end\n");
            };
            _state = track_state::ended;
            co_return std::vector<media_frame>{};
        }
        std::size_t total = 0;
        while (true) {
            std::size_t consumed = 0;
            auto frame =
                _split_cb(std::span<const uint8_t>{_buffer.data() + total,
                                                   _buffer.size() - total},
                          consumed);
            total += consumed;
            if (!frame)
                break;
            _q.emplace_back(std::move(*frame));
            if (total == _buffer.size())
                break;
        }
        _buffer.erase(_buffer.begin(), _buffer.begin() + total);
        if (!_q.empty()) {
            auto f = std::move(_q.front());
            _q.pop_front();
            std::vector<media_frame> result;
            result.push_back(std::move(f));
            co_return result;
        }
    }
    std::unreachable();
}

} // namespace asiortc

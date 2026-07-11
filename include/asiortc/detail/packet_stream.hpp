#pragma once

#include "asioice/detail/packet_queue.hpp"
#include "asioice/detail/shared_promise.hpp"
#include "asioice/detail/if_else.hpp"

#include <exec/repeat_until.hpp>

namespace asiortc::detail {

struct packet_stream {
    packet_stream(std::size_t max_cache_bytes = 128 * 1024) noexcept:
        _buf(max_cache_bytes)
    {}

    packet_stream(const packet_stream&) = delete;
    packet_stream(packet_stream&&) = delete;
    packet_stream& operator=(const packet_stream&) = delete;
    packet_stream& operator=(packet_stream&&) = delete;

    bool readable() const noexcept {
        return _buf.empty();
    }

    auto wait_readable() noexcept {
        return asioice::utils::if_else(
            stdexec::just(readable()),
            [] {
                return stdexec::just();
            },
            [this] {
                return stdexec::just() |
                        stdexec::let_value([this] {
                            return _reader.get_future() |
                                    stdexec::then([this] {
                                        return readable();
                                    });
                        }) |
                        exec::repeat_until();
            }
        );
    }

    std::span<const uint8_t> peek() const noexcept {
        return _buf.peek();
    }

    void pop() noexcept {
        _buf.pop();
        _writer.set_value();
    }

    bool try_write(std::span<const uint8_t> data) {
        bool written = _buf.write(data);
        if (written)
            _reader.set_value();
        return written;
    }

    auto async_write(std::span<const uint8_t> data) {
        bool success = try_write(data);
        return asioice::utils::if_else(
            stdexec::just(success),
            [] {
                return stdexec::just(true);
            },
            [this, data] {
                return stdexec::just() |
                        stdexec::let_value([this, data] {
                            return _writer.get_future() |
                                    stdexec::then([this, data] {
                                        return try_write(data);
                                    });
                        }) |
                        exec::repeat_until() |
                        stdexec::then([] {
                            return true;
                        }) |
                        stdexec::upon_stopped([] {
                            return false;
                        });
            }
        );
    }
private:
    asioice::utils::packet_queue _buf;
    asioice::shared_promise<void> _writer{};
    asioice::shared_promise<void> _reader{};
};

using packet_stream_async_write_result = std::decay_t<decltype(std::declval<packet_stream&>().async_write(std::span<const uint8_t>{}))>;
using packet_stream_wait_readable_result = std::decay_t<decltype(std::declval<packet_stream&>().wait_readable())>;

} // namespace asiortc::detail
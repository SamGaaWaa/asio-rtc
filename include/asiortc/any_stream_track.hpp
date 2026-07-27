#pragma once

#include "asiortc/config.hpp"
#include "asiortc/media_track.hpp"
#include "asioice/config.hpp"
#include "asioice/detail/asio2exec.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#include <asio/buffer.hpp>
#include <asio/read.hpp>
#include <asio/completion_condition.hpp>
namespace asiortc {
namespace net = asio;
}
#else
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/completion_condition.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#endif

#include <memory>
#include <functional>
#include <optional>

namespace asiortc {

struct any_stream_track final : asiortc::media_track {
    using buffer_type =
        net::dynamic_vector_buffer<uint8_t,
                                   std::vector<uint8_t>::allocator_type>;
    using read_func_t =
        std::function<asioice::utils::sender<asiortc::error_code, std::size_t>(
            std::shared_ptr<void> &, buffer_type)>;

  public:
    using split_callback_t = std::function<std::optional<media_frame>(
        std::span<const uint8_t> data, std::size_t &consumed)>;

    template <class Pipe>
    any_stream_track(Pipe &&pipe, split_callback_t split_cb, media_kind kind,
                     media_format format)
        : any_stream_track(
              std::shared_ptr<void>(std::make_shared<std::decay_t<Pipe>>(
                  std::forward<Pipe>(pipe))),
              [](std::shared_ptr<void> &a, buffer_type buf) {
                  using p_type = std::decay_t<Pipe>;
                  auto p = static_cast<p_type *>(a.get());
                  return net::async_read(*p, buf, net::transfer_at_least(1),
                                         asioice::utils::use_any_sender);
              },
              std::move(split_cb), kind, format) {}

    any_stream_track(const any_stream_track &) = delete;
    any_stream_track(any_stream_track &&other) noexcept
        : _impl{std::exchange(other._impl, nullptr)} {}

    any_stream_track &operator=(const any_stream_track &) = delete;
    any_stream_track &operator=(any_stream_track &&other) {
        if (&other != this) {
            stop();
            _impl = other._impl;
            other._impl = nullptr;
        }
        return *this;
    }

    ~any_stream_track();

    media_kind kind() const noexcept override;
    media_format format() const noexcept override;
    void set_format(media_format f) noexcept;
    const std::string &id() const noexcept override;
    track_state ready_state() const noexcept override;

    void stop() noexcept override;
    bool stopped() const noexcept {
        return ready_state() == track_state::ended;
    }

    asiortc::task<std::optional<media_frame>> recv() override;

    std::size_t max_cache_size() const noexcept;
    void set_max_cache_size(std::size_t) noexcept;

  private:
    any_stream_track(std::shared_ptr<void> any_pipe, read_func_t read_func,
                     split_callback_t split_cb, media_kind kind,
                     media_format format);

    struct impl_t;

    impl_t *_impl;
};

} // namespace asiortc
#include "media_track_impl.hpp"

#include "asioice/detail/asio2exec.hpp"

#include <chrono>
#include <optional>

namespace asiortc {

media_track_impl::media_track_impl(media_kind k, std::string track_id,
                                   net::io_context &ctx)
    : _kind{k}, _id{std::move(track_id)}, _ctx{ctx},
      _jitter(std::chrono::milliseconds(500)) {}

void media_track_impl::stop() { _state = track_state::ended; }

void media_track_impl::push_frame(media_frame frame) {
    _jitter.push(std::move(frame));
}

asioice::task<std::optional<media_frame>> media_track_impl::recv() {
    using asioice::utils::use_sender;

    net::steady_timer timer(_ctx);

    while (_state == track_state::live) {
        auto frame = _jitter.pop();
        if (frame)
            co_return frame;

        timer.expires_after(std::chrono::milliseconds(5));
        auto [ec] = co_await timer.async_wait(net::as_tuple(use_sender));
        if (ec)
            break;
    }
    co_return std::nullopt;
}

} // namespace asiortc

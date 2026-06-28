#include "media_track_impl.hpp"

#include "asioice/detail/asio2exec.hpp"

#include <chrono>
#include <optional>

namespace asiortc {

media_track_impl::media_track_impl(media_kind k, std::string track_id)
    : _kind{k}, _id{std::move(track_id)},
      _jitter(std::chrono::milliseconds(500)) {}

void media_track_impl::stop() { _state = track_state::ended; }

void media_track_impl::push_frame(media_frame frame) {
    _jitter.push(std::move(frame));
    _on_frame.set_one_value();
}

asioice::task<std::optional<media_frame>> media_track_impl::recv() {
    auto lk = co_await _mtx.lock();
    while (true) {
        auto frame = _jitter.pop();
        if (frame)
            co_return frame;
        co_await _on_frame.get_future();
    }
    std::unreachable();
}

} // namespace asiortc

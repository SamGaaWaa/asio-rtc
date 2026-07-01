#include "media_track_impl.hpp"

#include "asioice/detail/asio2exec.hpp"

#include <chrono>
#include <optional>

namespace asiortc {

media_track_impl::media_track_impl(media_kind k, std::string track_id)
    : _kind{k}, _id{std::move(track_id)},
      _jitter(std::chrono::milliseconds(500),
              k == media_kind::video) {}

void media_track_impl::stop() { _state = track_state::ended; }

void media_track_impl::push_frame(rtp::rtp_packet pkt) {
    _jitter.push(std::move(pkt));
    _on_frame.set_one_value();
}

asioice::task<std::optional<rtp::rtp_packet>>
media_track_impl::recv_packet() {
    auto lk = co_await _mtx.lock();
    while (true) {
        auto pkt = _jitter.pop_frame();
        if (pkt)
            co_return pkt;
        co_await _on_frame.get_future();
    }
    std::unreachable();
}

asioice::task<std::optional<media_frame>> media_track_impl::recv() {
    auto pkt = co_await recv_packet();
    if (!pkt)
        co_return std::nullopt;
    media_frame mf;
    mf.kind = _kind;
    mf.timestamp = pkt->timestamp;
    mf.data = std::move(pkt->payload);
    co_return mf;
}

} // namespace asiortc

#include "media_track_impl.hpp"

#include "asioice/detail/asio2exec.hpp"
#include "asioice/detail/binary.hpp"

#include <chrono>
#include <optional>

namespace asiortc {

media_track_impl::media_track_impl(media_kind k, std::string track_id)
    : _kind{k}, _id{std::move(track_id)},
      _jitter(std::chrono::milliseconds(500), k == media_kind::video),
      _clock_rate(k == media_kind::audio ? 48000u : 90000u) {}

void media_track_impl::stop() { _state = track_state::ended; }

void media_track_impl::push_frame(rtp::rtp_packet pkt) {
    _jitter.push(std::move(pkt));
    _on_frame.set_one_value();
}

void media_track_impl::push_rtx_packet(rtp::rtp_packet pkt) {
    uint16_t osn = asioice::binary::ntoh<uint16_t>(
        *reinterpret_cast<const uint16_t *>(pkt.payload.data()));
    pkt.payload.erase(pkt.payload.begin(), pkt.payload.begin() + 2);
    pkt.sequence_number = osn;
    push_frame(std::move(pkt));
}

asiortc::task<std::optional<rtp::rtp_packet>> media_track_impl::recv_packet() {
    auto lk = co_await _mtx.lock();
    while (true) {
        auto pkt = _jitter.pop_frame();
        if (pkt)
            co_return pkt;
        co_await _on_frame.get_future();
    }
    std::unreachable();
}

asiortc::task<std::optional<media_frame>> media_track_impl::recv() {
    auto pkt = co_await recv_packet();
    if (!pkt)
        co_return std::nullopt;
    media_frame mf;
    mf.kind = _kind;
    mf.timestamp = pkt->timestamp;
    mf.data = std::move(pkt->payload);
    mf.info = rtp_frame_info{
        .rtp_timestamp = pkt->timestamp,
        .ssrc = pkt->ssrc,
        .clock_rate = _clock_rate,
        .first_sequence_number = pkt->sequence_number,
        .marker = static_cast<bool>(pkt->marker),
        .receive_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())};
    co_return mf;
}

} // namespace asiortc

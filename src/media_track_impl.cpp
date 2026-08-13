#include "media_track_impl.hpp"

#include "asioice/detail/asio2exec.hpp"
#include "asioice/detail/binary.hpp"
#include "asioice/detail/string_utils.hpp"

#include <chrono>
#include <optional>

namespace asiortc {

media_kind media_track_impl::kind() const noexcept {
    return asioice::utils::nceq("opus", _codec.name) ? media_kind::audio
                                                     : media_kind::video;
}

media_description media_track_impl::description() const noexcept {
    media_description desc;
    if (asioice::utils::nceq(_codec.name, "H264"))
        desc.format = media_format::h264;
    else if (asioice::utils::nceq(_codec.name, "VP8"))
        desc.format = media_format::vp8;
    else if (asioice::utils::nceq(_codec.name, "VP9"))
        desc.format = media_format::vp9;
    else if (asioice::utils::nceq(_codec.name, "opus"))
        desc.format = media_format::opus;
    desc.clock_rate = _codec.clock_rate;
    desc.encoding_params = _codec.params_string();
    desc.channels = _codec.channels;
    return desc;
}

void media_track_impl::stop() noexcept { _state = track_state::ended; }

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

asiortc::task<std::vector<media_frame>>
media_track_impl::recv(std::span<const encode_target> layers) {
    auto pkt = co_await recv_packet();
    if (!pkt)
        co_return std::vector<media_frame>{};
    media_frame mf;
    mf.kind = this->kind();
    mf.timestamp = pkt->timestamp;
    mf.data = std::move(pkt->payload);
    mf.info = rtp_frame_info{
        .rtp_timestamp = pkt->timestamp,
        .ssrc = pkt->ssrc,
        .clock_rate = _codec.clock_rate,
        .first_sequence_number = pkt->sequence_number,
        .marker = static_cast<bool>(pkt->marker),
        .receive_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())};
    std::vector<media_frame> result;
    result.push_back(std::move(mf));
    co_return result;
}

} // namespace asiortc

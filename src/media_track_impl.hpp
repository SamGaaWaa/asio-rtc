#pragma once

#include <memory>
#include <span>
#include <string>

#include "asiortc/media_track.hpp"
#include "asioice/config.hpp"
#include "asioice/detail/shared_promise.hpp"
#include "asioice/detail/async_mutex.hpp"
#include "asioice/detail/string_utils.hpp"
#include "jitter_buffer.hpp"
#include "asiortc/rtp.hpp"
#include "sdp.hpp"
#include "asiortc/detail/uuid.hpp"

namespace asiortc {

struct rtp_receiver;
struct connection_impl;

struct media_track_impl : public media_track {
    media_track_impl(sdp_rtpmap codec)
        : _codec{std::move(codec)}, _id{utils::uuid()},
          _jitter{std::chrono::milliseconds(500),
                  !asioice::utils::nceq(_codec.name, "opus")} {}

    const sdp_rtpmap &rtpmap() const noexcept { return _codec; }

    void set_rtpmap(sdp_rtpmap r) { _codec = std::move(r); }

    media_kind kind() const noexcept override;
    media_description description() const noexcept override;
    const std::string &id() const noexcept override { return _id; }
    void set_id(std::string id) noexcept { _id = std::move(id); }
    track_state ready_state() const noexcept override { return _state; }
    void stop() noexcept override;
    asiortc::task<std::vector<media_frame>>
    recv(std::span<const encode_target> layers) override;

    void push_frame(rtp::rtp_packet pkt);

  private:
    friend struct connection_impl;
    friend struct rtp_receiver;

    asiortc::task<std::vector<rtp::rtp_packet>> recv_packet();

    sdp_rtpmap _codec;
    std::string _id;
    track_state _state = track_state::live;

    asioice::utils::async_mutex _mtx{};
    asioice::shared_promise<void> _on_frame{};
    jitter_buffer _jitter;
};

} // namespace asiortc

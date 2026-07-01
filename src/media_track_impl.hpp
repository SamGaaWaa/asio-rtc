#pragma once

#include <memory>
#include <string>

#include "asiortc/media_track.hpp"
#include "asioice/config.hpp"
#include "asioice/detail/shared_promise.hpp"
#include "asioice/detail/async_mutex.hpp"
#include "jitter_buffer.hpp"
#include "rtp.hpp"

namespace asiortc {

struct media_track_impl : public media_track {
    media_track_impl(media_kind k, std::string track_id);

    media_kind kind() const noexcept override { return _kind; }
    std::string id() const noexcept override { return _id; }
    track_state ready_state() const noexcept override { return _state; }
    void stop() override;
    asioice::task<std::optional<media_frame>> recv() override;

    void push_frame(rtp::rtp_packet pkt);
    asioice::task<std::optional<rtp::rtp_packet>> recv_packet();

  private:
    media_kind _kind;
    std::string _id;
    track_state _state = track_state::live;

    asioice::utils::async_mutex _mtx{};
    asioice::shared_promise<void> _on_frame{};
    jitter_buffer _jitter;
};

} // namespace asiortc

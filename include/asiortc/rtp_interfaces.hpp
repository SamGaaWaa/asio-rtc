#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "asiortc/codecs/base.hpp"
#include "asiortc/media_track.hpp"
#include "asiortc/rtp.hpp"
#include "asiortc/rtp_parameters.hpp"
#include "asiortc/session_description.hpp"

namespace asiortc {

struct rtp_transceiver_interface;

// ── rtp_sender_interface ──────────────────────────────────────

struct rtp_sender;

struct rtp_sender_interface {
    const std::string &mid() const noexcept;
    const std::shared_ptr<media_track> &track() const noexcept;
    bool stopped() const noexcept;
    void stop();
    rtp_transceiver_interface transceiver() const noexcept;
    const rtp_send_parameters &parameters() const noexcept;
    const std::vector<std::string> &msids() const noexcept;
    void set_track(std::shared_ptr<media_track> t) noexcept;
    size_t num_streams() const noexcept;
    uint32_t ssrc(size_t idx = 0) const;
    operator bool() const noexcept { return _impl != nullptr; }

  private:
    friend struct rtp_transceiver_interface;
    friend struct peer_connection;

    std::shared_ptr<rtp_sender> _impl{};
};

// ── rtp_receiver_interface ─────────────────────────────────────

struct rtp_receiver;

struct rtp_receiver_interface {
    const std::string &mid() const noexcept;
    const std::shared_ptr<media_track> &track() const noexcept;
    bool stopped() const noexcept;
    void stop();
    rtp_transceiver_interface transceiver() const noexcept;
    const rtp_receive_parameters &parameters() const noexcept;
    const std::shared_ptr<codecs::decoder> &decoder() const noexcept;
    void set_on_rtp(std::function<bool(rtp::rtp_packet &)> cb);
    operator bool() const noexcept { return _impl != nullptr; }

  private:
    friend struct rtp_transceiver_interface;
    friend struct peer_connection;

    std::shared_ptr<rtp_receiver> _impl{};
};

// ── rtp_transceiver_interface ──────────────────────────────────

struct rtp_transceiver;

struct rtp_transceiver_interface {
    const std::string &mid() const noexcept;
    sdp_direction direction() const noexcept;
    void set_direction(sdp_direction dir);
    bool stopped() const noexcept;
    void stop();
    rtp_sender_interface sender() const noexcept;
    rtp_receiver_interface receiver() const noexcept;
    const std::vector<sdp_codec> &codecs() const noexcept;
    operator bool() const noexcept { return _impl != nullptr; }

  private:
    friend struct rtp_receiver_interface;
    friend struct rtp_sender_interface;
    friend struct peer_connection;

    std::shared_ptr<rtp_transceiver> _impl{};
};

} // namespace asiortc

#include "asiortc/rtp_interfaces.hpp"

#include "rtp_transceiver.hpp"

namespace asiortc {

// ── rtp_sender_interface ──────────────────────────────────────

const std::string &rtp_sender_interface::mid() const noexcept {
    return _impl->mid();
}

const std::shared_ptr<media_track> &
rtp_sender_interface::track() const noexcept {
    return _impl->track();
}

bool rtp_sender_interface::stopped() const noexcept { return _impl->stopped(); }

void rtp_sender_interface::stop() {
    if (_impl)
        _impl->stop();
}

const rtp_send_parameters &rtp_sender_interface::parameters() const noexcept {
    return _impl->parameters();
}

const std::vector<std::string> &rtp_sender_interface::msids() const noexcept {
    return _impl->msids();
}

void rtp_sender_interface::set_track(std::shared_ptr<media_track> t) noexcept {
    _impl->set_track(std::move(t));
}

// ── rtp_receiver_interface ─────────────────────────────────────

const std::string &rtp_receiver_interface::mid() const noexcept {
    return _impl->mid();
}

const std::shared_ptr<media_track> &
rtp_receiver_interface::track() const noexcept {
    return _impl->track();
}

bool rtp_receiver_interface::stopped() const noexcept {
    return _impl->stopped();
}

void rtp_receiver_interface::stop() { _impl->stop(); }

const rtp_receive_parameters &
rtp_receiver_interface::parameters() const noexcept {
    return _impl->parameters();
}

const std::shared_ptr<codecs::decoder> &
rtp_receiver_interface::decoder() const noexcept {
    return _impl->decoder();
}

// ── rtp_transceiver_interface ──────────────────────────────────

const std::string &rtp_transceiver_interface::mid() const noexcept {
    return _impl->mid();
}

sdp_direction rtp_transceiver_interface::direction() const noexcept {
    return _impl->direction();
}

void rtp_transceiver_interface::set_direction(sdp_direction dir) {
    _impl->set_direction(dir);
}

bool rtp_transceiver_interface::stopped() const noexcept {
    return _impl->stopped();
}

void rtp_transceiver_interface::stop() { _impl->stop(); }

rtp_sender_interface rtp_transceiver_interface::sender() const noexcept {
    rtp_sender_interface si;
    si._impl = _impl->sender();
    return si;
}

rtp_receiver_interface rtp_transceiver_interface::receiver() const noexcept {
    rtp_receiver_interface ri;
    ri._impl = _impl->receiver();
    return ri;
}

const std::vector<sdp_codec> &
rtp_transceiver_interface::codecs() const noexcept {
    return _impl->codecs();
}

rtp_transceiver_interface rtp_sender_interface::transceiver() const noexcept {
    rtp_transceiver_interface ti;
    ti._impl = _impl->transceiver();
    return ti;
}

rtp_transceiver_interface rtp_receiver_interface::transceiver() const noexcept {
    rtp_transceiver_interface ti;
    ti._impl = _impl->transceiver();
    return ti;
}

} // namespace asiortc

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sdp.hpp"

namespace asiortc {

struct connection_impl;

struct rtp_encoding_parameters {
    bool active = true;
    std::optional<uint32_t> ssrc;
    std::optional<uint32_t> max_bitrate;
    double scale_resolution_down_by = 1.0;
    std::string scalability_mode;
    std::string rid;
};

struct rtp_rtcp_parameters {
    std::string cname;
    bool reduced_size = false;
};

struct rtp_send_parameters {
    std::optional<std::string> transaction_id;
    std::vector<rtp_encoding_parameters> encodings;
    std::vector<std::string> header_extensions;
    rtp_rtcp_parameters rtcp;
};

struct rtp_receive_parameters {
    std::vector<std::string> header_extensions;
    rtp_rtcp_parameters rtcp;
};

struct rtp_transceiver;

struct rtp_sender : std::enable_shared_from_this<rtp_sender> {
    rtp_sender() noexcept = default;

    std::string mid() const noexcept { return _mid; }
    const std::string &track_id() const noexcept { return _track_id; }
    void set_track_id(std::string id) { _track_id = std::move(id); }

    bool stopped() const noexcept { return _stopped; }
    void stop() { _stopped = true; }

    std::shared_ptr<rtp_transceiver> transceiver() const noexcept {
        return _transceiver.lock();
    }

    const rtp_send_parameters &parameters() const noexcept {
        return _parameters;
    }

  private:
    friend struct rtp_transceiver;

    std::string _mid{};
    std::string _track_id{};
    std::weak_ptr<rtp_transceiver> _transceiver{};
    bool _stopped = false;
    rtp_send_parameters _parameters{};
};

struct rtp_receiver : std::enable_shared_from_this<rtp_receiver> {
    rtp_receiver() noexcept = default;

    std::string mid() const noexcept { return _mid; }
    const std::string &track_id() const noexcept { return _track_id; }
    void set_track_id(std::string id) { _track_id = std::move(id); }

    bool stopped() const noexcept { return _stopped; }
    void stop() { _stopped = true; }

    std::shared_ptr<rtp_transceiver> transceiver() const noexcept {
        return _transceiver.lock();
    }

    const rtp_receive_parameters &parameters() const noexcept {
        return _parameters;
    }

  private:
    friend struct rtp_transceiver;

    std::string _mid{};
    std::string _track_id{};
    std::weak_ptr<rtp_transceiver> _transceiver{};
    bool _stopped = false;
    rtp_receive_parameters _parameters{};
};

struct rtp_transceiver : std::enable_shared_from_this<rtp_transceiver> {
    rtp_transceiver(std::weak_ptr<connection_impl> conn, std::string mid,
                    sdp_direction direction = sdp_direction::sendrecv);

    std::string mid() const noexcept { return _mid; }
    void set_mid(std::string mid) { _mid = std::move(mid); }

    sdp_direction direction() const noexcept { return _direction; }
    void set_direction(sdp_direction dir) { _direction = dir; }

    bool stopped() const noexcept { return _stopped; }
    void stop();

    std::shared_ptr<rtp_sender> sender() const noexcept { return _sender; }
    std::shared_ptr<rtp_receiver> receiver() const noexcept {
        return _receiver;
    }

    const std::vector<sdp_codec> &codecs() const noexcept { return _codecs; }
    void set_codecs(std::vector<sdp_codec> codecs);

    const std::string &msid() const noexcept { return _msid; }
    void set_msid(std::string msid) { _msid = std::move(msid); }

    std::shared_ptr<connection_impl> connection() const noexcept {
        return _conn.lock();
    }

    void wire_back_references();

    sdp_media to_offer_sdp_media() const;
    sdp_media to_answer_sdp_media(const sdp_media &remote_media) const;
    void from_remote_sdp(const sdp_media &remote);

  private:
    std::string _mid{};
    sdp_direction _direction{sdp_direction::sendrecv};
    std::string _msid{};
    std::weak_ptr<connection_impl> _conn{};

    std::shared_ptr<rtp_sender> _sender{};
    std::shared_ptr<rtp_receiver> _receiver{};

    std::vector<sdp_codec> _codecs{};
    bool _stopped = false;
};

} // namespace asiortc

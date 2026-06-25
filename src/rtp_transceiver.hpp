#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "any_sender.hpp"
#include "codecs/base.hpp"
#include "sdp.hpp"

#include <functional>

namespace asiortc {

struct connection_impl;
struct media_track;

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
    std::vector<sdp_extmap> header_extensions;
    rtp_rtcp_parameters rtcp;
};

struct rtp_receive_parameters {
    std::vector<sdp_extmap> header_extensions;
    rtp_rtcp_parameters rtcp;
};

struct rtp_transceiver;

struct rtp_sender : std::enable_shared_from_this<rtp_sender> {
    rtp_sender() noexcept = default;

    const std::string &mid() const noexcept { return _mid; }

    const std::shared_ptr<media_track> &track() const noexcept {
        return _track;
    }

    void set_track(std::shared_ptr<media_track> t) noexcept {
        _track = std::move(t);
    }

    bool stopped() const noexcept { return _stopped; }
    void stop() {
        _stopped = true;
        _send_rtp_loop.reset();
        _send_rtcp_loop.reset();
    }

    std::shared_ptr<rtp_transceiver> transceiver() const noexcept {
        return _transceiver.lock();
    }

    const rtp_send_parameters &parameters() const noexcept {
        return _parameters;
    }

    const std::vector<std::string> &msids() const noexcept { return _msids; }

  private:
    friend struct rtp_transceiver;
    friend struct connection_impl;

    void set_msids(std::vector<std::string> msids) noexcept {
        _msids = std::move(msids);
    }

    std::string _mid{};
    std::shared_ptr<media_track> _track{};
    std::weak_ptr<rtp_transceiver> _transceiver{};
    bool _stopped = false;
    uint16_t _seq = 0;
    rtp_send_parameters _parameters{};
    std::optional<any_sender<void>> _send_rtp_loop{};
    std::optional<any_sender<void>> _send_rtcp_loop{};
    std::vector<std::string> _msids{};
    std::shared_ptr<codecs::encoder> _encoder{};
    bool _force_keyframe = false;

    uint32_t _packet_count = 0;
    uint32_t _octet_count = 0;
    uint64_t _ntp_timestamp = 0;
    uint32_t _rtp_timestamp = 0;

    uint16_t _seq_base = 0;
    uint64_t _ntp_base = 0;

    uint32_t _rtx_ssrc = 0;
    uint8_t _rtx_pt = 97;
    uint16_t _rtx_seq = 0;

    static constexpr size_t RTP_HISTORY = 128;
    struct _history_entry {
        std::vector<uint8_t> payload;
        uint16_t seq = 0;
        uint32_t timestamp = 0;
    };
    std::array<std::optional<_history_entry>, RTP_HISTORY> _history{};
};

struct rtp_receiver : std::enable_shared_from_this<rtp_receiver> {
    rtp_receiver() noexcept = default;

    const std::string &mid() const noexcept { return _mid; }

    const std::shared_ptr<media_track> &track() const noexcept {
        return _track;
    }

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
    friend struct connection_impl;

    void set_track(std::shared_ptr<media_track> t) { _track = std::move(t); }

    std::string _mid{};
    std::shared_ptr<media_track> _track{};
    std::weak_ptr<rtp_transceiver> _transceiver{};
    bool _stopped = false;
    rtp_receive_parameters _parameters{};
    std::optional<any_sender<void>> _rtcp_loop{};
};

struct rtp_transceiver_init {
    sdp_direction direction;
    std::vector<rtp_encoding_parameters> send_encodings;
    std::vector<std::string> streams;
};

std::vector<sdp_codec> default_video_codecs();
std::vector<sdp_codec> default_audio_codecs();

struct rtp_transceiver : std::enable_shared_from_this<rtp_transceiver> {
    rtp_transceiver(std::weak_ptr<connection_impl> conn)
        : _conn{std::move(conn)}, _sender{std::make_shared<rtp_sender>()},
          _receiver{std::make_shared<rtp_receiver>()} {
        if (_conn.expired())
            throw std::invalid_argument{"conn == nullptr"};
    }

    const std::string &mid() const noexcept { return _mid; }
    void set_mid(std::string mid) { _mid = std::move(mid); }

    sdp_direction direction() const noexcept { return _direction; }
    void set_direction(sdp_direction dir) { _direction = dir; }

    bool stopped() const noexcept { return _stopped; }
    void stop();

    const std::shared_ptr<rtp_sender> &sender() const noexcept {
        return _sender;
    }
    const std::shared_ptr<rtp_receiver> &receiver() const noexcept {
        return _receiver;
    }

    const std::vector<sdp_codec> &codecs() const noexcept { return _codecs; }
    void set_codecs(std::vector<sdp_codec> codecs);

    std::shared_ptr<connection_impl> connection() const noexcept {
        return _conn.lock();
    }

    void wire_back_references();

    sdp_media to_offer_sdp_media() const;
    sdp_media to_answer_sdp_media(const sdp_media &remote_media) const;
    void from_remote_sdp(const sdp_media &remote);

  private:
    std::weak_ptr<connection_impl> _conn;
    std::string _mid{};
    sdp_direction _direction{sdp_direction::sendrecv};

    std::shared_ptr<rtp_sender> _sender{};
    std::shared_ptr<rtp_receiver> _receiver{};

    std::vector<sdp_codec> _codecs{};
    bool _stopped = false;
};

} // namespace asiortc

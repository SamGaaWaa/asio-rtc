#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "any_sender.hpp"
#include "asiortc/rtp_parameters.hpp"
#include "rtp_packetizer/base.hpp"
#include "rtp_stream.hpp"
#include "ssrc_context.hpp"
#include "asiortc/rtp.hpp"
#include "sdp.hpp"
#include "media_track_impl.hpp"

#include <functional>
#include <list>

namespace asiortc {

struct connection_impl;
struct media_track;
struct rtp_transceiver;

struct rtp_sender : std::enable_shared_from_this<rtp_sender> {
    rtp_sender();

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

    rtp_stream &stream() { return *_streams.at(0); }
    const auto &streams() const noexcept { return _streams; }
    auto &streams() noexcept { return _streams; }

    rtp_packetizer::rtp_packetizer_base *packetizer() noexcept {
        return _packetizer.get();
    }

  private:
    friend struct rtp_transceiver;
    friend struct connection_impl;
    friend struct rtp_sender_interface;

    void set_msids(std::vector<std::string> msids) noexcept {
        _msids = std::move(msids);
    }

    void set_connection(std::weak_ptr<connection_impl> conn) noexcept {
        _conn = std::move(conn);
    }

    std::string _mid{};
    std::shared_ptr<media_track> _track{};
    std::weak_ptr<rtp_transceiver> _transceiver{};
    std::weak_ptr<connection_impl> _conn{};
    bool _stopped = false;
    rtp_send_parameters _parameters{};
    std::optional<any_sender<void>> _send_rtp_loop{};
    std::optional<any_sender<void>> _send_rtcp_loop{};
    std::vector<std::string> _msids{};
    std::unique_ptr<rtp_packetizer::rtp_packetizer_base> _packetizer{};
    std::vector<std::shared_ptr<rtp_stream>> _streams{};
};

struct rtp_receiver : std::enable_shared_from_this<rtp_receiver> {
    rtp_receiver() noexcept = default;

    rtp_receiver(const rtp_receiver &) = delete;
    rtp_receiver(rtp_receiver &&) = delete;
    rtp_receiver &operator=(const rtp_receiver &) = delete;
    rtp_receiver &operator=(rtp_receiver &&) = delete;

    const std::string &mid() const noexcept { return _mid; }

    const std::shared_ptr<media_track_impl> &track() const noexcept {
        return _track;
    }

    bool stopped() const noexcept { return _stopped; }
    void stop() {
        _stopped = true;
        _rtcp_loop.reset();
    }

    std::shared_ptr<rtp_transceiver> transceiver() const noexcept {
        return _transceiver.lock();
    }

    const rtp_receive_parameters &parameters() const noexcept {
        return _parameters;
    }

    void on_rtp(std::function<bool(rtp::rtp_packet &)> cb) {
        _on_rtp_cb = std::move(cb);
    }

    ssrc_context &create_ssrc_context(uint32_t ssrc,
                                      ssrc_context_set &ssrc_set);

    std::uint32_t rtcp_ssrc() const noexcept { return _rtcp_ssrc; }

    void set_rtcp_ssrc(std::uint32_t ssrc) { _rtcp_ssrc = ssrc; }

  private:
    friend struct rtp_transceiver;
    friend struct connection_impl;
    friend struct rtp_receiver_interface;

    std::string _mid{};
    std::shared_ptr<media_track_impl> _track{};
    std::weak_ptr<rtp_transceiver> _transceiver{};
    bool _stopped = false;
    rtp_receive_parameters _parameters{};
    std::optional<any_sender<void>> _rtcp_loop{};
    std::uint32_t _rtcp_ssrc{};
    std::function<bool(rtp::rtp_packet &)> _on_rtp_cb;
    std::list<ssrc_context> _ssrcs{};
};

struct rtp_transceiver : std::enable_shared_from_this<rtp_transceiver> {
    rtp_transceiver(media_description desc, std::weak_ptr<connection_impl> conn)
        : _conn{std::move(conn)}, _sender{std::make_shared<rtp_sender>()},
          _receiver{std::make_shared<rtp_receiver>()},
          _codec{sdp_rtpmap::from_media_description(desc)} {
        if (_conn.expired())
            throw std::invalid_argument{"conn == nullptr"};
    }

    rtp_transceiver(const rtp_transceiver &) = delete;
    rtp_transceiver(rtp_transceiver &&) = delete;
    rtp_transceiver &operator=(const rtp_transceiver &) = delete;
    rtp_transceiver &operator=(rtp_transceiver &&) = delete;

    ~rtp_transceiver() noexcept;

    const std::string &mid() const noexcept { return _mid; }
    void set_mid(std::string mid) { _mid = std::move(mid); }

    media_kind kind() const noexcept;
    const sdp_rtpmap &rtpmap() const noexcept { return _codec; }

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

    uint8_t payload_type() const noexcept { return _codec.payload_type; }

    std::shared_ptr<connection_impl> connection() const noexcept {
        return _conn.lock();
    }

    void wire_back_references();

    sdp_media to_offer_sdp_media(std::string mid) const;
    sdp_media to_answer_sdp_media(const sdp_media &remote_media) const;
    void from_remote_offer(const sdp_media &remote);
    void from_remote_answer(const sdp_media &remote);

    std::optional<std::size_t> mline_index() const noexcept { return _m_idx; }

    void set_mline_index(std::size_t idx) const noexcept { _m_idx = idx; }

    const sdp_rtpmap &codec() const noexcept { return _codec; }

  private:
    void apply_simulcast_to(sdp_media &m) const;
    std::vector<sdp_rtpmap> match_offer_rtpmaps(const sdp_media &offer,
                                                sdp_media &answer) const;

  public:
    std::vector<rtp_encoding_parameters> send_encodings;

  private:
    friend struct connection_impl;

    mutable std::optional<std::size_t> _m_idx{};
    std::weak_ptr<connection_impl> _conn;
    std::string _mid{};
    sdp_direction _direction{sdp_direction::sendrecv};

    std::shared_ptr<rtp_sender> _sender;
    std::shared_ptr<rtp_receiver> _receiver;

    sdp_rtpmap _codec{};
    bool _stopped = false;
};

} // namespace asiortc

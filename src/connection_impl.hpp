#pragma once

#include "any_sender.hpp"

#include "asioice/basic_agent.hpp"
#include "asioice/config.hpp"
#include "asioice/data_channel.hpp"
#include "asioice/detail/property.hpp"
#include "asioice/dtls_transport.hpp"
#include "asioice/ssl/dtls_config.hpp"
#include "asioice/task.hpp"
#include "asiortc/connection.hpp"
#include "asiortc/media_track.hpp"
#include "asiortc/rtc_stats.hpp"
#include "data_channel.hpp"
#include "rtp_transceiver.hpp"
#include "sdp.hpp"
#include "srtp_transport.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/ip/udp.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#else
#include <asio/ip/udp.hpp>
namespace asiortc {
namespace net = asio;
}
#endif

#include <boost/compat/move_only_function.hpp>
#include <functional>
#include <optional>
#include <unordered_map>

namespace asiortc {

struct connection_impl;
struct media_track_impl;

struct connection_impl : std::enable_shared_from_this<connection_impl> {
    using agent_type = asioice::basic_agent<net::ip::udp::socket>;
    using executor_type = typename agent_type::executor_type;
    using ice_transport_type = typename agent_type::ice_transport_type;
    using dtls_transport_type =
        asioice::ssl::dtls_transport<ice_transport_type>;
    using srtp_transport_type = asiortc::srtp_transport<ice_transport_type>;
    using sctp_transport_type = asioice::sctp::transport<dtls_transport_type>;
    using datachannel_manager_type =
        asioice::data_channel_manager<sctp_transport_type>;
    using data_channel_type = typename datachannel_manager_type::data_channel;

    using on_data_channel_cb = boost::compat::move_only_function<void(
        std::shared_ptr<asiortc::data_channel>)>;
    using on_candidates_cb = boost::compat::move_only_function<void(
        std::span<const asioice::candidate>)>;
    using on_track_cb = boost::compat::move_only_function<void(
        std::shared_ptr<rtp_receiver> receiver,
        std::shared_ptr<media_track> track, std::vector<std::string> msids,
        std::shared_ptr<rtp_transceiver> transceiver)>;

    using encoder_factory = std::function<
        std::shared_ptr<codecs::encoder>(int bitrate)>;
    using codec_registry = std::unordered_map<std::string, encoder_factory>;

    struct _remote_stream_stats {
        uint32_t ssrc = 0;
        uint32_t max_seq = 0;
        uint32_t cycles = 0;
        uint32_t base_seq = 0;
        bool base_seq_set = false;
        uint32_t packets_expected = 0;
        uint32_t packets_received = 0;
        uint32_t expected_prior = 0;
        uint32_t received_prior = 0;
        int jitter_q4 = 0;
        uint32_t last_arrival_ts = 0;
        uint32_t last_rtp_ts = 0;
        uint64_t lsr = 0;
        int consecutive_lost = 0;
        std::chrono::steady_clock::time_point lsr_time{};
    };

    connection_impl(executor_type ex, asiortc::configuration cfg = {});

    connection_impl(const connection_impl &) = delete;
    connection_impl(connection_impl &&) = delete;
    connection_impl &operator=(const connection_impl &) = delete;
    connection_impl &operator=(connection_impl &&) = delete;

    executor_type get_executor() const { return _executor; }

    bool can_trickle_ice_candidates() const noexcept {
        return _agent.config().trickle_ice;
    }

    ice_connection_state_t ice_connection_state() const noexcept {
        return to_ice_connection_state(_agent.state());
    }

    auto on_ice_connection_state_changed() noexcept {
        return _agent.on_state_change() |
               stdexec::then([this] { return ice_connection_state(); });
    }

    ice_gathering_state_t ice_gathering_state() const noexcept {
        return _gathering_state.get();
    }

    auto on_ice_gathering_state_changed() noexcept {
        return _gathering_state.on_change() |
               stdexec::continues_on(asioice::utils::scheduler{_executor});
    }

    connection_state_t connection_state() const noexcept {
        return _connection_state.get();
    }

    auto on_connection_state_changed() noexcept {
        return _connection_state.on_change() |
               stdexec::continues_on(asioice::utils::scheduler{_executor});
    }

    const auto &sctp() const noexcept { return _sctp_transport; }

    auto &sctp() noexcept { return _sctp_transport; }

    const auto &srtp() const noexcept { return _srtp_transport; }

    auto &srtp() noexcept { return _srtp_transport; }

    datachannel_manager_type *data_channel_manager() noexcept {
        if (_data_channel_manager)
            return &*_data_channel_manager;
        return nullptr;
    }

    const datachannel_manager_type *data_channel_manager() const noexcept {
        if (_data_channel_manager)
            return &*_data_channel_manager;
        return nullptr;
    }

    void on_new_ssrc(srtp_transport_base::on_new_ssrc_callback_type cb) {
        if (_srtp_transport)
            _srtp_transport->on_new_ssrc(std::move(cb));
        else
            _pending_srtp_new_ssrc_cb = std::move(cb);
    }

    void on_rtp_rtcp_packet(
        srtp_transport_base::on_rtp_rtcp_packet_callback_type cb) {
        if (_srtp_transport)
            _srtp_transport->on_rtp_rtcp_packet(std::move(cb));
        else
            _pending_srtp_rtp_cb = std::move(cb);
    }

    signaling_state_t signaling_state() const noexcept {
        return _signaling_state.get();
    }

    const session_description *local_description() const noexcept {
        if (_local_desc)
            return &*_local_desc;
        return nullptr;
    }

    asioice::task<session_description> create_offer();
    asioice::task<session_description> create_answer();

    std::shared_ptr<asiortc::data_channel>
    create_data_channel(std::string label,
                        asiortc::data_channel::options options = {});

    std::shared_ptr<rtp_transceiver>
    add_transceiver(media_kind kind, rtp_transceiver_init init = {});

    std::shared_ptr<rtp_transceiver>
    add_transceiver(std::shared_ptr<media_track>,
                    rtp_transceiver_init init = {});

    const auto &transceivers() const noexcept { return _transceivers; }
    auto &transceivers() noexcept { return _transceivers; }

    asioice::task<void> set_local_description(session_description desc);
    asioice::task<void> set_remote_description(session_description desc);

    auto add_ice_candidate(asioice::candidate c) {
        return _agent.add_remote_candidate(std::move(c));
    }
    auto add_ice_candidate() { return _agent.add_remote_candidate(); }
    void on_candidates(on_candidates_cb cb) noexcept {
        _on_candidates = std::move(cb);
    }
    void on_track(on_track_cb cb);

    void register_encoder(std::string name, encoder_factory factory);

    rtc_stats_report get_stats() const;

    auto sendto(net::const_buffer data, uint8_t component) {
        return _agent.sendto(data, component);
    }

    void close() noexcept;
    void on_remote_channel(on_data_channel_cb cb);

  private:
    asioice::task<void> apply_descriptions();
    void start_gathering();
    void do_on_candidates(std::span<const asioice::candidate>);
    void start_connecting();
    asioice::task<void> do_connect();
    static ice_connection_state_t
    to_ice_connection_state(asioice::agent_state_t s) noexcept;
    void on_data_channel(std::shared_ptr<data_channel_type> ch);
    void _start_sender_loops();
    static asioice::task<void>
    _sender_send_loop(std::weak_ptr<rtp_sender> weak_sender,
                      std::shared_ptr<srtp_transport_type> srtp);
    static asioice::task<void>
    _sender_rtcp_loop(std::weak_ptr<rtp_sender> weak_sender,
                      std::shared_ptr<srtp_transport_type> srtp);
    static asioice::task<void>
    _receiver_rtcp_loop(std::weak_ptr<rtp_receiver> weak_receiver);

    executor_type _executor;
    stdexec::counting_scope _scope{};

    agent_type _agent;
    bundle_policy_t _bundle_policy{bundle_policy_t::max_bundle};
    on_candidates_cb _on_candidates{};
    asioice::ssl::dtls_certificate _cert{};
    bool _roles_set = false;
    std::shared_ptr<ice_transport_type> _ice_transport{};
    std::shared_ptr<dtls_transport_type> _dtls_transport{};
    std::shared_ptr<srtp_transport_type> _srtp_transport{};
    std::shared_ptr<sctp_transport_type> _sctp_transport{};
    std::optional<datachannel_manager_type> _data_channel_manager{};

    std::vector<std::shared_ptr<rtp_transceiver>> _transceivers{};
    std::vector<std::vector<uint8_t>> _pending_rtcp{};
    std::vector<std::vector<uint8_t>> _pending_rtx{};
    std::unordered_map<uint32_t, std::shared_ptr<media_track_impl>>
        _ssrc_track_map{};
    std::unordered_map<uint32_t, _remote_stream_stats> _stream_stats{};

    std::string _transport_stats_id{};
    uint64_t _tx_packets = 0, _tx_bytes = 0;
    uint64_t _rx_packets = 0, _rx_bytes = 0;
    std::unordered_map<uint32_t, rtc_remote_outbound_rtp_stream_stats>
        _remote_outbound_stats{};
    std::unordered_map<uint32_t, rtc_remote_inbound_rtp_stream_stats>
        _remote_inbound_stats{};

    std::optional<session_description> _local_desc{};
    std::optional<session_description> _remote_desc{};
    std::optional<session_description> _pending_local_desc{};
    std::optional<session_description> _pending_remote_desc{};
    asioice::utils::property<ice_gathering_state_t> _gathering_state{
        ice_gathering_state_t::init};
    std::optional<any_sender<void>> _gathering_task{};
    asioice::utils::property<signaling_state_t> _signaling_state{
        signaling_state_t::stable};
    std::optional<any_sender<void>> _connecting_task{};
    asioice::utils::property<connection_state_t> _connection_state{
        connection_state_t::init};

    on_data_channel_cb _on_remote_channel_cb;
    on_track_cb _on_track_cb;
    codec_registry _codec_registry;
    bool _need_sctp{false};

    srtp_transport_base::on_new_ssrc_callback_type _pending_srtp_new_ssrc_cb;
    srtp_transport_base::on_rtp_rtcp_packet_callback_type _pending_srtp_rtp_cb;

    std::optional<any_sender<void>> _ice_connection_state_watcher{};
};

} // namespace asiortc

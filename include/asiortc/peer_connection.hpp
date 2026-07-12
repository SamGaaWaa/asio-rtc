#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <stdexcept>
#include <array>

#include "asiortc/config.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#define ASIOICE_USE_BOOST_ASIO 0
#include <asio/any_io_executor.hpp>
namespace asiortc {
namespace net = asio;
}
#else
#define ASIOICE_USE_BOOST_ASIO 1
#include <boost/asio/any_io_executor.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#endif

#include "asiortc/candidate.hpp"
#include "asiortc/configuration.hpp"
#include "asiortc/media_track.hpp"
#include "asiortc/rtc_stats.hpp"
#include "asiortc/rtp_interfaces.hpp"
#include "asiortc/rtp_parameters.hpp"
#include "asiortc/session_description.hpp"
#include "asiortc/task.hpp"
#include "asiortc/detail/packet_stream.hpp"
#include "asiortc/codecs/base.hpp"
#include "asiortc/datachannel.hpp"
#include "asioice/detail/asio2exec.hpp"

namespace asiortc {

namespace utils = asioice::utils;

enum struct ice_connection_state_t : char {
    init,
    checking,
    connected,
    failed,
    closed
};

enum struct ice_gathering_state_t : char { init, gathering, complete };

enum struct signaling_state_t : char {
    stable,
    have_local_offer,
    have_remote_offer,
    have_local_pranswer,
    have_remote_pranswer,
    closed
};

enum struct connection_state_t : char {
    init,
    connecting,
    connected,
    disconnected,
    failed,
    closed
};

struct connection_impl;

struct peer_connection {
    using executor_type = net::any_io_executor;

    peer_connection(net::any_io_executor ex, configuration cfg = {});

    peer_connection(const peer_connection &) = delete;
    peer_connection(peer_connection &&other) noexcept
        : _impl{std::move(other._impl)} {}

    peer_connection &operator=(const peer_connection &) = delete;
    peer_connection &operator=(peer_connection &&other) noexcept {
        if (this != &other) {
            close();
            _impl = std::move(other._impl);
        }
        return *this;
    }

    ~peer_connection() noexcept { close(); }

    executor_type get_executor() const;
    utils::scheduler get_scheduler() const {
        return utils::scheduler{get_executor()};
    }

    asiortc::task<session_description> create_offer();
    asiortc::task<session_description> create_answer();
    asiortc::task<void> set_local_description(session_description desc);
    asiortc::task<void> set_remote_description(session_description desc);
    const session_description *local_description() const noexcept;

    bool can_trickle_ice_candidates() const noexcept;
    asiortc::task<void> add_ice_candidate(candidate c);
    asiortc::task<void> add_ice_candidate();

    ice_connection_state_t ice_connection_state() const noexcept;
    asiortc::task<void> on_ice_connection_state_changed();

    ice_gathering_state_t ice_gathering_state() const noexcept;
    asiortc::task<void> on_ice_gathering_state_changed();

    connection_state_t connection_state() const noexcept;
    asiortc::task<void> on_connection_state_changed();

    signaling_state_t signaling_state() const noexcept;

    rtp_transceiver_interface add_transceiver(media_kind kind,
                                              rtp_transceiver_init init = {});
    rtp_transceiver_interface
    add_transceiver(std::shared_ptr<media_track> track,
                    rtp_transceiver_init init = {});

    auto send_rtp(const rtp_sender_interface &sender,
                  const rtp::rtp_packet &pkt) {
        if (!_impl || !sender)
            throw std::runtime_error{"!_impl || !sender"};
        return stdexec::just(std::array<uint8_t, 2000>{}) |
               stdexec::let_value([&, this](auto &buf) {
                   int n = pkt.write_to(buf.data(), buf.size());
                   if (n < 0)
                       throw std::runtime_error{
                           "send_rtp: pkt.write_to failed"};
                   rewrite_rtp_packet({buf.data(), (std::size_t)n}, sender);
                   auto enc = encrypt_rtp({buf.data(), (std::size_t)n}, buf);
                   if (enc.empty())
                       throw std::runtime_error{"send_rtp: encrypt_rtp failed"};
                   return rtp_send_buffer().async_write(
                              {enc.data(), enc.size()}) |
                          stdexec::then([&sender, &pkt, enc_size = enc.size(),
                                         this](auto sent) {
                              if (sent)
                                  update_sender_status_after_send_rtp(
                                      pkt.payload.size(), enc_size, sender);
                              return sent;
                          });
               }) |
               stdexec::upon_error([](auto err) { return false; });
    }

    data_channel_interface
    create_data_channel(std::string label, data_channel_options options = {});

    using on_track_cb = std::function<void(
        rtp_receiver_interface, std::shared_ptr<media_track>,
        std::vector<std::string> msids, rtp_transceiver_interface)>;
    void on_track(on_track_cb cb);

    using on_data_channel_cb = std::function<void(data_channel_interface)>;
    void on_data_channel(on_data_channel_cb cb);

    using on_candidates_cb =
        std::function<void(std::vector<asiortc::candidate>)>;
    void on_candidates(on_candidates_cb cb);

    void register_encoder(std::string name,
                          std::function<std::shared_ptr<codecs::encoder>(
                              const codecs::encoder_params &)>
                              factory);
    void
    register_decoder(std::string name,
                     std::function<std::shared_ptr<codecs::decoder>()> factory);

    rtc_stats_report get_stats() const;
    void close() noexcept;

  private:
    detail::packet_stream &rtp_send_buffer() noexcept;
    void rewrite_rtp_packet(std::span<uint8_t> data,
                            const rtp_sender_interface &sender) noexcept;
    std::span<uint8_t> encrypt_rtp(std::span<const uint8_t> data,
                                   std::span<uint8_t> buf) noexcept;
    void update_sender_status_after_send_rtp(
        std::size_t octet, std::size_t encrypted,
        const rtp_sender_interface &sender) noexcept;

    std::shared_ptr<connection_impl> _impl;
};

} // namespace asiortc

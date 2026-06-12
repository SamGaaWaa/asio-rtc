#pragma once

#include <boost/compat/move_only_function.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "asioice/detail/receiver.hpp"
#include "asioice/ssl/dtls_config.hpp"
#include "concepts.hpp"
#include "rtp.hpp"

namespace asiortc {

struct remote_sdp {
  std::string ice_ufrag;
  std::string ice_pwd;
  std::string fingerprint;  // "sha-256 AB:CD:..." (after "a=fingerprint:")
  std::string setup;        // "active" or "passive"
  std::vector<std::string> candidates;
};

remote_sdp parse_remote_sdp(std::string_view sdp);

struct srtp_init_guard;

class srtp_transport_base : public asioice::datagram_receiver {
 public:
  using srtp_protection_profile = asioice::ssl::srtp_protection_profile;
  using dtls_role = asioice::ssl::dtls_role;
  using srtp_key_material = asioice::ssl::srtp_key_material;

  using on_new_ssrc_callback_type =
      boost::compat::move_only_function<bool(uint32_t,
                                             std::span<const uint8_t> data)>;
  using on_rtp_rtcp_packet_callback_type =
      boost::compat::move_only_function<void(asioice::io_buffer_ptr)>;

  static constexpr std::size_t MAX_ACTIVE_SSRCS = 32;

  srtp_transport_base();
  srtp_transport_base(const srtp_key_material& keys, dtls_role role);
  ~srtp_transport_base();

  srtp_transport_base(const srtp_transport_base&) = delete;
  srtp_transport_base& operator=(const srtp_transport_base&) = delete;
  srtp_transport_base(srtp_transport_base&& other) = delete;
  srtp_transport_base& operator=(srtp_transport_base&& other) = delete;

  static std::size_t max_protect_rtp_overhead() noexcept;
  static std::size_t max_protect_rtcp_overhead() noexcept;

  srtp_protection_profile profile() const noexcept { return _profile; }

  void setup(const srtp_key_material& keys, dtls_role role);

  void remove_incoming_ssrc(uint32_t ssrc) noexcept;

  void on_new_ssrc(on_new_ssrc_callback_type cb) {
    _on_new_ssrc = std::move(cb);
  }

  void on_rtp_rtcp_packet(on_rtp_rtcp_packet_callback_type cb) {
    _on_rtp_rtcp_packet = std::move(cb);
  }

  std::span<uint8_t> protect_rtp(std::span<const uint8_t> input,
                                 std::span<uint8_t> output) noexcept;
  std::span<uint8_t> unprotect_rtp(std::span<const uint8_t> input,
                                   std::span<uint8_t> output) noexcept;
  std::span<uint8_t> protect_rtcp(std::span<const uint8_t> input,
                                  std::span<uint8_t> output) noexcept;
  std::span<uint8_t> unprotect_rtcp(std::span<const uint8_t> input,
                                    std::span<uint8_t> output) noexcept;

 private:
  void destroy_sessions() noexcept;
  static constexpr bool is_rtcp_packet(
      std::span<const uint8_t> packet) noexcept {
    if (packet.size() < 8)
      return false;
    uint8_t payload_type = packet[1];
    return (payload_type >= 200 && payload_type <= 204);
  }
  bool datagram_received(asioice::io_buffer_ptr& buffer) override;

  std::shared_ptr<srtp_init_guard> _init_guard;

 protected:
  void* _send_session = nullptr;  // srtp_t
  void* _recv_session = nullptr;  // srtp_t
  srtp_protection_profile _profile{};

  boost::unordered::unordered_flat_set<uint32_t> _active_ssrcs{};
  on_new_ssrc_callback_type _on_new_ssrc{};
  on_rtp_rtcp_packet_callback_type _on_rtp_rtcp_packet{};
};

template <class IceTransport>
struct srtp_transport : srtp_transport_base {
  using ice_transport_type = IceTransport;

  srtp_transport(std::shared_ptr<IceTransport> transport)
      : srtp_transport_base(), _transport(std::move(transport)) {
    if (!_transport)
      throw std::invalid_argument{"transport == nullptr"};
    _transport->add_receiver(*this);
  }

  ~srtp_transport() { detach(); }

  auto send_rtp(std::span<const uint8_t> data, std::span<uint8_t> buf) {
    if (!rtp::is_rtp_packet(data))
      throw std::invalid_argument{"!rtp::is_rtp_packet(data)"};
    auto enc = protect_rtp(data, buf);
    if (enc.empty())
      throw std::runtime_error{"protect_rtp failed"};
    return do_send(enc);
  }

  template <VectorLikeBuffer Vec>
  auto send_rtp(Vec& data) {
    std::size_t origin_size = data.size();
    data.resize(data.size() + max_protect_rtp_overhead());
    auto enc = protect_rtp({(const uint8_t*)data.data(), origin_size},
                           {(uint8_t*)data.data(), data.size()});
    if (enc.empty())
      throw std::runtime_error{"protect_rtp failed"};
    return do_send(enc);
  }

 private:
  auto do_send(std::span<const uint8_t> data) {
    if (!_send_session)
      throw std::runtime_error{"!_send_session"};
    return _transport->async_send(data);
  }

  std::shared_ptr<ice_transport_type> _transport;
};

}  // namespace asiortc

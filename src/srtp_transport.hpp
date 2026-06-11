#pragma once

#include <memory>
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <utility>
#include <span>

#include "asioice/ssl/dtls_config.hpp"

namespace asiortc {

struct remote_sdp {
    std::string ice_ufrag;
    std::string ice_pwd;
    std::string fingerprint;   // "sha-256 AB:CD:..." (after "a=fingerprint:")
    std::string setup;         // "active" or "passive"
    std::vector<std::string> candidates;
};

remote_sdp parse_remote_sdp(std::string_view sdp);

struct srtp_init_guard;

class srtp_transport_base {
public:
    using srtp_protection_profile = asioice::ssl::srtp_protection_profile;
    using dtls_role = asioice::ssl::dtls_role;
    using srtp_key_material = asioice::ssl::srtp_key_material;

    srtp_transport_base(const srtp_key_material& keys, dtls_role role);
    ~srtp_transport_base();

    srtp_transport_base(const srtp_transport_base&) = delete;
    srtp_transport_base& operator=(const srtp_transport_base&) = delete;
    srtp_transport_base(srtp_transport_base&& other) noexcept;
    srtp_transport_base& operator=(srtp_transport_base&& other) noexcept;

    static std::size_t max_protect_rtp_overhead() noexcept;
    static std::size_t max_protect_rtcp_overhead() noexcept;

    srtp_protection_profile profile() const noexcept {
        return _profile; 
    }

    std::span<uint8_t> protect_rtp(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept;
    std::span<uint8_t> unprotect_rtp(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept;
    std::span<uint8_t> protect_rtcp(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept;
    std::span<uint8_t> unprotect_rtcp(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept;
private:
    std::shared_ptr<srtp_init_guard> _init_guard;

    void* _send_session = nullptr;  // srtp_t
    void* _recv_session = nullptr;  // srtp_t
    srtp_protection_profile _profile;

    void setup(const srtp_key_material& keys, dtls_role role);
    void destroy_sessions() noexcept;
};

template <class IceTransport>
struct srtp_transport {

};

} // namespace asiortc

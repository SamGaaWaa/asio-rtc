#include "srtp_transport.hpp"

#include "asioice/detail/scope_guard.hpp"

#include "srtp.h"

#include <stdexcept>
#include <cassert>
#include <mutex>
#include <string>
#include <utility>

namespace asiortc {

struct srtp_init_guard {
    srtp_init_guard() {
        auto ret = ::srtp_init();
        if (ret != srtp_err_status_ok)
            throw std::runtime_error{std::to_string((int)ret)};
    }

    ~srtp_init_guard() {
        auto ret = ::srtp_shutdown();
        assert(ret == srtp_err_status_ok);
    }
};

static std::shared_ptr<srtp_init_guard> get_libsrtp_guard() {
    static std::mutex mtx;
    static std::weak_ptr<srtp_init_guard> weak_guard;

    std::lock_guard<std::mutex> lk(mtx);
    auto guard = weak_guard.lock();
    if (!guard) {
        guard = std::make_shared<srtp_init_guard>();
        weak_guard = guard;
    }
    return guard;
}

static ::srtp_profile_t
to_libsrtp_profile(asioice::ssl::srtp_protection_profile p) {
    using enum asioice::ssl::srtp_protection_profile;
    switch (p) {
    case srtp_aes128_cm_sha1_80:
        return ::srtp_profile_aes128_cm_sha1_80;
    case srtp_aes128_cm_sha1_32:
        return ::srtp_profile_aes128_cm_sha1_32;
    case srtp_aead_aes_128_gcm:
        return ::srtp_profile_aead_aes_128_gcm;
    case srtp_aead_aes_256_gcm:
        return ::srtp_profile_aead_aes_256_gcm;
    default:
        throw std::runtime_error{"unsupported SRTP protection profile"};
    }
}

static ::srtp_t create_session(const std::vector<uint8_t>& key,
                               const std::vector<uint8_t>& salt,
                               ::srtp_profile_t profile,
                               const ::srtp_ssrc_t& ssrc) {
    ::srtp_policy_t policy = nullptr;
    auto ret = ::srtp_policy_create(&policy);
    if (ret != srtp_err_status_ok)
        throw std::runtime_error{"srtp_policy_create failed: " +
                                 std::to_string((int)ret)};
    asioice::utils::scope_guard policy_guard([&policy]()noexcept {
        ::srtp_policy_destroy(policy);
    });

    ::srtp_policy_set_profile(policy, profile);
    ::srtp_policy_set_ssrc(policy, ssrc);
    ret = ::srtp_policy_add_key(policy, key.data(), key.size(), salt.data(),
                                salt.size(), nullptr, 0);
    if (ret != srtp_err_status_ok)
        throw std::runtime_error{"srtp_policy_add_key failed: " +
                                 std::to_string((int)ret)};

    ::srtp_t session = nullptr;
    ret = ::srtp_create(&session, policy);
    if (ret != srtp_err_status_ok)
        throw std::runtime_error{"srtp_create failed: " +
                                 std::to_string((int)ret)};

    return session;
}

void srtp_transport_base::setup(const srtp_key_material& keys, dtls_role role) {
    using enum asioice::ssl::dtls_role;

    const auto& send_key =
        (role == client) ? keys.client_write_key : keys.server_write_key;
    const auto& send_salt =
        (role == client) ? keys.client_write_salt : keys.server_write_salt;
    const auto& recv_key =
        (role == client) ? keys.server_write_key : keys.client_write_key;
    const auto& recv_salt =
        (role == client) ? keys.server_write_salt : keys.client_write_salt;

    auto profile = to_libsrtp_profile(keys.profile);

    _send_session = create_session(send_key, send_salt, profile,
                                   {::ssrc_any_outbound, 0});
    _recv_session = create_session(recv_key, recv_salt, profile,
                                   {::ssrc_any_inbound, 0});
    _profile = keys.profile;
}

void srtp_transport_base::destroy_sessions() noexcept {
    if (_send_session) {
        ::srtp_dealloc(static_cast<::srtp_t>(_send_session));
        _send_session = nullptr;
    }
    if (_recv_session) {
        ::srtp_dealloc(static_cast<::srtp_t>(_recv_session));
        _recv_session = nullptr;
    }
}

srtp_transport_base::srtp_transport_base(const srtp_key_material& keys, dtls_role role):
    _init_guard{get_libsrtp_guard()}
{
    setup(keys, role);
}

srtp_transport_base::srtp_transport_base(srtp_transport_base&& other) noexcept:
    _init_guard{std::move(other._init_guard)},
    _send_session{std::exchange(other._send_session, nullptr)},
    _recv_session{std::exchange(other._recv_session, nullptr)},
    _profile{other._profile}
{}

srtp_transport_base& srtp_transport_base::operator=(srtp_transport_base&& other) noexcept
{
    if (this != &other) {
        destroy_sessions();
        _init_guard = std::move(other._init_guard);
        _send_session = std::exchange(other._send_session, nullptr);
        _recv_session = std::exchange(other._recv_session, nullptr);
        _profile = other._profile;
    }
    return *this;
}

srtp_transport_base::~srtp_transport_base() { destroy_sessions(); }

std::span<uint8_t>
srtp_transport_base::protect_rtp(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept
{
    size_t cap = output.size();
    auto ret = ::srtp_protect(static_cast<::srtp_t>(_send_session), input.data(), input.size(), output.data(), &cap, 0);
    if (ret != srtp_err_status_ok)
        return {};
    return {output.data(), cap};
}

std::span<uint8_t>
srtp_transport_base::unprotect_rtp(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept {
    std::size_t cap = output.size();
    auto ret = ::srtp_unprotect(static_cast<::srtp_t>(_recv_session), input.data(), input.size(),
                                output.data(), &cap);
    if (ret != srtp_err_status_ok)
        return {};
    return {output.data(), cap};
}

std::span<uint8_t>
srtp_transport_base::protect_rtcp(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept {
    size_t cap = output.size();
    auto ret = ::srtp_protect_rtcp(static_cast<::srtp_t>(_send_session), input.data(), input.size(), output.data(), &cap, 0);
    if (ret != srtp_err_status_ok)
        return {};
    return {output.data(), cap};
}

std::span<uint8_t>
srtp_transport_base::unprotect_rtcp(std::span<const uint8_t> input, std::span<uint8_t> output) noexcept {
    std::size_t cap = output.size();
    auto ret = ::srtp_unprotect_rtcp(static_cast<::srtp_t>(_recv_session), input.data(),
                                     input.size(), output.data(), &cap);
    if (ret != srtp_err_status_ok)
        return {};
    return {output.data(), cap};
}

std::size_t srtp_transport_base::max_protect_rtp_overhead() noexcept {
    return SRTP_MAX_TRAILER_LEN;
}

std::size_t srtp_transport_base::max_protect_rtcp_overhead() noexcept {
    return SRTP_MAX_SRTCP_TRAILER_LEN;
}


remote_sdp parse_remote_sdp(std::string_view sdp) {
    remote_sdp d;
    std::string_view r = sdp;
    while (!r.empty()) {
        auto p = r.find("\r\n");
        if (p == std::string_view::npos)
            p = r.find('\n');
        auto l = r.substr(0, p);
        r.remove_prefix(p == std::string_view::npos
                            ? r.size()
                            : p + (l.size() < r.size() && r[l.size()] == '\r'
                                       ? 2
                                       : 1));

        if (l.starts_with("a=ice-ufrag:"))
            d.ice_ufrag = l.substr(12);
        else if (l.starts_with("a=ice-pwd:"))
            d.ice_pwd = l.substr(10);
        else if (l.starts_with("a=fingerprint:"))
            d.fingerprint = l.substr(14);
        else if (l.starts_with("a=setup:"))
            d.setup = l.substr(8);
        else if (l.starts_with("a=") && l.substr(2).starts_with("candidate:"))
            d.candidates.emplace_back(l.substr(2));
    }
    return d;
}

} // namespace asiortc

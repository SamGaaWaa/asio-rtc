#include "asioice/config.hpp"
#include "srtp_transport.hpp"
#include "samlog.hpp"

#include <cassert>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "asioice/detail/scope_guard.hpp"
#include "srtp.h"

namespace asiortc {

static void srtp_log_func(srtp_log_level_t level, const char *msg,
                          void *data) noexcept {
    SAMLOG_INFO(auto sink) {
        char buf[256];
        sink({buf, sizeof(buf)}, "srtp_log_level_t {}: {}\n", (int)level, msg);
    };
}

static const char *status_to_string(srtp_err_status_t s) noexcept {
    switch (s) {
    case srtp_err_status_ok:
        return "nothing to report";
    case srtp_err_status_fail:
        return "unspecified failure";
    case srtp_err_status_bad_param:
        return "unsupported parameter";
    case srtp_err_status_alloc_fail:
        return "couldn't allocate memory";
    case srtp_err_status_dealloc_fail:
        return "couldn't deallocate properly";
    case srtp_err_status_init_fail:
        return "couldn't initialize";
    case srtp_err_status_terminus:
        return "can't process as much data as requested";
    case srtp_err_status_auth_fail:
        return "authentication failure";
    case srtp_err_status_cipher_fail:
        return "cipher failure";
    case srtp_err_status_replay_fail:
        return "replay check failed (bad index";
    case srtp_err_status_replay_old:
        return "replay check failed (index too old)";
    case srtp_err_status_algo_fail:
        return "algorithm failed test routine";
    case srtp_err_status_no_such_op:
        return "unsupported operation";
    case srtp_err_status_no_ctx:
        return "no appropriate context found";
    case srtp_err_status_cant_check:
        return "unable to perform desired validation";
    case srtp_err_status_key_expired:
        return "can't use key any more";
    case srtp_err_status_socket_err:
        return "error in use of socket";
    case srtp_err_status_signal_err:
        return "error in use POSIX signals";
    case srtp_err_status_nonce_bad:
        return "nonce check failed";
    case srtp_err_status_read_fail:
        return "couldn't read data";
    case srtp_err_status_write_fail:
        return "couldn't write data";
    case srtp_err_status_parse_err:
        return "error parsing data";
    case srtp_err_status_encode_err:
        return "error encoding data";
    case srtp_err_status_semaphore_err:
        return "error while using semaphores";
    case srtp_err_status_pfkey_err:
        return "error while using pfkey";
    case srtp_err_status_bad_mki:
        return "error MKI present in packet is invalid";
    case srtp_err_status_pkt_idx_old:
        return "packet index is too old to consider";
    case srtp_err_status_pkt_idx_adv:
        return "packet index advanced, reset needed";
    case srtp_err_status_buffer_small:
        return "out buffer is too small";
    case srtp_err_status_cryptex_err:
        return "unsupported cryptex operation";
    default:
        return "unknown";
    }
    std::unreachable();
}

struct srtp_init_guard {
    srtp_init_guard() {
        auto ret = ::srtp_init();
        if (ret != srtp_err_status_ok)
            throw std::runtime_error{status_to_string(ret)};
        ret = ::srtp_install_log_handler(srtp_log_func, nullptr);
        if (ret != srtp_err_status_ok) {
            ::srtp_shutdown();
            throw std::runtime_error{status_to_string(ret)};
        }
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

static ::srtp_t create_session(const std::vector<uint8_t> &key,
                               const std::vector<uint8_t> &salt,
                               ::srtp_profile_t profile,
                               const ::srtp_ssrc_t &ssrc) {
    ::srtp_policy_t policy = nullptr;
    auto ret = ::srtp_policy_create(&policy);
    if (ret != srtp_err_status_ok)
        throw std::runtime_error{std::string("srtp_policy_create failed: ") +
                                 status_to_string(ret)};
    asioice::utils::scope_guard policy_guard(
        [&policy]() noexcept { ::srtp_policy_destroy(policy); });

    ::srtp_policy_set_profile(policy, profile);
    ::srtp_policy_set_ssrc(policy, ssrc);
    ret = ::srtp_policy_add_key(policy, key.data(), key.size(), salt.data(),
                                salt.size(), nullptr, 0);
    if (ret != srtp_err_status_ok)
        throw std::runtime_error{std::string("srtp_policy_add_key failed: ") +
                                 status_to_string(ret)};

    ::srtp_t session = nullptr;
    ret = ::srtp_create(&session, policy);
    if (ret != srtp_err_status_ok)
        throw std::runtime_error{std::string("srtp_create failed: ") +
                                 status_to_string(ret)};

    return session;
}

void srtp_transport_base::setup(const srtp_key_material &keys, dtls_role role) {
    using enum asioice::ssl::dtls_role;

    const auto &send_key =
        (role == client) ? keys.client_write_key : keys.server_write_key;
    const auto &send_salt =
        (role == client) ? keys.client_write_salt : keys.server_write_salt;
    const auto &recv_key =
        (role == client) ? keys.server_write_key : keys.client_write_key;
    const auto &recv_salt =
        (role == client) ? keys.server_write_salt : keys.client_write_salt;

    auto profile = to_libsrtp_profile(keys.profile);

    _send_session =
        create_session(send_key, send_salt, profile, {::ssrc_any_outbound, 0});
    _recv_session =
        create_session(recv_key, recv_salt, profile, {::ssrc_any_inbound, 0});
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

srtp_transport_base::srtp_transport_base() : _init_guard{get_libsrtp_guard()} {}

srtp_transport_base::srtp_transport_base(const srtp_key_material &keys,
                                         dtls_role role)
    : _init_guard{get_libsrtp_guard()} {
    setup(keys, role);
}

srtp_transport_base::~srtp_transport_base() { destroy_sessions(); }

std::span<uint8_t>
srtp_transport_base::protect_rtp(std::span<const uint8_t> input,
                                 std::span<uint8_t> output) noexcept {
    size_t cap = output.size();
    auto ret =
        ::srtp_protect(static_cast<::srtp_t>(_send_session), input.data(),
                       input.size(), output.data(), &cap, 0);
    if (ret != srtp_err_status_ok) {
        SAMLOG_WARN(auto sink) {
            char buf[256];
            sink({buf, sizeof(buf)}, "srtp_protect failed: {}\n",
                 status_to_string(ret));
        };
        return {};
    }
    return {output.data(), cap};
}

std::span<uint8_t>
srtp_transport_base::unprotect_rtp(std::span<const uint8_t> input,
                                   std::span<uint8_t> output) noexcept {
    std::size_t cap = output.size();
    auto ret =
        ::srtp_unprotect(static_cast<::srtp_t>(_recv_session), input.data(),
                         input.size(), output.data(), &cap);
    if (ret != srtp_err_status_ok) {
        SAMLOG_WARN(auto sink) {
            char buf[256];
            sink({buf, sizeof(buf)}, "srtp_unprotect failed: {}\n",
                 status_to_string(ret));
        };
        return {};
    }
    return {output.data(), cap};
}

std::span<uint8_t>
srtp_transport_base::protect_rtcp(std::span<const uint8_t> input,
                                  std::span<uint8_t> output) noexcept {
    size_t cap = output.size();
    auto ret =
        ::srtp_protect_rtcp(static_cast<::srtp_t>(_send_session), input.data(),
                            input.size(), output.data(), &cap, 0);
    if (ret != srtp_err_status_ok) {
        SAMLOG_WARN(auto sink) {
            char buf[256];
            sink({buf, sizeof(buf)}, "srtp_protect_rtcp failed: {}\n",
                 status_to_string(ret));
        };
        return {};
    }
    return {output.data(), cap};
}

std::span<uint8_t>
srtp_transport_base::unprotect_rtcp(std::span<const uint8_t> input,
                                    std::span<uint8_t> output) noexcept {
    std::size_t cap = output.size();
    auto ret =
        ::srtp_unprotect_rtcp(static_cast<::srtp_t>(_recv_session),
                              input.data(), input.size(), output.data(), &cap);
    if (ret != srtp_err_status_ok) {
        SAMLOG_INFO(auto sink) {
            char buf[256];
            sink({buf, sizeof(buf)}, "srtp_unprotect_rtcp failed: {}\n",
                 status_to_string(ret));
        };
        return {};
    }
    return {output.data(), cap};
}

std::size_t srtp_transport_base::max_protect_rtp_overhead() noexcept {
    return SRTP_MAX_TRAILER_LEN;
}

std::size_t srtp_transport_base::max_protect_rtcp_overhead() noexcept {
    return SRTP_MAX_SRTCP_TRAILER_LEN;
}

bool srtp_transport_base::datagram_received(asioice::io_buffer_ptr &buffer) {
    if (!buffer || buffer->size() < 8)
        return false;
    auto first_byte = *buffer->begin();
    if (first_byte < 128 || first_byte > 191)
        return false;
    if (_recv_session == nullptr) {
        // TODO: for now just discard it
        return true;
    }
    auto data_span = std::span<uint8_t>(buffer->data(), buffer->size());

    if (is_rtcp_packet(data_span)) {
        auto result = unprotect_rtcp(data_span, data_span);
        if (result.empty())
            return true;
        buffer->consume_back(data_span.size() - result.size());

        auto tmp = std::move(buffer);
        if (_on_rtp_rtcp_packet)
            _on_rtp_rtcp_packet(std::move(tmp));
        return true;
    }
    if (buffer->size() < 12)
        return false;
    if ((first_byte & 0xC0) != 0x80)
        return false;

    uint32_t ssrc = rtp::rtp_packet::get_ssrc(data_span.data());

    bool active = _active_ssrcs.contains(ssrc);
    if (!active) {
        if (_active_ssrcs.size() >= MAX_ACTIVE_SSRCS) {
            // 超过上限，强制拒绝，防止 DoS
            return false;
        }
        bool accept = false;
        if (_on_new_ssrc)
            accept = _on_new_ssrc(ssrc, data_span);
        if (!accept)
            return false;
        _active_ssrcs.insert(ssrc);
    }
    asioice::utils::scope_guard on_exit([&]() noexcept {
        if (!active) {
            _active_ssrcs.erase(ssrc);
        }
    });
    auto result = unprotect_rtp(data_span, data_span);
    if (result.empty())
        return true;

    buffer->consume_back(data_span.size() - result.size());

    auto tmp = std::move(buffer);
    if (_on_rtp_rtcp_packet)
        _on_rtp_rtcp_packet(std::move(tmp));
    on_exit.dismiss();
    return true;
}

void srtp_transport_base::remove_incoming_ssrc(uint32_t ssrc) noexcept {
    _active_ssrcs.erase(ssrc);
    ::srtp_stream_remove(static_cast<::srtp_t>(_recv_session), ssrc);
}

remote_sdp parse_remote_sdp(std::string_view sdp) {
    remote_sdp d;
    std::string_view r = sdp;
    while (!r.empty()) {
        auto p = r.find("\r\n");
        if (p == std::string_view::npos)
            p = r.find('\n');
        auto l = r.substr(0, p);
        r.remove_prefix(
            p == std::string_view::npos
                ? r.size()
                : p + (l.size() < r.size() && r[l.size()] == '\r' ? 2 : 1));

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

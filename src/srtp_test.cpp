#include "srtp_transport.hpp"
#include "asioice/ssl/dtls_config.hpp"

#include <cstring>
#include <iostream>
#include <vector>
#include <algorithm>

using namespace asioice::ssl;
using namespace asiortc;

static std::vector<uint8_t> make_key(size_t n) {
    std::vector<uint8_t> k(n);
    for (size_t i = 0; i < n; ++i)
        k[i] = static_cast<uint8_t>(i + 1);
    return k;
}

static std::vector<uint8_t> make_reversed_key(size_t n) {
    auto k = make_key(n);
    std::reverse(k.begin(), k.end());
    return k;
}

static void key_lengths_for_profile(srtp_protection_profile p, size_t& key_len,
                                    size_t& salt_len) {
    if (p == srtp_protection_profile::srtp_aead_aes_256_gcm) {
        key_len = 32;
        salt_len = 12;
    } else if (p == srtp_protection_profile::srtp_aead_aes_128_gcm) {
        key_len = 16;
        salt_len = 12;
    } else {
        key_len = 16;
        salt_len = 14;
    }
}

static srtp_key_material make_test_keys(srtp_protection_profile p) {
    size_t key_len, salt_len;
    key_lengths_for_profile(p, key_len, salt_len);

    srtp_key_material m;
    m.profile = p;
    m.client_write_key = make_key(key_len);
    m.server_write_key = make_reversed_key(key_len);
    m.client_write_salt = make_key(salt_len);
    m.server_write_salt = make_reversed_key(salt_len);
    return m;
}

static std::vector<uint8_t> make_rtp_packet(uint16_t seq = 1,
                                           uint32_t ssrc = 0x12345678) {
    std::vector<uint8_t> pkt(12);
    // RTP header: V=2, P=0, X=0, CC=0, M=0, PT=96
    pkt[0] = 0x80;
    pkt[1] = 0x60;
    pkt[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    pkt[3] = static_cast<uint8_t>(seq & 0xFF);
    pkt[4] = 0x00;
    pkt[5] = 0x00;
    pkt[6] = 0x00;
    pkt[7] = 0x01; // timestamp
    pkt[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
    pkt[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    pkt[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
    pkt[11] = static_cast<uint8_t>(ssrc & 0xFF);
    for (size_t i = 0; i < 20; ++i)
        pkt.push_back(static_cast<uint8_t>(i));
    return pkt;
}

static std::vector<uint8_t>
make_rtcp_packet() {
    std::vector<uint8_t> pkt(8);
    pkt[0] = 0x80;       // V=2, P=0, RC=0
    pkt[1] = 0xC8;       // PT=200 (Sender Report)
    pkt[2] = 0x00;
    pkt[3] = 0x01;       // length = 1 (28 bytes total, in 32-bit words minus 1)
    pkt[4] = 0x12;
    pkt[5] = 0x34;
    pkt[6] = 0x56;
    pkt[7] = 0x78;       // SSRC
    for (size_t i = 0; i < 20; ++i)
        pkt.push_back(static_cast<uint8_t>(i));
    return pkt;
}

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": "       \
                      << #cond << "\n";                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

static void test_rtp_roundtrip(srtp_protection_profile p) {
    auto keys = make_test_keys(p);

    srtp_transport_base client(keys, dtls_role::client);
    srtp_transport_base server(keys, dtls_role::server);

    ASSERT(client.profile() == p);
    ASSERT(server.profile() == p);

    // client -> server RTP
    {
        auto pkt = make_rtp_packet(1);
        auto original = pkt;
        pkt.resize(pkt.size() + srtp_transport_base::max_protect_rtp_overhead());

        pkt.resize(client.protect_rtp(original, pkt).size());
        ASSERT(pkt.size() > original.size());

        pkt.resize(server.unprotect_rtp(pkt, pkt).size());
        ASSERT(pkt.size() == original.size());
        ASSERT(std::memcmp(pkt.data(), original.data(), pkt.size()) == 0);
    }

    // server -> client RTP
    {
        auto pkt = make_rtp_packet(2);
        auto original = pkt;
        pkt.resize(pkt.size() + srtp_transport_base::max_protect_rtp_overhead());

        pkt.resize(server.protect_rtp(original, pkt).size());
        ASSERT(pkt.size() > original.size());

        pkt.resize(client.unprotect_rtp(pkt, pkt).size());
        ASSERT(pkt.size() == original.size());
        ASSERT(std::memcmp(pkt.data(), original.data(), pkt.size()) == 0);
    }

    // Auth failure: corrupt protected packet
    {
        auto pkt = make_rtp_packet(3);
        std::size_t old = pkt.size();
        pkt.resize(pkt.size() + srtp_transport_base::max_protect_rtp_overhead());

        pkt.resize(client.protect_rtp({pkt.data(), old}, pkt).size());
        ASSERT(!pkt.empty());
        // Flip a bit in the auth tag area
        pkt.back() ^= 0x01;
        ASSERT(server.unprotect_rtp(pkt, pkt).empty());
    }

    std::cout << "  RTP round-trip OK\n";
}

static void test_rtcp_roundtrip(srtp_protection_profile p) {
    auto keys = make_test_keys(p);

    srtp_transport_base client(keys, dtls_role::client);
    srtp_transport_base server(keys, dtls_role::server);

    // client -> server RTCP
    {
        auto pkt = make_rtcp_packet();
        auto original = pkt;
        pkt.resize(pkt.size() + srtp_transport_base::max_protect_rtcp_overhead());

        pkt.resize(client.protect_rtcp(original, pkt).size());
        ASSERT(pkt.size() > original.size());

        pkt.resize(server.unprotect_rtcp(pkt, pkt).size());
        ASSERT(pkt.size() == original.size());
        ASSERT(std::memcmp(pkt.data(), original.data(), pkt.size()) == 0);
    }

    // server -> client RTCP
    {
        auto pkt = make_rtcp_packet();
        auto original = pkt;
        pkt.resize(pkt.size() + srtp_transport_base::max_protect_rtcp_overhead());

        pkt.resize(server.protect_rtcp(original, pkt).size());
        ASSERT(pkt.size() > original.size());

        pkt.resize(client.unprotect_rtcp(pkt, pkt).size());
        ASSERT(pkt.size() == original.size());
        ASSERT(std::memcmp(pkt.data(), original.data(), pkt.size()) == 0);
    }

    std::cout << "  RTCP round-trip OK\n";
}

static void test_profile(srtp_protection_profile p) {
    std::cout << "  RTP:\n";
    test_rtp_roundtrip(p);
    std::cout << "  RTCP:\n";
    test_rtcp_roundtrip(p);
}

static void test_move() {
    auto keys = make_test_keys(srtp_protection_profile::srtp_aes128_cm_sha1_80);
    srtp_transport_base t1(keys, dtls_role::client);
    ASSERT(t1.profile() == srtp_protection_profile::srtp_aes128_cm_sha1_80);

    srtp_transport_base t2(std::move(t1));
    ASSERT(t2.profile() == srtp_protection_profile::srtp_aes128_cm_sha1_80);

    auto pkt = make_rtp_packet();
    auto original = pkt;
    auto original_size = pkt.size();
    pkt.resize(pkt.size() + srtp_transport_base::max_protect_rtp_overhead());
    size_t len = original_size;

    pkt.resize(t2.protect_rtp(original, pkt).size());
    ASSERT(pkt.size() > original_size);

    auto server =
        srtp_transport_base(keys, dtls_role::server);
    pkt.resize(server.unprotect_rtp(pkt, pkt).size());
    ASSERT(pkt.size() == original.size());
    ASSERT(std::memcmp(pkt.data(), original.data(), pkt.size()) == 0);

    srtp_transport_base t3(keys, dtls_role::client);
    t3 = std::move(t2);

    pkt = make_rtp_packet(2);
    original = pkt;
    original_size = pkt.size();
    pkt.resize(original_size + srtp_transport_base::max_protect_rtp_overhead());
    len = original_size;

    pkt.resize(t3.protect_rtp(original, pkt).size());
    ASSERT(!pkt.empty());

    std::cout << "Move OK\n";
}

static void test_sdp_parse() {
    std::string sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 0.0.0.0\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=ice-ufrag:testUfrag\r\n"
        "a=ice-pwd:testPwd123\r\n"
        "a=fingerprint:sha-256 AB:CD:EF:01:02:03\r\n"
        "a=setup:active\r\n"
        "a=candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host\r\n"
        "a=sctp-port:5000\r\n";

    auto d = parse_remote_sdp(sdp);

    ASSERT(d.ice_ufrag == "testUfrag");
    ASSERT(d.ice_pwd == "testPwd123");
    ASSERT(d.fingerprint == "sha-256 AB:CD:EF:01:02:03");
    ASSERT(d.setup == "active");
    ASSERT(d.candidates.size() == 1);
    ASSERT(d.candidates[0] ==
           "candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host");

    // Parse with \n only (no \r\n)
    std::string sdp_lf =
        "v=0\n"
        "a=ice-ufrag:lfOnly\n"
        "a=ice-pwd:pass\n"
        "a=fingerprint:sha-256 FF:EE:DD\n"
        "a=setup:passive\n";

    auto d2 = parse_remote_sdp(sdp_lf);
    ASSERT(d2.ice_ufrag == "lfOnly");
    ASSERT(d2.fingerprint == "sha-256 FF:EE:DD");
    ASSERT(d2.setup == "passive");

    // Empty SDP
    auto d3 = parse_remote_sdp("");
    ASSERT(d3.ice_ufrag.empty());
    ASSERT(d3.candidates.empty());

    std::cout << "SDP parse OK\n";
}

int main() {
    std::cout << "SRTP_AES128_CM_SHA1_80:\n";
    test_profile(srtp_protection_profile::srtp_aes128_cm_sha1_80);

    std::cout << "SRTP_AES128_CM_SHA1_32:\n";
    test_profile(srtp_protection_profile::srtp_aes128_cm_sha1_32);

    std::cout << "SRTP_AEAD_AES_128_GCM:\n";
    test_profile(srtp_protection_profile::srtp_aead_aes_128_gcm);

    std::cout << "SRTP_AEAD_AES_256_GCM:\n";
    test_profile(srtp_protection_profile::srtp_aead_aes_256_gcm);

    std::cout << "Move semantics:\n";
    test_move();

    std::cout << "SDP parsing:\n";
    test_sdp_parse();

    std::cout << "All tests passed\n";
    return 0;
}

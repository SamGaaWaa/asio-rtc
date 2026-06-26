#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace asiortc::rtcp {

struct packet_type {
    static constexpr uint8_t SR   = 200;
    static constexpr uint8_t RR   = 201;
    static constexpr uint8_t SDES = 202;
    static constexpr uint8_t BYE  = 203;
    static constexpr uint8_t APP  = 204;
    static constexpr uint8_t RTPFB = 205;
    static constexpr uint8_t PSFB  = 206;

    static constexpr uint8_t RTPFB_NACK = 1;
    static constexpr uint8_t RTPFB_TCC  = 15;
    static constexpr uint8_t PSFB_PLI   = 1;
    static constexpr uint8_t PSFB_FIR   = 4;
    static constexpr uint8_t PSFB_APP   = 15;
};

struct report_block {
    uint32_t ssrc = 0;
    uint8_t fraction_lost = 0;
    uint32_t cumulative_lost = 0;
    uint32_t ext_highest_seq = 0;
    uint32_t jitter = 0;
    uint32_t lsr = 0;
    uint32_t dlsr = 0;
};

struct rtcp_packet {
    uint8_t version = 2;
    uint8_t padding = 0;
    uint8_t report_count = 0;
    uint8_t type = 0;
    uint32_t ssrc = 0;

    uint64_t ntp_timestamp = 0;
    uint32_t rtp_timestamp = 0;
    uint32_t sender_packet_count = 0;
    uint32_t sender_octet_count = 0;

    std::vector<report_block> blocks;

    std::vector<uint8_t> payload;

    static std::optional<rtcp_packet> parse(const void *data,
                                            std::size_t len) noexcept;
    int write_to(void *data, std::size_t len) const noexcept;
    std::size_t serialized_size() const noexcept;

    static bool is_rtcp_packet(const void *data, std::size_t len) noexcept;
    static uint8_t get_packet_type(const void *data,
                                    std::size_t len) noexcept;
};

// RTCP Feedback: NACK
struct rtcp_rtpfb {
    uint8_t fmt = 1;  // NACK
    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc = 0;
    std::vector<uint16_t> lost;

    std::vector<uint8_t> bytes() const;
};

// RTCP Feedback: PLI / FIR / REMB
struct rtcp_psfb {
    uint8_t fmt = 1;  // PLI
    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc = 0;
    std::vector<uint8_t> fci;

    std::vector<uint8_t> bytes() const;
};

// REMB FCI
std::pair<uint32_t, std::vector<uint32_t>>
parse_remb(const uint8_t *data, size_t len);

std::vector<uint8_t> pack_remb(uint32_t bitrate,
                                const std::vector<uint32_t> &ssrcs);

// SDES
struct sdes_chunk {
    uint32_t ssrc = 0;
    std::string cname;

    std::vector<uint8_t> bytes() const;
};

std::vector<uint16_t> parse_nack(const uint8_t *data, size_t len);

std::vector<rtcp_packet> parse_compound(const void *data,
                                        std::size_t len) noexcept;

struct transport_cc_feedback {
    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc = 0;
    uint16_t base_seq = 0;
    uint16_t status_count = 0;
    uint32_t reference_time = 0;
    uint8_t feedback_packet_count = 0;
    std::vector<uint8_t> packet_chunks;
};

std::optional<transport_cc_feedback>
parse_transport_cc(const uint8_t *data, size_t len);

std::vector<uint8_t>
build_transport_cc(const transport_cc_feedback &fb);

enum class tcc_packet_status : uint8_t {
    not_received = 0,
    small_delta = 1,
    large_delta = 2,
};

struct tcc_packet_info {
    tcc_packet_status status;
    int16_t delta = 0;
};

std::vector<tcc_packet_info>
tcc_parse_packet_status(const transport_cc_feedback &fb);

std::vector<uint8_t>
tcc_build_packet_status(const std::vector<tcc_packet_info> &packets);

} // namespace asiortc::rtcp

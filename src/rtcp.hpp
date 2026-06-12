#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace asiortc::rtcp {

struct packet_type {
    static constexpr uint8_t SR = 200;
    static constexpr uint8_t RR = 201;
    static constexpr uint8_t SDES = 202;
    static constexpr uint8_t BYE = 203;
    static constexpr uint8_t APP = 204;
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

std::vector<rtcp_packet> parse_compound(const void *data,
                                        std::size_t len) noexcept;

} // namespace asiortc::rtcp

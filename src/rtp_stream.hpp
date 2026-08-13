#pragma once

#include "asiortc/media_frame.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace asiortc {

struct rtp_stream {
    rtp_stream() = default;

    rtp_stream(const rtp_stream&) = delete;
    rtp_stream& operator=(const rtp_stream&) = delete;
    rtp_stream(rtp_stream&&) = delete;
    rtp_stream& operator=(rtp_stream&&) = delete;

    uint32_t ssrc = 0;
    uint32_t rtx_ssrc = 0;
    uint8_t rtx_pt = 97;
    uint16_t seq = 0;
    uint16_t rtx_seq = 0;
    std::string rid;

    uint32_t packet_count = 0;
    uint32_t octet_count = 0;
    uint64_t ntp_timestamp = 0;
    uint32_t rtp_timestamp = 0;

    static constexpr size_t HISTORY_SIZE = 128;
    struct history_entry {
        std::vector<uint8_t> payload;
        uint16_t seq = 0;
        uint32_t timestamp = 0;
    };
    std::array<std::optional<history_entry>, HISTORY_SIZE> history{};
};

} // namespace asiortc

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace asiortc::rtp_packetizer {

struct vpx_payload_descriptor {
    bool partition_start = false;
    uint8_t partition_id = 0;
    std::optional<uint16_t> picture_id;
    std::optional<uint8_t> tl0picidx;
    std::optional<std::pair<uint8_t, uint8_t>> tid;
    std::optional<uint8_t> keyidx;

    static vpx_payload_descriptor
    parse(const uint8_t *data, size_t len, size_t &consumed);

    std::vector<uint8_t> bytes() const;
};

} // namespace asiortc::rtp_packetizer

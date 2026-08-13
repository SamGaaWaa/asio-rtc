#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "rtp_packetizer/base.hpp"

namespace asiortc::rtp_packetizer {

struct OpusPacketizer : rtp_packetizer_base {
    OpusPacketizer() = default;

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data, uint32_t timestamp) override;
};

} // namespace asiortc::rtp_packetizer

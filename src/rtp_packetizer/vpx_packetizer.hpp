#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "rtp_packetizer/base.hpp"

namespace asiortc::rtp_packetizer {

struct Vp8Packetizer : rtp_packetizer_base {
    Vp8Packetizer();

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data, uint32_t timestamp) override;

  private:
    uint16_t _picture_id = 0;
};

struct Vp9Packetizer : rtp_packetizer_base {
    Vp9Packetizer();

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data, uint32_t timestamp) override;

  private:
    uint16_t _picture_id = 0;
};

} // namespace asiortc::rtp_packetizer

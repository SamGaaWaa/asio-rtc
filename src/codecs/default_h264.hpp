#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "asiortc/codecs/base.hpp"

namespace asiortc::codecs {

struct DefaultH264Encoder : encoder {
    DefaultH264Encoder(const encoder_params &p) {}

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    encode(const media_frame &frame, bool force_keyframe) override;

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data, uint32_t timestamp) override;
};

} // namespace asiortc::codecs

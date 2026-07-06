#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "asiortc/media_frame.hpp"

namespace asiortc::codecs {

struct encoder_params {
    std::optional<int> bitrate;        // bps
    std::optional<int> max_framerate;  // fps numerator (den=1)
};

struct encoder {
    encoder() = default;
    encoder(const encoder&) = delete;
    encoder(encoder&&) = delete;
    encoder& operator=(const encoder&) = delete;
    encoder& operator=(encoder&&) = delete;
    virtual ~encoder() = default;

    virtual std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    encode(const media_frame &frame, bool force_keyframe = false) = 0;

    virtual std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data, uint32_t timestamp) = 0;

    virtual void set_parameters(const encoder_params &p) {}
};

struct decoder {
    decoder() = default;
    decoder(const decoder&) = delete;
    decoder(decoder&&) = delete;
    decoder& operator=(const decoder&) = delete;
    decoder& operator=(decoder&&) = delete;
    virtual ~decoder() = default;

    virtual std::vector<media_frame>
    decode(const std::vector<uint8_t> &rtp_payload,
           uint32_t timestamp) = 0;
};

} // namespace asiortc::codecs

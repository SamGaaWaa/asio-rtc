#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "asiortc/rtp_frame_info.hpp"

namespace asiortc {

enum class media_kind : uint8_t { audio = 0, video = 1 };

enum class media_format : int {
    unknown = -1,
    opus = 200,
    vp8 = 201,
    h264 = 202,
    vp9 = 203
};

constexpr const char *media_format_name(media_format f) {
    switch (f) {
    case media_format::opus:
        return "opus";
    case media_format::vp8:
        return "vp8";
    case media_format::h264:
        return "h264";
    case media_format::vp9:
        return "vp9";
    default:
        return "unknown";
    }
}

struct media_frame {
    media_kind kind{}; // audio / video
    media_format format = media_format::unknown;

    // 核心字段：
    // 时间戳必须是 RTP 时钟域 (视频 90kHz, 音频 48kHz)
    // 这样无论是直通还是转码，都能直接使用，无需转换
    uint32_t timestamp = 0;

    // 视频元数据 (Audio 时忽略)
    int width = 0;
    int height = 0;

    // 音频元数据 (Video 时忽略，建议补充这两个字段)
    int sample_rate = 0; // 例如 48000
    int channels = 0;    // 例如 2

    // 原始数据
    std::vector<uint8_t> data{};

    std::optional<rtp_frame_info> info;
};

} // namespace asiortc
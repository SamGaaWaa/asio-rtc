#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "asioice/task.hpp"

namespace asiortc {

enum class media_kind : uint8_t { audio = 0, video = 1 };

enum class media_format : uint16_t {
    unknown = 0,
    yuv420p = 1,
    nv12 = 2,
    bgra = 3,
    pcm_s16le = 100,
    pcm_f32le = 101,
    opus = 200,
    vp8 = 201,
    h264 = 202,
};

inline bool is_encoded_format(media_format fmt) noexcept {
    using enum media_format;
    return fmt == opus || fmt == vp8 || fmt == h264;
}

enum class track_state : uint8_t { live = 0, ended = 1 };

struct media_frame {
    media_kind kind{};              // audio / video
    media_format format = media_format::unknown;
    
    // 核心字段：
    // 时间戳必须是 RTP 时钟域 (视频 90kHz, 音频 48kHz)
    // 这样无论是直通还是转码，都能直接使用，无需转换
    uint32_t timestamp = 0;         

    // 视频元数据 (Audio 时忽略)
    int width = 0;
    int height = 0;

    // 音频元数据 (Video 时忽略，建议补充这两个字段)
    int sample_rate = 0;            // 例如 48000
    int channels = 0;               // 例如 2

    // 原始数据
    std::vector<uint8_t> data{};
};

struct media_track {
    virtual ~media_track() = default;

    virtual media_kind kind() const noexcept = 0;
    virtual std::string id() const noexcept = 0;
    virtual track_state ready_state() const noexcept = 0;

    virtual void stop() = 0;
    bool stopped() const noexcept {
        return ready_state() == track_state::ended;
    }

    virtual asioice::task<std::optional<media_frame>> recv() = 0;
};

} // namespace asiortc

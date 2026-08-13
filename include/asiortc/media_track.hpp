#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "asiortc/task.hpp"
#include "asiortc/media_frame.hpp"

namespace asiortc {

enum class track_state : uint8_t { live = 0, ended = 1 };

struct encode_target {
    // int max_width = 0;
    // int max_height = 0;
    std::optional<int> max_bitrate;
    std::string rid;
};

struct media_description {
    media_format format = media_format::unknown;
    uint32_t clock_rate = 90000;
    std::string encoding_params;
    std::optional<std::uint8_t> channels;

    static constexpr media_description make_default(media_format format) {
        if (format == media_format::unknown)
            throw std::invalid_argument{"format == media_format::unknown"};
        media_description desc{format};
        if (format == media_format::opus) {
            desc.clock_rate = 48000;
            desc.channels = 2;
        } else if (format == media_format::h264)
            desc.encoding_params = "profile-level-id=42001f";
        return desc;
    }
};

struct media_track {
    virtual ~media_track() = default;

    virtual media_kind kind() const noexcept = 0;
    virtual media_description description() const noexcept = 0;
    virtual const std::string &id() const noexcept = 0;
    virtual track_state ready_state() const noexcept = 0;

    virtual void stop() noexcept = 0;
    bool stopped() const noexcept {
        return ready_state() == track_state::ended;
    }

    virtual asiortc::task<std::vector<media_frame>>
    recv(std::span<const encode_target> layers) = 0;
};

} // namespace asiortc

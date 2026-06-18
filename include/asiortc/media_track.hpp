#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "asioice/task.hpp"

namespace asiortc {

enum class media_kind : uint8_t { audio = 0, video = 1 };

enum class track_state : uint8_t { live = 0, ended = 1 };

struct media_frame {
    media_kind kind{};
    uint32_t ssrc = 0;
    uint32_t timestamp = 0;
    uint8_t payload_type = 0;
    bool marker = false;
    std::vector<uint8_t> data;
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

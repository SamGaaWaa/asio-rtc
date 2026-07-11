#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "asiortc/task.hpp"
#include "asiortc/media_frame.hpp"

namespace asiortc {

enum class track_state : uint8_t { live = 0, ended = 1 };

struct media_track {
    virtual ~media_track() = default;

    virtual media_kind kind() const noexcept = 0;
    virtual media_format format() const noexcept = 0;
    virtual std::string id() const noexcept = 0;
    virtual track_state ready_state() const noexcept = 0;

    virtual void stop() = 0;
    bool stopped() const noexcept {
        return ready_state() == track_state::ended;
    }

    virtual asiortc::task<std::optional<media_frame>> recv() = 0;
};

} // namespace asiortc

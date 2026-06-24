#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

#include "asiortc/media_track.hpp"

namespace asiortc {

class jitter_buffer {
  public:
    explicit jitter_buffer(
        std::chrono::milliseconds max_delay = std::chrono::milliseconds(500));

    void push(media_frame frame);

    std::optional<media_frame> pop();

    void reset();

  private:
    static uint32_t _extend_seq(uint16_t seq, uint32_t last_extended);

    std::map<uint32_t, media_frame> _sorted;
    uint32_t _next_extended_seq = 0;
    bool _first_packet = true;
    std::chrono::milliseconds _max_delay;
    std::optional<std::chrono::steady_clock::time_point> _gap_start;
};

} // namespace asiortc

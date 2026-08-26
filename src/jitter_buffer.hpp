#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "asiortc/rtp.hpp"

namespace asiortc {

class jitter_buffer {
  public:
    explicit jitter_buffer(
        std::chrono::milliseconds max_delay = std::chrono::milliseconds(500),
        bool is_video = false);

    void push(rtp::rtp_packet pkt);

    std::vector<rtp::rtp_packet> pop_frame();

    void reset();

  private:
    static uint32_t _extend_seq(uint16_t seq, uint32_t last_extended);

    std::map<uint32_t, rtp::rtp_packet> _sorted;
    uint32_t _next_extended_seq = 0;
    bool _first_packet = true;
    std::chrono::milliseconds _max_delay;
    std::optional<std::chrono::steady_clock::time_point> _gap_start;
    std::optional<std::chrono::steady_clock::time_point> _video_wait_start;
    bool _is_video = false;
};

} // namespace asiortc

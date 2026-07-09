#pragma once

#include <chrono>
#include <cstdint>

namespace asiortc {

struct rtp_frame_info {
    uint32_t rtp_timestamp = 0;
    uint32_t ssrc = 0;
    uint32_t clock_rate = 0;
    uint16_t first_sequence_number = 0;
    bool marker = false;
    std::chrono::microseconds receive_time{0};
};

} // namespace asiortc

#include "jitter_buffer.hpp"

#include <vector>

namespace asiortc {

jitter_buffer::jitter_buffer(std::chrono::milliseconds max_delay,
                             bool is_video)
    : _max_delay(max_delay), _is_video(is_video) {}

uint32_t jitter_buffer::_extend_seq(uint16_t seq, uint32_t last_extended) {
    uint32_t extended = (last_extended & 0xFFFF0000) | seq;
    if (extended + 0x8000 < last_extended)
        extended += 0x10000;
    return extended;
}

void jitter_buffer::push(rtp::rtp_packet pkt) {
    uint32_t extended = _extend_seq(pkt.sequence_number,
                                    _next_extended_seq);

    if (_first_packet) {
        _first_packet = false;
        _next_extended_seq = extended;
    }

    if (extended < _next_extended_seq)
        return;

    auto it = _sorted.find(extended);
    if (it != _sorted.end())
        return;

    _sorted.emplace(extended, std::move(pkt));
}

std::optional<rtp::rtp_packet> jitter_buffer::pop_frame() {
    if (_sorted.empty())
        return std::nullopt;

    auto it = _sorted.begin();
    uint32_t extended = it->first;

    if (extended > _next_extended_seq) {
        auto now = std::chrono::steady_clock::now();
        if (!_gap_start)
            _gap_start = now;
        if (now - *_gap_start >= _max_delay) {
            _next_extended_seq = extended;
            _gap_start.reset();
        } else {
            return std::nullopt;
        }
    }

    if (extended < _next_extended_seq) {
        _sorted.erase(it);
        return pop_frame();
    }

    // extended == _next_extended_seq
    _gap_start.reset();
    uint32_t ts = it->second.timestamp;
    uint32_t next_expected = _next_extended_seq;

    // Scan forward: collect all consecutive packets with the same timestamp
    while (it != _sorted.end() && it->first == next_expected &&
           it->second.timestamp == ts) {
        ++next_expected;
        ++it;
    }

    // Check if frame is complete: next entry either has a different timestamp
    // or there's a gap.  If the next entry has the same timestamp, the frame
    // is still incomplete → wait for more packets.
    if (it != _sorted.end() && it->first == next_expected &&
        it->second.timestamp == ts) {
        return std::nullopt;
    }

    // For video: if we reached end of buffer, we may have only partial
    // fragments of a multi-packet frame.  Wait for more packets via timeout.
    if (_is_video && it == _sorted.end()) {
        auto now = std::chrono::steady_clock::now();
        if (!_video_wait_start)
            _video_wait_start = now;
        if (now - *_video_wait_start < _max_delay / 2)
            return std::nullopt;
    } else {
        _video_wait_start.reset();
    }

    // Frame is complete — concatenate all payloads
    std::vector<uint8_t> joined;
    auto erase_end = it;
    it = _sorted.begin();
    while (it != erase_end) {
        auto &p = it->second.payload;
        joined.insert(joined.end(), p.begin(), p.end());
        ++it;
    }

    rtp::rtp_packet assembled;
    assembled.sequence_number =
        static_cast<uint16_t>(_next_extended_seq);
    assembled.timestamp = ts;
    assembled.payload = std::move(joined);

    _next_extended_seq = next_expected;
    _sorted.erase(_sorted.begin(), erase_end);
    return assembled;
}

void jitter_buffer::reset() {
    _sorted.clear();
    _next_extended_seq = 0;
    _first_packet = true;
    _gap_start.reset();
    _video_wait_start.reset();
}

} // namespace asiortc

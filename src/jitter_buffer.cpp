#include "jitter_buffer.hpp"

namespace asiortc {

jitter_buffer::jitter_buffer(std::chrono::milliseconds max_delay)
    : _max_delay(max_delay) {}

uint32_t jitter_buffer::_extend_seq(uint16_t seq, uint32_t last_extended) {
    uint32_t extended = (last_extended & 0xFFFF0000) | seq;
    if (extended + 0x8000 < last_extended)
        extended += 0x10000;
    else if (extended - 0x8000 > last_extended)
        extended -= 0x10000;
    return extended;
}

void jitter_buffer::push(media_frame frame) {
    uint32_t extended = _extend_seq(frame.sequence_number,
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

    _sorted.emplace(extended, std::move(frame));
}

std::optional<media_frame> jitter_buffer::pop() {
    if (_sorted.empty())
        return std::nullopt;

    auto it = _sorted.begin();
    uint32_t extended = it->first;

    if (extended == _next_extended_seq) {
        auto frame = std::move(it->second);
        _sorted.erase(it);
        ++_next_extended_seq;
        _gap_start.reset();
        return frame;
    }

    if (extended > _next_extended_seq) {
        auto now = std::chrono::steady_clock::now();
        if (!_gap_start)
            _gap_start = now;
        if (now - *_gap_start >= _max_delay) {
            _next_extended_seq = extended;
            _gap_start.reset();
            return pop();
        }
        return std::nullopt;
    }

    _sorted.erase(it);
    return pop();
}

void jitter_buffer::reset() {
    _sorted.clear();
    _next_extended_seq = 0;
    _first_packet = true;
    _gap_start.reset();
}

} // namespace asiortc

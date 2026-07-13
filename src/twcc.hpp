#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>
#include <functional>

namespace asiortc {

class transport_cc {
  public:
    using send_callback = std::function<void(std::vector<uint8_t>)>;

    transport_cc(uint16_t sender_ssrc = 0, uint16_t media_ssrc = 0) noexcept
        : _sender_ssrc(sender_ssrc), _media_ssrc(media_ssrc) {}

    void handle_incoming(std::span<const uint8_t> extension_data,
                         send_callback cb);

    std::vector<uint8_t> report();

    void set_sender_ssrc(uint32_t sender_ssrc) noexcept {
        _sender_ssrc = sender_ssrc;
    }

    void set_media_ssrc(uint32_t media_ssrc) noexcept {
        _media_ssrc = media_ssrc;
    }

  private:
    struct _entry {
        uint16_t seq;
        std::chrono::steady_clock::time_point ts;
    };

    std::vector<_entry> _recv{};
    uint16_t _next_report_seq{};
    uint32_t _sender_ssrc;
    uint32_t _media_ssrc;
    uint8_t _fb_count = 0;
};

} // namespace asiortc

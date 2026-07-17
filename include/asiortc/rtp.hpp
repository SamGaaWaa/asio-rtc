#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace asiortc::rtp {

struct rtp_ext_element {
    uint8_t id = 0;
    uint8_t length = 0;
    const uint8_t *data = nullptr;
};

struct rtp_ext_sentinel {
    rtp_ext_sentinel() noexcept : _end{nullptr} {}
    explicit rtp_ext_sentinel(const void *end) noexcept
        : _end{static_cast<const uint8_t *>(end)} {}

  private:
    friend struct rtp_ext_iterator;
    const uint8_t *_end;
};

struct rtp_ext_iterator {
    using difference_type = std::ptrdiff_t;
    using value_type = rtp_ext_element;

    rtp_ext_iterator(const void *begin) noexcept
        : _current{(const uint8_t *)begin} {}

    rtp_ext_element operator*() const noexcept {
        uint8_t hdr = *_current;
        uint8_t len = _size();
        return {(uint8_t)((hdr >> 4) & 0xF), len,
                len > 0 ? _current + 1 : (uint8_t *)0};
    }

    rtp_ext_iterator &operator++() noexcept {
        uint8_t len = _size();
        _current += 1 + len;
        return *this;
    }

    rtp_ext_iterator operator++(int) noexcept {
        auto tmp = *this;
        ++*this;
        return tmp;
    }

    bool operator==(const rtp_ext_sentinel &s) const noexcept {
        return _current >= s._end || _current + _size() + 1 > s._end;
    }

  private:
    uint8_t _size() const noexcept { return (*_current & 0xF) + 1; }

    const uint8_t *_current;
};

bool is_rtp_packet(const uint8_t *data, std::size_t len) noexcept;

struct rtp_packet {
    uint8_t version = 2;
    uint8_t padding = 0;
    uint8_t extension = 0;
    uint8_t csrc_count = 0;
    uint8_t marker = 0;
    uint8_t payload_type = 0;
    uint16_t sequence_number = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    std::vector<uint32_t> csrcs;
    uint16_t extension_profile = 0;
    std::vector<uint8_t> extension_data;
    std::vector<uint8_t> payload;

    static std::optional<rtp_packet> parse(const void *data,
                                           std::size_t len) noexcept;
    int write_to(void *data, std::size_t len) const noexcept;
    std::size_t serialized_size() const noexcept;

    static uint32_t get_ssrc(const uint8_t *data) noexcept;
    static uint8_t get_payload_type(const uint8_t *data) noexcept;
    static uint16_t get_sequence_number(const uint8_t *data) noexcept;
};

} // namespace asiortc::rtp

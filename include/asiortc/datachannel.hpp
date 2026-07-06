#pragma once

#include <memory>
#include <cstdint>
#include <vector>
#include <string_view>

#include "asiortc/task.hpp"

namespace asiortc {

struct data_channel;

struct data_channel_message {
    data_channel_message(std::vector<uint8_t> data, bool binary)
        : _data{std::move(data)}, _binary{binary} {}

    bool is_binary() const noexcept { return _binary; }
    std::span<const uint8_t> binary_data() const noexcept { return _data; }
    std::string_view text_data() const noexcept {
        return std::string_view{(const char *)_data.data(), _data.size()};
    }
    std::vector<uint8_t> take_data() noexcept { return std::move(_data); }

  private:
    std::vector<uint8_t> _data{};
    bool _binary{false};
};

enum struct data_channel_ready_state_t : char {
    connecting,
    open,
    closing,
    closed
};

enum struct data_channel_priority : uint16_t {
    very_low = 128,
    low = 256,
    medium = 512,
    high = 1024
};

struct data_channel_options {
    bool ordered = true;
    std::optional<uint32_t> max_packet_life_time{};
    std::optional<uint32_t> max_retransmits{};
    std::string protocol = "";
    bool negotiated = false;
    uint16_t stream_id = 0;
    data_channel_priority priority = data_channel_priority::low;
};

struct data_channel_interface {
    uint16_t stream_id() const noexcept;
    const std::string &label() const noexcept;
    const std::string &protocol() const noexcept;
    bool ordered() const noexcept;
    data_channel_ready_state_t state() const noexcept;
    data_channel_priority priority() const noexcept;

    asiortc::task<bool> open();

    asiortc::task<bool> send(std::string_view text);
    asiortc::task<bool> send(std::span<const uint8_t> data);
    asiortc::task<data_channel_message> read();
    void close() noexcept;

    operator bool() const noexcept { return _impl != nullptr; }

  private:
    friend struct peer_connection;

    std::shared_ptr<data_channel> _impl;
};

} // namespace asiortc
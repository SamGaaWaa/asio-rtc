#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <optional>

namespace asiortc {

enum class sdp_direction : char {
    inactive = 0,
    sendonly = 1,
    recvonly = 2,
    sendrecv = 3
};

struct sdp_extmap {
    uint16_t id = 0;
    std::string uri;
    std::optional<sdp_direction> direction;
    std::string attributes;
};

struct session_description_interface {
    virtual ~session_description_interface(){};
    virtual std::string to_string() const = 0;
    virtual std::string_view type() const noexcept = 0;
};

std::unique_ptr<session_description_interface> parse_sdp(std::string_view sdp,
                                                         std::string_view type);

} // namespace asiortc

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace asiortc {

enum struct candidate_type : uint8_t { host, srflx, prflx, relay };

struct candidate {
    std::string foundation;
    uint8_t component = 1;
    std::string transport_type = "UDP";
    uint32_t priority = 0;
    std::string address;
    uint16_t port = 0;
    candidate_type type = candidate_type::host;
    std::optional<std::string> related_address;
    std::optional<uint16_t> related_port;
    std::string tcptype;
    std::optional<uint32_t> generation;

    std::string to_sdp() const;
    static std::optional<candidate> from_sdp(std::string_view sdp) noexcept;
};

} // namespace asiortc

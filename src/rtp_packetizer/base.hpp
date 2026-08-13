#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace asiortc::rtp_packetizer {

struct rtp_packetizer_base {
    virtual std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data, uint32_t timestamp) = 0;
    virtual ~rtp_packetizer_base() = default;
};

struct rtp_packetizer_factory {
    virtual std::unique_ptr<rtp_packetizer_base> create() = 0;
    virtual ~rtp_packetizer_factory() = default;
};

} // namespace asiortc::rtp_packetizer

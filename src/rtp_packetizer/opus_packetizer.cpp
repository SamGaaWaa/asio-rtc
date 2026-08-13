#include "rtp_packetizer/opus_packetizer.hpp"

namespace asiortc::rtp_packetizer {

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
OpusPacketizer::pack(const std::vector<uint8_t> &encoded_data,
                     uint32_t timestamp) {
    return {{encoded_data}, timestamp};
}

} // namespace asiortc::rtp_packetizer

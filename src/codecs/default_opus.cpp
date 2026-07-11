#include "codecs/default_opus.hpp"

#include <stdexcept>

namespace asiortc::codecs {

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
DefaultOpusEncoder::encode(const media_frame &, bool) {
    throw std::runtime_error{
        "Opus encode not supported: use pre-encoded data via pack()"};
}

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
DefaultOpusEncoder::pack(const std::vector<uint8_t> &encoded_data,
                         uint32_t timestamp) {
    return {{encoded_data}, timestamp};
}

} // namespace asiortc::codecs

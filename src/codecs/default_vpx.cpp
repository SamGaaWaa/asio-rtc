#include "codecs/default_vpx.hpp"

#include "codecs/vpx_descriptor.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace asiortc::codecs {

static constexpr int kPacketMax = 1200;

DefaultVp8Encoder::DefaultVp8Encoder(const encoder_params &p) {
    static thread_local std::random_device rd;
    std::mt19937 gen(rd());
    _picture_id = gen() & 0x7FFF;
}

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
DefaultVp8Encoder::encode(const media_frame &, bool) {
    throw std::runtime_error{
        "VP8 encode not supported: use pre-encoded data via pack()"};
}

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
DefaultVp8Encoder::pack(const std::vector<uint8_t> &data, uint32_t timestamp) {
    vpx_payload_descriptor desc;
    desc.partition_start = true;
    desc.partition_id = 0;
    desc.picture_id = _picture_id;

    std::vector<std::vector<uint8_t>> payloads;
    size_t off = 0;
    while (off < data.size()) {
        auto dbytes = desc.bytes();
        size_t chunk = std::min(
            data.size() - off, static_cast<size_t>(kPacketMax - dbytes.size()));
        std::vector<uint8_t> p;
        p.reserve(dbytes.size() + chunk);
        p.insert(p.end(), dbytes.begin(), dbytes.end());
        p.insert(p.end(), data.begin() + off, data.begin() + off + chunk);
        payloads.push_back(std::move(p));
        desc.partition_start = false;
        off += chunk;
    }
    _picture_id = (_picture_id + 1) & 0x7FFF;
    return {payloads, timestamp};
}

DefaultVp9Encoder::DefaultVp9Encoder(const encoder_params &p) {
    static thread_local std::random_device rd;
    std::mt19937 gen(rd());
    _picture_id = gen() & 0x7FFF;
}

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
DefaultVp9Encoder::encode(const media_frame &, bool) {
    throw std::runtime_error{
        "VP9 encode not supported: use pre-encoded data via pack()"};
}

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
DefaultVp9Encoder::pack(const std::vector<uint8_t> &data, uint32_t timestamp) {
    vpx_payload_descriptor desc;
    desc.partition_start = true;
    desc.partition_id = 0;
    desc.picture_id = _picture_id;

    std::vector<std::vector<uint8_t>> payloads;
    size_t off = 0;
    while (off < data.size()) {
        auto dbytes = desc.bytes();
        size_t chunk = std::min(
            data.size() - off, static_cast<size_t>(kPacketMax - dbytes.size()));
        std::vector<uint8_t> p;
        p.reserve(dbytes.size() + chunk);
        p.insert(p.end(), dbytes.begin(), dbytes.end());
        p.insert(p.end(), data.begin() + off, data.begin() + off + chunk);
        payloads.push_back(std::move(p));
        desc.partition_start = false;
        off += chunk;
    }
    _picture_id = (_picture_id + 1) & 0x7FFF;
    return {payloads, timestamp};
}

} // namespace asiortc::codecs

#include "rtp_packetizer/vpx_packetizer.hpp"

#include "rtp_packetizer/vpx_descriptor.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace asiortc::rtp_packetizer {

static constexpr int kPacketMax = 1200;

Vp8Packetizer::Vp8Packetizer() {
    static thread_local std::random_device rd;
    std::mt19937 gen(rd());
    _picture_id = gen() & 0x7FFF;
}

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
Vp8Packetizer::pack(const std::vector<uint8_t> &data, uint32_t timestamp) {
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

Vp9Packetizer::Vp9Packetizer() {
    static thread_local std::random_device rd;
    std::mt19937 gen(rd());
    _picture_id = gen() & 0x7FFF;
}

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
Vp9Packetizer::pack(const std::vector<uint8_t> &data, uint32_t timestamp) {
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

} // namespace asiortc::rtp_packetizer

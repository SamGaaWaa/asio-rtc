#include "asiortc/rtp.hpp"

#include <cassert>
#include <cstring>

#include "asioice/detail/binary.hpp"

namespace asiortc::rtp {

#pragma pack(push, 1)
struct rtp_header_t {
    uint8_t version_cc;
    uint8_t marker_pt;
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t ssrc;
};

static_assert(sizeof(rtp_header_t) == 12, "RTP fixed header must be 12 bytes");

struct rtp_extension_header_t {
    uint16_t profile_specific;
    uint16_t length;
};

static_assert(sizeof(rtp_extension_header_t) == 4,
              "RTP extension header must be 4 bytes");
#pragma pack(pop)

bool is_rtp_packet(const uint8_t *data, std::size_t len) noexcept {
    if (len < sizeof(rtp_header_t))
        return false;
    return (data[0] >> 6) == 2;
}

std::optional<rtp_packet> rtp_packet::parse(const void *data,
                                            std::size_t len) noexcept {
    if (!is_rtp_packet(static_cast<const uint8_t *>(data), len))
        return {};

    const auto *header = static_cast<const rtp_header_t *>(data);

    rtp_packet pkt;
    pkt.version = (header->version_cc >> 6) & 0x03;
    pkt.padding = (header->version_cc >> 5) & 0x01;
    pkt.extension = (header->version_cc >> 4) & 0x01;
    pkt.csrc_count = header->version_cc & 0x0F;
    pkt.marker = (header->marker_pt >> 7) & 0x01;
    pkt.payload_type = header->marker_pt & 0x7F;
    pkt.sequence_number =
        asioice::binary::ntoh<uint16_t>(header->sequence_number);
    pkt.timestamp = asioice::binary::ntoh<uint32_t>(header->timestamp);
    pkt.ssrc = asioice::binary::ntoh<uint32_t>(header->ssrc);

    const auto *ptr = reinterpret_cast<const uint8_t *>(data);
    std::size_t offset = sizeof(rtp_header_t);

    if (pkt.csrc_count > 0) {
        if (len < offset + pkt.csrc_count * sizeof(uint32_t))
            return {};
        pkt.csrcs.resize(pkt.csrc_count);
        for (uint8_t i = 0; i < pkt.csrc_count; ++i) {
            pkt.csrcs[i] = asioice::binary::read_big<uint32_t>(ptr + offset);
            offset += sizeof(uint32_t);
        }
    }

    if (pkt.extension) {
        if (len < offset + sizeof(rtp_extension_header_t))
            return {};
        const auto *ext =
            reinterpret_cast<const rtp_extension_header_t *>(ptr + offset);
        pkt.extension_profile =
            asioice::binary::ntoh<uint16_t>(ext->profile_specific);
        auto ext_len =
            asioice::binary::ntoh<uint16_t>(ext->length) * sizeof(uint32_t);
        offset += sizeof(rtp_extension_header_t);
        if (len < offset + ext_len)
            return {};
        pkt.extension_data.assign(ptr + offset, ptr + offset + ext_len);
        offset += ext_len;
    }

    pkt.payload.assign(ptr + offset, ptr + len);

    if (pkt.padding && !pkt.payload.empty()) {
        uint8_t pad_len = pkt.payload.back();
        if (pad_len == 0 || pad_len > pkt.payload.size())
            return {};
        pkt.payload.resize(pkt.payload.size() - pad_len);
    }

    return pkt;
}

int rtp_packet::write_to(void *data, std::size_t len) const noexcept {
    auto serialized = serialized_size();
    if (len < serialized)
        return -1;

    auto *header = static_cast<rtp_header_t *>(data);
    header->version_cc =
        static_cast<uint8_t>((version << 6) | (padding << 5) |
                             (extension << 4) | (csrc_count & 0x0F));
    header->marker_pt =
        static_cast<uint8_t>((marker << 7) | (payload_type & 0x7F));
    header->sequence_number = asioice::binary::hton<uint16_t>(sequence_number);
    header->timestamp = asioice::binary::hton<uint32_t>(timestamp);
    header->ssrc = asioice::binary::hton<uint32_t>(ssrc);

    auto *ptr = reinterpret_cast<uint8_t *>(data) + sizeof(rtp_header_t);

    for (uint32_t csrc : csrcs) {
        asioice::binary::write_big<uint32_t>(ptr, csrc);
        ptr += sizeof(uint32_t);
    }

    if (extension) {
        auto *ext = reinterpret_cast<rtp_extension_header_t *>(ptr);
        ext->profile_specific =
            asioice::binary::hton<uint16_t>(extension_profile);
        ext->length = asioice::binary::hton<uint16_t>(
            static_cast<uint16_t>((extension_data.size() + 3) / 4));
        ptr += sizeof(rtp_extension_header_t);
        if (!extension_data.empty()) {
            std::memcpy(ptr, extension_data.data(), extension_data.size());
            ptr += extension_data.size();
            std::size_t pad = (4 - extension_data.size() % 4) % 4;
            if (pad) {
                std::memset(ptr, 0, pad);
                ptr += pad;
            }
        }
    }

    if (!payload.empty()) {
        std::memcpy(ptr, payload.data(), payload.size());
        ptr += payload.size();
    }

    if (padding) {
        std::size_t written = ptr - reinterpret_cast<uint8_t *>(data);
        std::size_t pad = serialized - written;
        if (pad > 0) {
            std::memset(ptr, 0, pad - 1);
            ptr[pad - 1] = static_cast<uint8_t>(pad);
        }
    }

    return static_cast<int>(serialized);
}

std::size_t rtp_packet::serialized_size() const noexcept {
    std::size_t total = sizeof(rtp_header_t);
    total += csrc_count * sizeof(uint32_t);

    if (extension) {
        total += sizeof(rtp_extension_header_t);
        total += extension_data.size();
        std::size_t ext_pad = (4 - extension_data.size() % 4) % 4;
        total += ext_pad;
    }

    total += payload.size();

    if (padding) {
        std::size_t rem = total % 4;
        if (rem == 0)
            total += 4;
        else
            total += 4 - rem;
    }

    return total;
}

uint32_t rtp_packet::get_ssrc(const uint8_t *data) noexcept {
    const auto *hd = reinterpret_cast<const rtp_header_t *>(data);
    return asioice::binary::ntoh<uint32_t>(hd->ssrc);
}

uint8_t rtp_packet::get_payload_type(const uint8_t *data) noexcept {
    return data[1] & 0x7F;
}

uint16_t rtp_packet::get_sequence_number(const uint8_t *data) noexcept {
    const auto *hd = reinterpret_cast<const rtp_header_t *>(data);
    return asioice::binary::ntoh<uint16_t>(hd->sequence_number);
}

} // namespace asiortc::rtp

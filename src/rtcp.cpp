#include "rtcp.hpp"

#include "asioice/detail/binary.hpp"

#include <cstring>

namespace asiortc::rtcp {

#pragma pack(push, 1)
struct rtcp_header_t {
    uint8_t version_p_rc;
    uint8_t type;
    uint16_t length;
};

static_assert(sizeof(rtcp_header_t) == 4,
              "RTCP header must be 4 bytes");

struct rtcp_sr_sender_info_t {
    uint32_t ntp_ts_msw;
    uint32_t ntp_ts_lsw;
    uint32_t rtp_ts;
    uint32_t sender_packet_count;
    uint32_t sender_octet_count;
};

static_assert(sizeof(rtcp_sr_sender_info_t) == 20,
              "RTCP SR sender info must be 20 bytes");

struct rtcp_report_block_t {
    uint32_t ssrc;
    uint32_t fraction_lost_cumulative;
    uint32_t ext_highest_seq;
    uint32_t jitter;
    uint32_t lsr;
    uint32_t dlsr;
};

static_assert(sizeof(rtcp_report_block_t) == 24,
              "RTCP report block must be 24 bytes");
#pragma pack(pop)

static report_block
parse_report_block(const void *data) noexcept {
    const auto *rb = static_cast<const rtcp_report_block_t *>(data);
    report_block b;
    b.ssrc = asioice::binary::ntoh<uint32_t>(rb->ssrc);
    uint32_t flc = asioice::binary::ntoh<uint32_t>(rb->fraction_lost_cumulative);
    b.fraction_lost = static_cast<uint8_t>((flc >> 24) & 0xFF);
    b.cumulative_lost = flc & 0x00FFFFFF;
    b.ext_highest_seq =
        asioice::binary::ntoh<uint32_t>(rb->ext_highest_seq);
    b.jitter = asioice::binary::ntoh<uint32_t>(rb->jitter);
    b.lsr = asioice::binary::ntoh<uint32_t>(rb->lsr);
    b.dlsr = asioice::binary::ntoh<uint32_t>(rb->dlsr);
    return b;
}

static void write_report_block(void *data,
                               const report_block &b) noexcept {
    auto *rb = static_cast<rtcp_report_block_t *>(data);
    rb->ssrc = asioice::binary::hton<uint32_t>(b.ssrc);
    uint32_t flc = (static_cast<uint32_t>(b.fraction_lost) << 24) |
                   (b.cumulative_lost & 0x00FFFFFF);
    rb->fraction_lost_cumulative = asioice::binary::hton<uint32_t>(flc);
    rb->ext_highest_seq =
        asioice::binary::hton<uint32_t>(b.ext_highest_seq);
    rb->jitter = asioice::binary::hton<uint32_t>(b.jitter);
    rb->lsr = asioice::binary::hton<uint32_t>(b.lsr);
    rb->dlsr = asioice::binary::hton<uint32_t>(b.dlsr);
}

std::optional<rtcp_packet> rtcp_packet::parse(const void *data,
                                              std::size_t len) noexcept {
    if (len < sizeof(rtcp_header_t))
        return {};

    const auto *header = static_cast<const rtcp_header_t *>(data);

    rtcp_packet pkt;
    pkt.version = (header->version_p_rc >> 6) & 0x03;
    pkt.padding = (header->version_p_rc >> 5) & 0x01;
    pkt.report_count = header->version_p_rc & 0x1F;
    pkt.type = header->type;

    std::size_t total =
        (asioice::binary::ntoh<uint16_t>(header->length) + 1) * 4;

    if (total > len)
        return {};

    const auto *ptr = reinterpret_cast<const uint8_t *>(data);
    std::size_t offset = sizeof(rtcp_header_t);

    switch (pkt.type) {
    case packet_type::SR: {
        if (total < offset + sizeof(uint32_t) +
                         sizeof(rtcp_sr_sender_info_t))
            return {};

        pkt.ssrc = asioice::binary::read_big<uint32_t>(ptr + offset);
        offset += sizeof(uint32_t);

        const auto *si =
            reinterpret_cast<const rtcp_sr_sender_info_t *>(ptr + offset);
        pkt.ntp_timestamp =
            (static_cast<uint64_t>(
                 asioice::binary::ntoh<uint32_t>(si->ntp_ts_msw))
             << 32) |
            asioice::binary::ntoh<uint32_t>(si->ntp_ts_lsw);
        pkt.rtp_timestamp =
            asioice::binary::ntoh<uint32_t>(si->rtp_ts);
        pkt.sender_packet_count =
            asioice::binary::ntoh<uint32_t>(si->sender_packet_count);
        pkt.sender_octet_count =
            asioice::binary::ntoh<uint32_t>(si->sender_octet_count);
        offset += sizeof(rtcp_sr_sender_info_t);

        for (uint8_t i = 0; i < pkt.report_count; ++i) {
            if (total < offset + sizeof(rtcp_report_block_t))
                return {};
            pkt.blocks.push_back(parse_report_block(ptr + offset));
            offset += sizeof(rtcp_report_block_t);
        }
        {
            std::size_t payload_len = total - offset;
            if (pkt.padding && payload_len > 0) {
                uint8_t pad_len = ptr[total - 1];
                if (pad_len == 0 || pad_len > payload_len)
                    return {};
                payload_len -= pad_len;
            }
            if (payload_len > 0)
                pkt.payload.assign(ptr + offset,
                                   ptr + offset + payload_len);
        }
        break;
    }
    case packet_type::RR: {
        if (total < offset + sizeof(uint32_t))
            return {};

        pkt.ssrc = asioice::binary::read_big<uint32_t>(ptr + offset);
        offset += sizeof(uint32_t);

        for (uint8_t i = 0; i < pkt.report_count; ++i) {
            if (total < offset + sizeof(rtcp_report_block_t))
                return {};
            pkt.blocks.push_back(parse_report_block(ptr + offset));
            offset += sizeof(rtcp_report_block_t);
        }
        {
            std::size_t payload_len = total - offset;
            if (pkt.padding && payload_len > 0) {
                uint8_t pad_len = ptr[total - 1];
                if (pad_len == 0 || pad_len > payload_len)
                    return {};
                payload_len -= pad_len;
            }
            if (payload_len > 0)
                pkt.payload.assign(ptr + offset,
                                   ptr + offset + payload_len);
        }
        break;
    }
    case packet_type::BYE:
    case packet_type::SDES:
    case packet_type::APP: {
        std::size_t payload_len = total - offset;
        if (pkt.padding && payload_len > 0) {
            uint8_t pad_len = ptr[total - 1];
            if (pad_len == 0 || pad_len > payload_len)
                return {};
            payload_len -= pad_len;
        }
        if (payload_len > 0) {
            pkt.payload.assign(ptr + offset, ptr + offset + payload_len);
        }
        break;
    }
    default:
        break;
    }

    return pkt;
}

int rtcp_packet::write_to(void *data, std::size_t len) const noexcept {
    auto serialized = serialized_size();
    if (len < serialized)
        return -1;

    auto *header = static_cast<rtcp_header_t *>(data);
    header->version_p_rc =
        static_cast<uint8_t>((version << 6) | (padding << 5) |
                              (report_count & 0x1F));
    header->type = type;
    header->length =
        asioice::binary::hton<uint16_t>(static_cast<uint16_t>(serialized / 4 - 1));

    auto *ptr = reinterpret_cast<uint8_t *>(data) + sizeof(rtcp_header_t);

    if (type == packet_type::SR || type == packet_type::RR) {
        asioice::binary::write_big<uint32_t>(ptr, ssrc);
        ptr += sizeof(uint32_t);
    }

    if (type == packet_type::SR) {
        auto *si = reinterpret_cast<rtcp_sr_sender_info_t *>(ptr);
        si->ntp_ts_msw =
            asioice::binary::hton<uint32_t>(
                static_cast<uint32_t>(ntp_timestamp >> 32));
        si->ntp_ts_lsw =
            asioice::binary::hton<uint32_t>(
                static_cast<uint32_t>(ntp_timestamp & 0xFFFFFFFF));
        si->rtp_ts = asioice::binary::hton<uint32_t>(rtp_timestamp);
        si->sender_packet_count =
            asioice::binary::hton<uint32_t>(sender_packet_count);
        si->sender_octet_count =
            asioice::binary::hton<uint32_t>(sender_octet_count);
        ptr += sizeof(rtcp_sr_sender_info_t);
    }

    if (!blocks.empty()) {
        for (const auto &b : blocks) {
            write_report_block(ptr, b);
            ptr += sizeof(rtcp_report_block_t);
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

std::size_t rtcp_packet::serialized_size() const noexcept {
    std::size_t total = sizeof(rtcp_header_t);

    if (type == packet_type::SR || type == packet_type::RR)
        total += sizeof(uint32_t);

    if (type == packet_type::SR)
        total += sizeof(rtcp_sr_sender_info_t);

    total += blocks.size() * sizeof(rtcp_report_block_t);
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

std::vector<rtcp_packet> parse_compound(const void *data,
                                        std::size_t len) noexcept {
    std::vector<rtcp_packet> packets;
    const auto *ptr = static_cast<const uint8_t *>(data);
    std::size_t offset = 0;

    while (offset + sizeof(rtcp_header_t) <= len) {
        const auto *header =
            reinterpret_cast<const rtcp_header_t *>(ptr + offset);
        std::size_t pkt_len =
            (asioice::binary::ntoh<uint16_t>(header->length) + 1) * 4;

        if (offset + pkt_len > len)
            break;

        auto pkt = rtcp_packet::parse(ptr + offset, pkt_len);
        if (!pkt)
            break;

        packets.push_back(std::move(*pkt));
        offset += pkt_len;
    }

    return packets;
}

bool rtcp_packet::is_rtcp_packet(const void *data,
                                  std::size_t len) noexcept {
    if (len < sizeof(rtcp_header_t))
        return false;
    const auto *buf = static_cast<const uint8_t *>(data);
    return (buf[0] >> 6) == 2 && buf[1] >= 192;
}

uint8_t rtcp_packet::get_packet_type(const void *data,
                                     std::size_t len) noexcept {
    if (len < sizeof(rtcp_header_t))
        return 0;
    const auto *buf = static_cast<const uint8_t *>(data);
    return buf[1];
}

} // namespace asiortc::rtcp

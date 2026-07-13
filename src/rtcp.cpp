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

static_assert(sizeof(rtcp_header_t) == 4, "RTCP header must be 4 bytes");

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

static report_block parse_report_block(const void *data) noexcept {
    const auto *rb = static_cast<const rtcp_report_block_t *>(data);
    report_block b;
    b.ssrc = asioice::binary::ntoh<uint32_t>(rb->ssrc);
    uint32_t flc =
        asioice::binary::ntoh<uint32_t>(rb->fraction_lost_cumulative);
    b.fraction_lost = static_cast<uint8_t>((flc >> 24) & 0xFF);
    b.cumulative_lost = flc & 0x00FFFFFF;
    b.ext_highest_seq = asioice::binary::ntoh<uint32_t>(rb->ext_highest_seq);
    b.jitter = asioice::binary::ntoh<uint32_t>(rb->jitter);
    b.lsr = asioice::binary::ntoh<uint32_t>(rb->lsr);
    b.dlsr = asioice::binary::ntoh<uint32_t>(rb->dlsr);
    return b;
}

static void write_report_block(void *data, const report_block &b) noexcept {
    auto *rb = static_cast<rtcp_report_block_t *>(data);
    rb->ssrc = asioice::binary::hton<uint32_t>(b.ssrc);
    uint32_t flc = (static_cast<uint32_t>(b.fraction_lost) << 24) |
                   (b.cumulative_lost & 0x00FFFFFF);
    rb->fraction_lost_cumulative = asioice::binary::hton<uint32_t>(flc);
    rb->ext_highest_seq = asioice::binary::hton<uint32_t>(b.ext_highest_seq);
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
        if (total < offset + sizeof(uint32_t) + sizeof(rtcp_sr_sender_info_t))
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
        pkt.rtp_timestamp = asioice::binary::ntoh<uint32_t>(si->rtp_ts);
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
                pkt.payload.assign(ptr + offset, ptr + offset + payload_len);
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
                pkt.payload.assign(ptr + offset, ptr + offset + payload_len);
        }
        break;
    }
    case packet_type::RTPFB:
    case packet_type::PSFB: {
        if (total < offset + 8)
            return {};
        // sender_ssrc at offset, ignored
        pkt.media_ssrc = asioice::binary::read_big<uint32_t>(ptr + offset + 4);
        offset += 8;
        std::size_t payload_len = total - offset;
        if (pkt.padding && payload_len > 0) {
            uint8_t pad_len = ptr[total - 1];
            if (pad_len == 0 || pad_len > payload_len)
                return {};
            payload_len -= pad_len;
        }
        if (payload_len > 0)
            pkt.payload.assign(ptr + offset, ptr + offset + payload_len);
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
    header->version_p_rc = static_cast<uint8_t>(
        (version << 6) | (padding << 5) | (report_count & 0x1F));
    header->type = type;
    header->length = asioice::binary::hton<uint16_t>(
        static_cast<uint16_t>(serialized / 4 - 1));

    auto *ptr = reinterpret_cast<uint8_t *>(data) + sizeof(rtcp_header_t);

    if (type == packet_type::SR || type == packet_type::RR) {
        asioice::binary::write_big<uint32_t>(ptr, ssrc);
        ptr += sizeof(uint32_t);
    }

    if (type == packet_type::SR) {
        auto *si = reinterpret_cast<rtcp_sr_sender_info_t *>(ptr);
        si->ntp_ts_msw = asioice::binary::hton<uint32_t>(
            static_cast<uint32_t>(ntp_timestamp >> 32));
        si->ntp_ts_lsw = asioice::binary::hton<uint32_t>(
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

bool rtcp_packet::is_rtcp_packet(const void *data, std::size_t len) noexcept {
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

// --- RTPFB NACK ---

std::vector<uint8_t> rtcp_rtpfb::bytes() const {
    size_t n = lost.size();
    size_t hdr = 12; // RTCP header + sender_ssrc + media_ssrc
    size_t fci = n * 4;
    size_t total = hdr + fci;

    std::vector<uint8_t> data(total, 0);
    // RTCP common header
    data[0] = (2 << 6) | 1; // V=2, RC=1
    data[1] = packet_type::RTPFB;
    uint16_t len_words = static_cast<uint16_t>((total / 4) - 1);
    asioice::binary::write_big<uint16_t>(data.data() + 2, len_words);
    // Feedback header
    asioice::binary::write_big<uint32_t>(data.data() + 4, sender_ssrc);
    asioice::binary::write_big<uint32_t>(data.data() + 8, media_ssrc);

    for (size_t i = 0; i < n; ++i) {
        asioice::binary::write_big<uint16_t>(data.data() + 12 + i * 4, lost[i]);
        // BLP = 0 (single loss)
    }
    return data;
}

// --- PSFB (PLI / FIR / REMB) ---

std::vector<uint8_t> rtcp_psfb::bytes() const {
    size_t hdr = 12;
    size_t total = hdr + fci.size();

    std::vector<uint8_t> data(total, 0);
    data[0] = (2 << 6) | fmt;
    data[1] = packet_type::PSFB;
    uint16_t len_words = static_cast<uint16_t>((total / 4) - 1);
    asioice::binary::write_big<uint16_t>(data.data() + 2, len_words);
    asioice::binary::write_big<uint32_t>(data.data() + 4, sender_ssrc);
    asioice::binary::write_big<uint32_t>(data.data() + 8, media_ssrc);
    if (!fci.empty())
        std::memcpy(data.data() + 12, fci.data(), fci.size());
    return data;
}

// --- REMB FCI ---

std::pair<uint32_t, std::vector<uint32_t>> parse_remb(const uint8_t *data,
                                                      size_t len) {
    std::pair<uint32_t, std::vector<uint32_t>> result{0, {}};
    if (len < 8)
        return result;
    if (data[0] != 'R' || data[1] != 'E' || data[2] != 'M' || data[3] != 'B')
        return result;
    uint8_t num_ssrc = data[4];
    uint8_t br_exp = (data[5] >> 2) & 0x3F;
    uint32_t br_mantissa = ((data[5] & 0x03) << 16) | (data[6] << 8) | data[7];
    result.first = br_mantissa << br_exp;

    size_t pos = 8;
    for (uint8_t i = 0; i < num_ssrc && pos + 4 <= len; ++i) {
        result.second.push_back(asioice::binary::ntoh<uint32_t>(
            *reinterpret_cast<const uint32_t *>(data + pos)));
        pos += 4;
    }
    return result;
}

std::vector<uint8_t> pack_remb(uint32_t bitrate,
                               const std::vector<uint32_t> &ssrcs) {
    uint8_t exp = 0;
    while (bitrate > 0x3FFFF) {
        bitrate >>= 1;
        ++exp;
    }
    uint32_t mantissa = bitrate & 0x3FFFF;

    size_t size = 8 + ssrcs.size() * 4;
    std::vector<uint8_t> data(size);
    data[0] = 'R';
    data[1] = 'E';
    data[2] = 'M';
    data[3] = 'B';
    data[4] = static_cast<uint8_t>(ssrcs.size());
    data[5] = static_cast<uint8_t>(exp << 2) |
              static_cast<uint8_t>((mantissa >> 16) & 0x03);
    data[6] = static_cast<uint8_t>((mantissa >> 8) & 0xFF);
    data[7] = static_cast<uint8_t>(mantissa & 0xFF);
    for (size_t i = 0; i < ssrcs.size(); ++i)
        asioice::binary::write_big<uint32_t>(data.data() + 8 + i * 4, ssrcs[i]);
    return data;
}

// --- SDES ---

std::vector<uint8_t> sdes_chunk::bytes() const {
    std::vector<uint8_t> data;
    // SSRC
    data.resize(4);
    asioice::binary::write_big<uint32_t>(data.data(), ssrc);
    // CNAME item: type=1, length, value
    size_t cname_len = cname.size();
    data.push_back(1);
    data.push_back(static_cast<uint8_t>(cname_len));
    data.insert(data.end(), cname.begin(), cname.end());
    // Null terminator (end of chunk)
    data.push_back(0);
    data.push_back(0);
    // Pad to 4-byte boundary
    while (data.size() % 4)
        data.push_back(0);

    // RTCP header
    std::vector<uint8_t> full(4 + data.size());
    full[0] = (2 << 6) | 1; // V=2, SC=1
    full[1] = packet_type::SDES;
    uint16_t len_words = static_cast<uint16_t>((full.size() / 4) - 1);
    asioice::binary::write_big<uint16_t>(full.data() + 2, len_words);
    std::memcpy(full.data() + 4, data.data(), data.size());
    return full;
}

std::vector<uint16_t> parse_nack(const uint8_t *data, size_t len) {
    std::vector<uint16_t> lost;
    const uint8_t *end = data + len;
    while (data + 4 <= end) {
        uint16_t pid = asioice::binary::ntoh<uint16_t>(
            *reinterpret_cast<const uint16_t *>(data));
        uint16_t blp = asioice::binary::ntoh<uint16_t>(
            *reinterpret_cast<const uint16_t *>(data + 2));
        lost.push_back(pid);
        for (int i = 0; i < 16; ++i) {
            if (blp & (1 << i))
                lost.push_back(pid + i + 1);
        }
        data += 4;
    }
    return lost;
}

std::optional<transport_cc_feedback> parse_transport_cc(const uint8_t *data,
                                                        size_t len) {
    if (len < 8)
        return std::nullopt;
    transport_cc_feedback fb;
    fb.base_seq = asioice::binary::ntoh<uint16_t>(
        *reinterpret_cast<const uint16_t *>(data));
    fb.status_count = asioice::binary::ntoh<uint16_t>(
        *reinterpret_cast<const uint16_t *>(data + 2));
    fb.reference_time = (static_cast<uint32_t>(data[4]) << 16) |
                        (static_cast<uint32_t>(data[5]) << 8) |
                        static_cast<uint32_t>(data[6]);
    fb.feedback_packet_count = data[7];
    if (len > 8)
        fb.packet_chunks.assign(data + 8, data + len);
    return fb;
}

std::vector<uint8_t> build_transport_cc(const transport_cc_feedback &fb) {
    size_t fci_size = 8 + fb.packet_chunks.size();
    std::vector<uint8_t> fci(fci_size);
    asioice::binary::write_big<uint16_t>(fci.data(), fb.base_seq);
    asioice::binary::write_big<uint16_t>(fci.data() + 2, fb.status_count);
    fci[4] = static_cast<uint8_t>((fb.reference_time >> 16) & 0xFF);
    fci[5] = static_cast<uint8_t>((fb.reference_time >> 8) & 0xFF);
    fci[6] = static_cast<uint8_t>(fb.reference_time & 0xFF);
    fci[7] = fb.feedback_packet_count;
    if (!fb.packet_chunks.empty())
        std::memcpy(fci.data() + 8, fb.packet_chunks.data(),
                    fb.packet_chunks.size());

    size_t total = 8 + 4 + fci.size();
    std::vector<uint8_t> pkt(total);
    pkt[0] = (2 << 6) | packet_type::RTPFB_TCC;
    pkt[1] = packet_type::RTPFB;
    asioice::binary::write_big<uint16_t>(pkt.data() + 2,
                                         static_cast<uint16_t>(total / 4 - 1));
    asioice::binary::write_big<uint32_t>(pkt.data() + 4, fb.sender_ssrc);
    asioice::binary::write_big<uint32_t>(pkt.data() + 8, fb.media_ssrc);
    std::memcpy(pkt.data() + 12, fci.data(), fci.size());
    size_t orig_size = pkt.size();
    while (pkt.size() % 4 != 0)
        pkt.push_back(0);
    if (pkt.size() > orig_size) {
        pkt[0] |= 0x20;
        pkt.back() = static_cast<uint8_t>(pkt.size() - orig_size);
    }
    asioice::binary::write_big<uint16_t>(
        pkt.data() + 2, static_cast<uint16_t>(pkt.size() / 4 - 1));
    return pkt;
}

std::vector<tcc_packet_info>
tcc_parse_packet_status(const transport_cc_feedback &fb) {
    std::vector<tcc_packet_info> result;
    result.reserve(fb.status_count);

    size_t chunk_bytes = ((fb.status_count + 6) / 7) * 2;
    if (chunk_bytes > fb.packet_chunks.size())
        return result;

    size_t delta_start = chunk_bytes;
    size_t delta_off = 0;

    size_t chunk_off = 0;
    for (uint16_t i = 0; i < fb.status_count;) {
        if (chunk_off + 2 > chunk_bytes)
            break;
        uint16_t chunk =
            asioice::binary::ntoh<uint16_t>(*reinterpret_cast<const uint16_t *>(
                fb.packet_chunks.data() + chunk_off));
        chunk_off += 2;

        int bits = 12;
        for (int j = 0; j < 7 && i < fb.status_count; ++j, ++i) {
            uint8_t sym = (chunk >> bits) & 3;
            int16_t delta = 0;

            if (sym == 1) {
                if (delta_start + delta_off < fb.packet_chunks.size()) {
                    delta = static_cast<int16_t>(
                                fb.packet_chunks[delta_start + delta_off]) -
                            128;
                    delta_off++;
                }
            } else if (sym == 2) {
                if (delta_start + delta_off + 1 < fb.packet_chunks.size()) {
                    delta =
                        static_cast<int16_t>(asioice::binary::ntoh<uint16_t>(
                            *reinterpret_cast<const uint16_t *>(
                                fb.packet_chunks.data() + delta_start +
                                delta_off))) -
                        32768;
                    delta_off += 2;
                }
            }

            result.push_back({sym == 0   ? tcc_packet_status::not_received
                              : sym == 1 ? tcc_packet_status::small_delta
                                         : tcc_packet_status::large_delta,
                              delta});
        }
    }
    return result;
}

std::vector<uint8_t>
tcc_build_packet_status(const std::vector<tcc_packet_info> &packets) {
    std::vector<uint8_t> chunks;
    chunks.reserve((packets.size() / 7 + 1) * 2 + packets.size());
    std::vector<uint8_t> deltas;
    for (size_t i = 0; i < packets.size();) {
        uint16_t chunk = 0xC000;
        int bits = 12;
        for (int j = 0; j < 7 && i < packets.size(); ++j, ++i) {
            uint8_t sym = 0;
            if (packets[i].status == tcc_packet_status::small_delta) {
                sym = 1;
                deltas.push_back(static_cast<uint8_t>(packets[i].delta));
            } else if (packets[i].status == tcc_packet_status::large_delta) {
                sym = 2;
                uint16_t d = static_cast<uint16_t>(packets[i].delta);
                deltas.push_back(static_cast<uint8_t>((d >> 8) & 0xFF));
                deltas.push_back(static_cast<uint8_t>(d & 0xFF));
            } else if (packets[i].status ==
                       tcc_packet_status::received_without_delta) {
                sym = 3;
            }
            chunk |= (sym << bits);
            bits -= 2;
        }

        size_t off = chunks.size();
        chunks.resize(off + 2);
        asioice::binary::write_big<uint16_t>(chunks.data() + off, chunk);
    }

    chunks.insert(chunks.end(), deltas.begin(), deltas.end());
    return chunks;
}

} // namespace asiortc::rtcp

#pragma once

#include "asiortc/media_frame.hpp"
#include "asioice/config.hpp"

#include <vector>
#include <span>
#include <map>
#include <queue>
#include <optional>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <cassert>

namespace asiortc {
namespace detail {

#pragma pack(push, 1)
struct TSHeader {
    uint8_t raw[4];

    bool error() const { return raw[1] & 0x80; }
    bool payload_unit_start_indicator() const { return raw[1] & 0x40; }
    uint16_t pid() const { return ((raw[1] & 0x1F) << 8) | raw[2]; }
    uint8_t adaptation_field_control() const { return (raw[3] >> 4) & 0x03; }
    uint8_t continuity_counter() const { return raw[3] & 0x0F; }
};
static_assert(sizeof(TSHeader) == 4, "TS header must be 4 bytes");
#pragma pack(pop)

class MPEGTSDemuxer {
    static constexpr std::size_t TS_PACKET_SIZE = 188;
    static constexpr std::size_t COMPACT_THRESHOLD = 64 * TS_PACKET_SIZE;
    static constexpr std::size_t MAX_PES_BUFFER = 4 * 1024 * 1024;
    static constexpr std::size_t MAX_FRAME_QUEUE = 64;
    static constexpr std::size_t MAX_PSI_SECTION = 4096;
    static constexpr int MAX_PROGRAMS = 5;

    struct stream_state {
        media_format format = media_format::unknown;
        media_kind kind = media_kind::video;
        std::vector<uint8_t> pes_buffer;
        uint32_t current_pts = 0;
        bool has_pts = false;
        uint8_t continuity_counter = 0;
        bool first_cc = true;
        std::vector<uint8_t> psi_section;
    };

  public:
    ~MPEGTSDemuxer() { flush(); }

    std::optional<media_frame> parse(std::span<const uint8_t> data,
                                     std::size_t &consumed) {
        if (!frame_queue_.empty()) {
            auto frame = std::move(frame_queue_.front());
            frame_queue_.pop();
            return frame;
        }
        if (data.empty()) {
            flush();
            if (!frame_queue_.empty()) {
                auto frame = std::move(frame_queue_.front());
                frame_queue_.pop();
                return frame;
            }
            return std::nullopt;
        }
        if (buffer_.size() - read_offset_ < TS_PACKET_SIZE) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + read_offset_);
            read_offset_ = 0;
        }
        buffer_.insert(buffer_.end(), data.begin(), data.end());
        consumed = data.size();
        process_ts_packets();
        if (!frame_queue_.empty()) {
            auto frame = std::move(frame_queue_.front());
            frame_queue_.pop();
            return frame;
        }
        return std::nullopt;
    }

    void flush() {
        for (auto &[pid, st] : streams_) {
            if (!st.pes_buffer.empty()) {
                media_frame f;
                f.kind = st.kind;
                f.format = st.format;
                if (st.has_pts)
                    f.timestamp = st.current_pts;
                f.data = std::move(st.pes_buffer);
                push_frame(std::move(f));
            }
        }
    }

  private:
    std::vector<uint8_t> buffer_;
    std::size_t read_offset_ = 0;

    struct program_info {
        uint16_t number;
        uint16_t pmt_pid;
    };
    program_info programs_[MAX_PROGRAMS];
    int program_count_ = 0;
    std::map<uint16_t, stream_state> streams_;

    std::queue<media_frame> frame_queue_;

    void push_frame(media_frame f) {
        while (frame_queue_.size() >= MAX_FRAME_QUEUE)
            frame_queue_.pop();
        frame_queue_.push(std::move(f));
    }

    bool is_pmt_pid(uint16_t pid) const {
        for (int i = 0; i < program_count_; ++i)
            if (programs_[i].pmt_pid == pid)
                return true;
        return false;
    }

    void process_ts_packets() {
        while (true) {
            std::size_t sync = find_sync();
            if (sync == std::string::npos)
                return;
            assert(buffer_.size() - read_offset_ >= TS_PACKET_SIZE);
            handle_ts_packet(buffer_.data() + read_offset_);
            read_offset_ += TS_PACKET_SIZE;
        }
    }

    std::size_t find_sync() noexcept {
        while (read_offset_ + TS_PACKET_SIZE <= buffer_.size()) {
            auto sync =
                std::find(buffer_.begin() + read_offset_, buffer_.end(), 0x47);
            if (sync == buffer_.end()) {
                buffer_.clear();
                read_offset_ = 0;
                return std::string::npos;
            }
            const auto n = std::distance(sync, buffer_.end());
            if (n < TS_PACKET_SIZE) {
                buffer_.erase(buffer_.begin(), sync);
                read_offset_ = 0;
                return std::string::npos;
            } else if (n > TS_PACKET_SIZE) {
                if (*(sync + TS_PACKET_SIZE) == 0x47) {
                    read_offset_ = std::distance(buffer_.begin(), sync);
                    return read_offset_;
                }
                read_offset_ =
                    std::distance(buffer_.begin(), sync) + TS_PACKET_SIZE + 1;
                continue;
            }
            read_offset_ = std::distance(buffer_.begin(), sync);
            return read_offset_;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + read_offset_);
        read_offset_ = 0;
        return std::string::npos;
    }

    void handle_ts_packet(const uint8_t *packet) {
        const auto *header = reinterpret_cast<const TSHeader *>(packet);
        assert(packet[0] == 0x47);
        if (header->error())
            return;

        uint16_t pid = header->pid();
        bool start = header->payload_unit_start_indicator();
        uint8_t cc = header->continuity_counter();

        auto si = streams_.find(pid);
        if (si != streams_.end()) {
            auto &st = si->second;
            if (!st.first_cc) {
                if (cc == st.continuity_counter)
                    return;
                uint8_t expected = st.continuity_counter + 1;
                if (expected > 0x0F)
                    expected = 0;
                if (cc != expected) {
                    ICE_IN_DEBUG {
                        std::cerr << "[mpegts] CC mismatch pid=" << pid
                                  << " exp=" << (int)expected
                                  << " got=" << (int)cc << '\n';
                    }
                }
            }
            st.continuity_counter = cc;
            st.first_cc = false;
        }

        std::size_t offset = sizeof(TSHeader);
        auto afc = header->adaptation_field_control();
        if (afc == 0x02 || afc == 0x03) {
            if (offset >= TS_PACKET_SIZE)
                return;
            uint8_t af_len = packet[offset];
            offset += 1 + af_len;
        }
        if (offset >= TS_PACKET_SIZE)
            return;
        if (afc == 0x02)
            return;

        const uint8_t *payload = packet + offset;
        std::size_t payload_len = TS_PACKET_SIZE - offset;

        if (pid == 0) {
            parse_psi(payload, payload_len, start, 0);
        } else if (is_pmt_pid(pid)) {
            parse_psi(payload, payload_len, start, pid);
        } else if (si != streams_.end()) {
            parse_pes(payload, payload_len, start, si->second);
        }
    }

    static uint32_t calculate_crc32(const uint8_t *data, size_t len) noexcept {
        uint32_t crc = 0xFFFFFFFF;
        while (len--) {
            crc ^= (uint32_t)(*data++) << 24;
            for (int i = 0; i < 8; ++i) {
                if (crc & 0x80000000)
                    crc = (crc << 1) ^ 0x04C11DB7;
                else
                    crc <<= 1;
            }
        }
        return crc;
    }

    void parse_psi(const uint8_t *data, std::size_t len, bool start,
                   uint16_t pid) {
        assert(len > 0);

        if (start) {
            uint8_t pointer_field = data[0];
            size_t table_start = 1 + pointer_field;

            if (table_start < len) {
                auto &buf = streams_[pid].psi_section;
                if (buf.size() + len - table_start > MAX_PSI_SECTION) {
                    streams_.erase(pid);
                    return;
                }
                buf.assign(data + table_start, data + len);
                process_psi_sections(buf, pid);
            } else if (table_start == len) {
                streams_.erase(pid);
            } else {
                ICE_IN_DEBUG {
                    std::cerr
                        << "[mpegts] Invalid pointer_field for pid=" << pid
                        << "\n";
                }
                streams_.erase(pid);
            }
        } else {
            auto it = streams_.find(pid);
            if (it != streams_.end() && !it->second.psi_section.empty()) {
                auto &buf = it->second.psi_section;
                if (buf.size() + len > MAX_PSI_SECTION) {
                    streams_.erase(it);
                    return;
                }
                buf.insert(buf.end(), data, data + len);
                process_psi_sections(buf, pid);
            } else {
                ICE_IN_DEBUG {
                    std::cerr << "[mpegts] Non-start packet with empty buffer"
                                 " for pid="
                              << pid << "\n";
                }
                if (it != streams_.end())
                    streams_.erase(it);
                return;
            }
        }
    }

    void process_psi_sections(std::vector<uint8_t> &buf, uint16_t pid) {
        while (true) {
            if (buf.size() < 3)
                return;

            if (buf[0] == 0xFF) {
                streams_.erase(pid);
                return;
            }

            uint16_t section_len = ((buf[1] & 0x0F) << 8) | buf[2];
            size_t total_section_size = 3 + section_len;

            if (section_len > 1021) {
                ICE_IN_DEBUG {
                    std::cerr << "[mpegts] Invalid section length "
                              << section_len << " for pid " << pid
                              << ", clearing buffer.\n";
                }
                streams_.erase(pid);
                return;
            }

            if (buf.size() < total_section_size)
                return;

            handle_psi_section(buf.data(), total_section_size, pid);
            buf.erase(buf.begin(), buf.begin() + total_section_size);
        }
    }

    void handle_psi_section(const uint8_t *data, std::size_t len,
                            uint16_t pid) {
        if (len < 4)
            return;

        uint32_t crc_calculated = calculate_crc32(data, len - 4);
        uint32_t crc_received = (data[len - 4] << 24) | (data[len - 3] << 16) |
                                (data[len - 2] << 8) | data[len - 1];

        if (crc_calculated != crc_received) {
            ICE_IN_DEBUG {
                std::cerr << "[mpegts] CRC mismatch for pid " << pid << "\n";
            }
            return;
        }

        uint8_t table_id = data[0];

        if (table_id == 0x00) {
            parse_pat(data, len);
        } else if (table_id == 0x02) {
            parse_pmt(data, len);
        }
    }

    void parse_pat(const uint8_t *data, std::size_t len) {
        assert(len >= 4);
        std::size_t loop_end = len - 4;

        program_count_ = 0;
        for (std::size_t i = 8; i + 4 <= loop_end; i += 4) {
            uint16_t prog = (data[i] << 8) | data[i + 1];
            uint16_t pid = ((data[i + 2] & 0x1F) << 8) | data[i + 3];
            if (prog != 0 && program_count_ < MAX_PROGRAMS) {
                programs_[program_count_].number = prog;
                programs_[program_count_].pmt_pid = pid;
                program_count_++;
            }
        }
    }

    void parse_pmt(const uint8_t *data, std::size_t len) {
        if (len < 12)
            return;

        uint16_t pi_len = ((data[10] & 0x0F) << 8) | data[11];

        std::size_t es_off = 12 + pi_len;
        std::size_t loop_end = len - 4;

        if (es_off > loop_end)
            return;

        while (es_off + 5 <= loop_end) {
            uint8_t stream_type = data[es_off];
            uint16_t es_pid =
                ((data[es_off + 1] & 0x1F) << 8) | data[es_off + 2];
            uint16_t info_len =
                ((data[es_off + 3] & 0x0F) << 8) | data[es_off + 4];

            media_format fmt = media_format::unknown;
            media_kind k = media_kind::video;
            switch (stream_type) {
            case 0x01:
            case 0x02:
                k = media_kind::video;
                break;
            case 0x03:
            case 0x04:
            case 0x0F:
            case 0x1C:
            case 0x81:
            case 0x87:
                k = media_kind::audio;
                break;
            case 0x1B:
                k = media_kind::video;
                fmt = media_format::h264;
                break;
            case 0x24:
                k = media_kind::video;
                fmt = media_format::h264;
                break;
            default:
                break;
            }

            for (std::size_t d = 0; d + 2 <= info_len;) {
                uint8_t tag = data[es_off + 5 + d];
                uint8_t dlen = data[es_off + 5 + d + 1];
                if (d + 2 + dlen > info_len)
                    break;
                switch (tag) {
                case 0x02:
                    k = media_kind::video;
                    break;
                case 0x03:
                    k = media_kind::audio;
                    break;
                case 0x05:
                    if (dlen >= 4) {
                        const uint8_t *fid = data + es_off + 5 + d + 2;
                        if (std::memcmp(fid, "Opus", 4) == 0)
                            fmt = media_format::opus, k = media_kind::audio;
                        else if (std::memcmp(fid, "VP80", 4) == 0)
                            fmt = media_format::vp8, k = media_kind::video;
                        else if (std::memcmp(fid, "VP90", 4) == 0)
                            fmt = media_format::vp9, k = media_kind::video;
                        else if (std::memcmp(fid, "AVC1", 4) == 0 ||
                                 std::memcmp(fid, "H264", 4) == 0)
                            fmt = media_format::h264, k = media_kind::video;
                    }
                    break;
                case 0x28:
                    k = media_kind::video;
                    break;
                case 0x6A:
                case 0x7A:
                    k = media_kind::audio;
                    break;
                }
                d += 2 + dlen;
            }

            auto &st = streams_[es_pid];
            st.format = fmt;
            st.kind = k;

            es_off += 5 + info_len;
        }
    }

    void parse_pes(const uint8_t *data, std::size_t len, bool start,
                   stream_state &st) {
        auto &pes = st.pes_buffer;

        if (!start) {
            if (!pes.empty()) {
                if (pes.size() + len > MAX_PES_BUFFER) {
                    pes.clear();
                    return;
                }
                pes.insert(pes.end(), data, data + len);
            }
            return;
        }

        std::size_t off = 0;
        if (len >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)
            off = 0;
        else if (len >= 4 && data[1] == 0x00 && data[2] == 0x00 &&
                 data[3] == 0x01)
            off = 1;

        if (len - off < 9)
            return;

        const uint8_t *pes_hdr = data + off;
        uint8_t stream_id = pes_hdr[3];

        if ((stream_id >= 0xC0 && stream_id <= 0xDF) || stream_id == 0xBD ||
            stream_id == 0xBF)
            st.kind = media_kind::audio;
        else if (stream_id >= 0xE0 && stream_id <= 0xEF)
            st.kind = media_kind::video;

        if (!pes.empty()) {
            media_frame f;
            f.kind = st.kind;
            f.format = st.format;
            if (st.has_pts)
                f.timestamp = st.current_pts;
            f.data = std::move(pes);
            push_frame(std::move(f));
        }

        uint16_t flags = (pes_hdr[6] << 8) | pes_hdr[7];
        uint8_t hdr_len = pes_hdr[8];

        bool got_pts = false;
        if ((flags & 0xC0) != 0 && hdr_len >= 5 && len - off >= 9 + 5) {
            const uint8_t *pts_data = pes_hdr + 9;

            uint8_t pts_dts = (flags >> 6) & 0x03;

            if (pts_dts >= 2 && (pts_data[0] & 0xF0) == 0x20 &&
                (pts_data[2] & 0x01) == 0x01 && (pts_data[4] & 0x01) == 0x01) {
                uint64_t pts = ((uint64_t)(pts_data[0] & 0x0E) << 29) |
                               ((uint64_t)pts_data[1] << 22) |
                               ((uint64_t)(pts_data[2] & 0xFE) << 14) |
                               ((uint64_t)pts_data[3] << 7) |
                               ((uint64_t)(pts_data[4] & 0xFE) >> 1);
                st.current_pts = static_cast<uint32_t>(pts);
                st.has_pts = true;
                got_pts = true;
            }
        }
        if (!got_pts)
            st.has_pts = false;

        std::size_t es_start = off + 9 + hdr_len;
        if (es_start < len) {
            if (pes.size() + len - es_start > MAX_PES_BUFFER) {
                pes.clear();
                return;
            }
            pes.insert(pes.end(), data + es_start, data + len);
        }
    }
};

} // namespace detail
} // namespace asiortc

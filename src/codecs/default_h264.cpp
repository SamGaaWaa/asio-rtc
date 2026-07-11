#include "codecs/default_h264.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace asiortc::codecs {

static constexpr int kPacketMax = 1200;

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
DefaultH264Encoder::encode(const media_frame &, bool) {
    throw std::runtime_error{
        "H264 encode not supported: use pre-encoded data via pack()"};
}

static const uint8_t *find_start_code(const uint8_t *data, size_t len,
                                      size_t &code_len) {
    for (size_t i = 0; i + 2 < len; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                code_len = 3;
                return data + i;
            }
            if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
                code_len = 4;
                return data + i;
            }
        }
    }
    return nullptr;
}

std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
DefaultH264Encoder::pack(const std::vector<uint8_t> &data, uint32_t timestamp) {
    std::vector<std::vector<uint8_t>> payloads;
    size_t off = 0;

    while (off < data.size()) {
        size_t code1_len = 0;
        const uint8_t *code1 =
            find_start_code(data.data() + off, data.size() - off, code1_len);
        if (!code1)
            break;

        size_t nal_begin = off + code1_len;

        size_t code2_len = 0;
        const uint8_t *code2 = find_start_code(
            data.data() + nal_begin, data.size() - nal_begin, code2_len);

        size_t nal_end;
        if (code2) {
            nal_end = code2 - data.data();
            off = nal_end;
        } else {
            nal_end = data.size();
            off = data.size();
        }

        size_t nal_size = nal_end - nal_begin;
        if (nal_size == 0)
            continue;

        if (nal_size <= static_cast<size_t>(kPacketMax)) {
            payloads.push_back(std::vector<uint8_t>(data.begin() + nal_begin,
                                                    data.begin() + nal_end));
        } else {
            uint8_t nal_hdr = data[nal_begin];
            uint8_t fu_indicator = (nal_hdr & 0x60) | 28;
            uint8_t nal_type = nal_hdr & 0x1F;

            size_t frag_off = nal_begin + 1;
            while (frag_off < nal_end) {
                size_t frag_size = std::min(
                    nal_end - frag_off, static_cast<size_t>(kPacketMax - 2));
                bool first = (frag_off == nal_begin + 1);
                bool last = (frag_off + frag_size >= nal_end);
                uint8_t fu_header = nal_type;
                if (first)
                    fu_header |= 0x80;
                if (last)
                    fu_header |= 0x40;

                std::vector<uint8_t> p;
                p.reserve(2 + frag_size);
                p.push_back(fu_indicator);
                p.push_back(fu_header);
                p.insert(p.end(), data.begin() + frag_off,
                         data.begin() + frag_off + frag_size);
                payloads.push_back(std::move(p));
                frag_off += frag_size;
            }
        }
    }

    return {payloads, timestamp};
}

} // namespace asiortc::codecs

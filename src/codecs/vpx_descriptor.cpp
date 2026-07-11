#include "vpx_descriptor.hpp"

#include <cstdint>
#include <vector>

namespace asiortc::codecs {

vpx_payload_descriptor vpx_payload_descriptor::parse(const uint8_t *data,
                                                     size_t len,
                                                     size_t &consumed) {
    vpx_payload_descriptor d;
    consumed = 0;
    if (len < 1)
        return d;

    uint8_t b0 = data[0];
    bool extended = (b0 >> 7) & 1;
    d.partition_start = (b0 >> 4) & 1;
    d.partition_id = b0 & 0xF;
    size_t pos = 1;

    if (extended) {
        if (len < pos + 1)
            return d;
        uint8_t ext = data[pos++];
        if (ext & 0x80) {
            if (len < pos + 1)
                return d;
            if (data[pos] & 0x80) {
                if (len < pos + 2)
                    return d;
                d.picture_id = ((data[pos] & 0x7F) << 8) | data[pos + 1];
                pos += 2;
            } else {
                d.picture_id = data[pos];
                pos += 1;
            }
        }
        if (ext & 0x40) {
            if (len < pos + 1)
                return d;
            d.tl0picidx = data[pos++];
        }
        if (ext & 0x30) {
            if (len < pos + 1)
                return d;
            uint8_t tk = data[pos++];
            if (ext & 0x20)
                d.tid = {(tk >> 6) & 3, (tk >> 5) & 1};
            if (ext & 0x10)
                d.keyidx = tk & 0x1F;
        }
    }

    consumed = pos;
    return d;
}

std::vector<uint8_t> vpx_payload_descriptor::bytes() const {
    std::vector<uint8_t> data;

    uint8_t octet = (partition_start << 4) | (partition_id & 0xF);

    bool has_ext = picture_id || tl0picidx || tid || keyidx;
    if (has_ext) {
        uint8_t ext = 0;
        if (picture_id)
            ext |= 0x80;
        if (tl0picidx)
            ext |= 0x40;
        if (tid)
            ext |= 0x20;
        if (keyidx)
            ext |= 0x10;

        data.push_back(0x80 | octet);
        data.push_back(ext);

        if (picture_id) {
            if (*picture_id < 128)
                data.push_back(static_cast<uint8_t>(*picture_id));
            else {
                uint16_t v = 0x8000 | *picture_id;
                data.push_back(static_cast<uint8_t>(v >> 8));
                data.push_back(static_cast<uint8_t>(v & 0xFF));
            }
        }
        if (tl0picidx)
            data.push_back(*tl0picidx);
        if (tid || keyidx) {
            uint8_t tk = 0;
            if (tid)
                tk |= (tid->first << 6) | (tid->second << 5);
            if (keyidx)
                tk |= *keyidx & 0x1F;
            data.push_back(tk);
        }
    } else {
        data.push_back(octet);
    }
    return data;
}

} // namespace asiortc::codecs

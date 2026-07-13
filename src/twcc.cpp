#include "twcc.hpp"

#include "asioice/detail/binary.hpp"
#include "rtcp.hpp"

#include <algorithm>
#include <cstring>

namespace asiortc {

static constexpr uint16_t max_tcc_count = 100;

void transport_cc::handle_incoming(std::span<const uint8_t> extension_data,
                                   transport_cc::send_callback cb) {
    size_t off = 0;
    while (off < extension_data.size()) {
        uint8_t hdr = extension_data[off];
        if (hdr == 0) {
            off++;
            continue;
        }
        uint8_t id = (hdr >> 4) & 0xF;
        uint8_t len = (hdr & 0xF) + 1;
        if (id == 4 && off + 1 + len <= extension_data.size()) {
            uint16_t seq =
                (static_cast<uint16_t>(extension_data[off + 1]) << 8) |
                static_cast<uint16_t>(extension_data[off + 2]);
            _entry e{seq, std::chrono::steady_clock::now()};
            auto it = std::lower_bound(
                _recv.begin(), _recv.end(), e, [](auto &a, auto &b) {
                    return static_cast<int16_t>(a.seq - b.seq) < 0;
                });
            if (it == _recv.end() || it->seq != e.seq)
                _recv.insert(it, e);
        }
        off += 1 + len;
    }
    while (_recv.size() > 200) {
        auto data = report();
        if (!data.empty() && cb)
            cb(std::move(data));
    }
}

std::vector<uint8_t> transport_cc::report() {
    if (_recv.empty())
        return {};
    if (_fb_count == 0)
        _next_report_seq = _recv[0].seq;

    const auto base_seq = _next_report_seq;
    const uint64_t base_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            _recv[0].ts.time_since_epoch())
            .count();
    const uint64_t ref_64ms = base_us / 64000 * 64000;

    auto last_ts = _recv[0].ts - std::chrono::microseconds{base_us - ref_64ms};

    std::vector<rtcp::tcc_packet_info> packets;
    auto it = _recv.begin();
    [&, this] {
        for (; it != _recv.end(); ++it) {
            if (packets.size() >= max_tcc_count)
                return;
            while (_next_report_seq != it->seq) {
                if (packets.size() >= max_tcc_count)
                    return;
                packets.push_back({rtcp::tcc_packet_status::not_received, 0});
                ++_next_report_seq;
            }
            const auto dur =
                std::chrono::duration_cast<std::chrono::microseconds>(it->ts -
                                                                      last_ts);
            const auto delta = dur.count() / 250;
            const auto status =
                (delta < -128 || delta > 127)
                    ? (delta < -32768 || delta > 32767
                           ? rtcp::tcc_packet_status::received_without_delta
                           : rtcp::tcc_packet_status::large_delta)
                    : rtcp::tcc_packet_status::small_delta;
            packets.push_back({status, static_cast<int16_t>(delta)});
            ++_next_report_seq;
            last_ts = it->ts;
        }
    }();
    _recv.erase(_recv.begin(), it);

    auto chunks = rtcp::tcc_build_packet_status(packets);

    rtcp::transport_cc_feedback fb;
    fb.sender_ssrc = _sender_ssrc;
    fb.media_ssrc = _media_ssrc;
    fb.base_seq = base_seq;
    fb.status_count = static_cast<uint16_t>(packets.size());
    fb.reference_time = ((uint32_t)(ref_64ms / 64000)) & 0xffffff;
    fb.feedback_packet_count = ++_fb_count;
    fb.packet_chunks = std::move(chunks);

    return rtcp::build_transport_cc(fb);
}

} // namespace asiortc

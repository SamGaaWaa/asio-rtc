#include "media_track_impl.hpp"

#include "asioice/detail/asio2exec.hpp"
#include "asioice/detail/binary.hpp"
#include "asioice/detail/string_utils.hpp"

#include <chrono>
#include <optional>

namespace asiortc {

media_kind media_track_impl::kind() const noexcept {
    return asioice::utils::nceq("opus", _codec.name) ? media_kind::audio
                                                     : media_kind::video;
}

media_description media_track_impl::description() const noexcept {
    media_description desc;
    if (asioice::utils::nceq(_codec.name, "H264"))
        desc.format = media_format::h264;
    else if (asioice::utils::nceq(_codec.name, "VP8"))
        desc.format = media_format::vp8;
    else if (asioice::utils::nceq(_codec.name, "VP9"))
        desc.format = media_format::vp9;
    else if (asioice::utils::nceq(_codec.name, "opus"))
        desc.format = media_format::opus;
    desc.clock_rate = _codec.clock_rate;
    desc.encoding_params = _codec.params_string();
    desc.channels = _codec.channels;
    return desc;
}

void media_track_impl::stop() noexcept { _state = track_state::ended; }

void media_track_impl::push_frame(rtp::rtp_packet pkt) {
    _jitter.push(std::move(pkt));
    _on_frame.set_one_value();
}

asiortc::task<std::vector<rtp::rtp_packet>> media_track_impl::recv_packet() {
    auto lk = co_await _mtx.lock();
    while (true) {
        auto pkts = _jitter.pop_frame();
        if (!pkts.empty())
            co_return pkts;
        co_await _on_frame.get_future();
    }
    std::unreachable();
}

namespace {

void h264_append_start_code(std::vector<uint8_t> &out) {
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
}

// Reassembles the H.264 RTP payloads of a single frame into an Annex-B byte
// stream (start-code-delimited NAL units).  Each input packet carries exactly
// one unit, so packet boundaries make the parsing unambiguous (RFC 6184
// packetization-mode 1):
//   - type 1..23 : single NAL unit
//   - type 24    : STAP-A (aggregation packet)
//   - type 28    : FU-A (fragmentation unit)
std::vector<uint8_t>
h264_packets_to_annex_b(const std::vector<rtp::rtp_packet> &packets) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> partial; // accumulated FU-A NAL (header + data)
    bool in_fu = false;

    for (const auto &pkt : packets) {
        const auto &in = pkt.payload;
        if (in.empty())
            continue;
        const uint8_t nal_type = in[0] & 0x1F;

        if (nal_type == 24) { // STAP-A: [0x18][2-byte size][NAL]...
            size_t off = 1;
            while (off + 2 <= in.size()) {
                const uint16_t size =
                    static_cast<uint16_t>((in[off] << 8) | in[off + 1]);
                off += 2;
                if (size == 0 || off + size > in.size())
                    break;
                h264_append_start_code(out);
                out.insert(out.end(), in.begin() + off,
                           in.begin() + off + size);
                off += size;
            }
            continue;
        }

        if (nal_type == 28) { // FU-A: [FU-indicator][FU-header][frag]
            if (in.size() < 2)
                continue;
            const uint8_t fu_indicator = in[0];
            const uint8_t fu_header = in[1];

            if (fu_header & 0x80) { // start bit
                partial.clear();
                partial.push_back((fu_indicator & 0xE0) | (fu_header & 0x1F));
                partial.insert(partial.end(), in.begin() + 2, in.end());
                in_fu = true;
            } else if (in_fu) {
                partial.insert(partial.end(), in.begin() + 2, in.end());
            }

            if (fu_header & 0x40) { // end bit
                h264_append_start_code(out);
                out.insert(out.end(), partial.begin(), partial.end());
                partial.clear();
                in_fu = false;
            }
            continue;
        }

        if (nal_type >= 1 && nal_type <= 23) { // single NAL unit
            h264_append_start_code(out);
            out.insert(out.end(), in.begin(), in.end());
            continue;
        }

        // Unsupported NAL type (STAP-B/MTAP16/MTAP24/FU-B); skip.
    }

    return out;
}

} // namespace

asiortc::task<std::vector<media_frame>>
media_track_impl::recv(std::span<const encode_target> layers) {
    auto pkts = co_await recv_packet();
    if (pkts.empty())
        co_return std::vector<media_frame>{};
    media_frame mf;
    mf.kind = this->kind();
    // TODO
    mf.format = [this] {
        if (asioice::utils::nceq(_codec.name, "H264"))
            return media_format::h264;
        else if (asioice::utils::nceq(_codec.name, "VP8"))
            return media_format::vp8;
        else if (asioice::utils::nceq(_codec.name, "VP9"))
            return media_format::vp9;
        else if (asioice::utils::nceq(_codec.name, "opus"))
            return media_format::opus;
        return media_format::unknown;
    }();
    mf.timestamp = pkts.front().timestamp;
    if (mf.format == media_format::h264) {
        mf.data = h264_packets_to_annex_b(pkts);
    } else {
        for (const auto &p : pkts)
            mf.data.insert(mf.data.end(), p.payload.begin(), p.payload.end());
    }
    mf.info = rtp_frame_info{
        .rtp_timestamp = pkts.front().timestamp,
        .ssrc = pkts.front().ssrc,
        .clock_rate = _codec.clock_rate,
        .first_sequence_number = pkts.front().sequence_number,
        .marker = static_cast<bool>(pkts.back().marker),
        // TODO
        .receive_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())};
    std::vector<media_frame> result;
    result.push_back(std::move(mf));
    co_return result;
}

} // namespace asiortc

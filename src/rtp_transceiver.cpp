#include "rtp_transceiver.hpp"

#include <algorithm>
#include <unordered_set>

#include "connection_impl.hpp"

namespace asiortc {

static sdp_direction negotiate_direction(sdp_direction local,
                                         sdp_direction remote) noexcept {
    using enum sdp_direction;
    if (local == inactive || remote == inactive)
        return inactive;
    if (local == sendrecv)
        return remote;
    if (local == sendonly)
        return (remote == recvonly || remote == sendrecv) ? sendonly : inactive;
    if (local == recvonly)
        return (remote == sendonly || remote == sendrecv) ? recvonly : inactive;
    return inactive;
}

std::vector<sdp_codec> default_video_codecs() {
    return {
        {96, "VP8", 90000, ""},
        {97, "rtx", 90000, "apt=96"},
    };
}

std::vector<sdp_codec> default_audio_codecs() {
    return {
        {111, "opus", 48000, "2"},
        {63, "telephone-event", 8000, ""},
    };
}

static std::string infer_media_type(const std::vector<sdp_codec> &codecs) {
    if (codecs.empty())
        return "application";
    const auto &name = codecs[0].name;
    if (name.starts_with("VP") || name.starts_with("H26") ||
        name.starts_with("AV1") || name == "red" || name == "ulpfec" ||
        name == "rtx" || name == "flexfec")
        return "video";
    if (name == "opus" || name == "PCMU" || name == "PCMA" ||
        name == "telephone-event" || name.starts_with("G7") || name == "CN" ||
        name == "L16" || name == "L24")
        return "audio";
    return "application";
}

void rtp_transceiver::wire_back_references() {
    auto self = weak_from_this();
    _sender->_transceiver = self;
    _receiver->_transceiver = std::move(self);
}

void rtp_transceiver::stop() {
    if (_stopped)
        return;
    _stopped = true;
    _sender->_stopped = true;
    _receiver->_stopped = true;
}

void rtp_transceiver::set_codecs(std::vector<sdp_codec> codecs) {
    _codecs = std::move(codecs);
}

sdp_media rtp_transceiver::to_offer_sdp_media() const {
    sdp_media m;
    m.mid = _mid;
    m.media_type = infer_media_type(_codecs);
    m.port = 9;
    m.proto = "UDP/TLS/RTP/SAVPF";
    m.conn_nettype = "IN";
    m.conn_addrtype = "IP4";
    m.conn_addr = "0.0.0.0";
    m.direction = _direction;
    m.rtcp_mux = true;
    m.msids = _sender->_msids;

    for (const auto &c : _codecs) {
        m.payload_types.push_back(c.payload_type);
        m.rtpmaps.push_back(c);
        if (!c.encoding_params.empty())
            m.fmtps.push_back(std::to_string(c.payload_type) +
                              " " + c.encoding_params);
    }

    // Default RTP header extensions per media kind
    std::string media = infer_media_type(_codecs);
    if (media == "video")
        m.extmaps = {
            {1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
            {3,
             "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}};
    else if (media == "audio")
        m.extmaps = {{1, "urn:ietf:params:rtp-hdrext:sdes:mid"}};

    return m;
}

sdp_media rtp_transceiver::to_answer_sdp_media(const sdp_media &remote) const {
    sdp_media m;
    m.mid = _mid;
    m.media_type = remote.media_type;
    m.proto = remote.proto;
    m.conn_nettype = "IN";
    m.conn_addrtype = "IP4";
    m.conn_addr = "0.0.0.0";
    m.rtcp_mux = remote.rtcp_mux;
    m.msids = remote.msids;

    if (_stopped) {
        m.port = 0;
        m.direction = sdp_direction::inactive;
        return m;
    }

    m.direction = negotiate_direction(_direction, remote.direction);

    bool accepted = false;
    std::vector<bool> used(remote.rtpmaps.size(), false);
    for (const auto &local_codec : _codecs) {
        for (std::size_t j = 0; j < remote.rtpmaps.size(); ++j) {
            if (used[j])
                continue;
            const auto &remote_codec = remote.rtpmaps[j];
            if (local_codec.name == remote_codec.name &&
                local_codec.clock_rate == remote_codec.clock_rate &&
                local_codec.encoding_params ==
                    remote_codec.encoding_params) {
                m.payload_types.push_back(remote_codec.payload_type);
                m.rtpmaps.push_back(remote_codec);
                used[j] = true;
                accepted = true;
                break;
            }
        }
    }

    m.port = accepted ? 9 : 0;

    if (accepted) {
        for (const auto &f : remote.fmtps) {
            std::string_view sv = f;
            auto space = sv.find(' ');
            if (space != std::string_view::npos) {
                auto pt = sv.substr(0, space);
                for (auto pt_val : m.payload_types) {
                    if (pt == std::to_string(pt_val)) {
                        m.fmtps.push_back(f);
                        break;
                    }
                }
            } else {
                m.fmtps.push_back(f);
            }
        }
        m.extmaps = remote.extmaps;
        m.msids = remote.msids;
    }

    return m;
}

void rtp_transceiver::from_remote_sdp(const sdp_media &remote) {
    if (!remote.mid.empty())
        _mid = remote.mid;

    _direction = negotiate_direction(_direction, remote.direction);

    if (_receiver) {
        _receiver->_parameters.header_extensions = remote.extmaps;
    }
}

} // namespace asiortc

#include "rtp_transceiver.hpp"

#include <algorithm>
#include <unordered_set>

#include "connection_impl.hpp"
#include "media_track_impl.hpp"

namespace asiortc {

std::vector<sdp_codec> default_video_codecs() {
    return {
        {96, "VP8", 90000, "", ""},
        {97, "rtx", 90000, "", "apt=96"},
    };
}

std::vector<sdp_codec> default_audio_codecs() {
    return {
        {111, "opus", 48000, "2", ""},
        {63, "telephone-event", 8000, "", ""},
        {0, "PCMU", 8000, "1", ""},
        {8, "PCMA", 8000, "1", ""},
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

rtp_sender::rtp_sender() { _streams.push_back(std::make_shared<rtp_stream>()); }

void rtp_transceiver::apply_simulcast_to(sdp_media &m) const {
    if (send_encodings.size() < 2)
        return;
    std::string send_rids;
    for (size_t i = 0; i < send_encodings.size(); ++i) {
        if (send_encodings[i].rid.empty())
            continue;
        std::string pt_str =
            std::to_string(_codecs.empty() ? 0 : _codecs[0].payload_type);
        m.rids.push_back(send_encodings[i].rid + " send pt=" + pt_str);
        if (!send_rids.empty())
            send_rids += ";";
        send_rids += send_encodings[i].rid;
    }
    if (!send_rids.empty())
        m.simulcast = "send " + send_rids;
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

sdp_media rtp_transceiver::to_offer_sdp_media(std::string mid) const {
    sdp_media m;
    m.mid = mid;
    m.media_type = infer_media_type(_codecs);
    m.port = 9;
    m.proto = "UDP/TLS/RTP/SAVPF";
    m.conn_nettype = "IN";
    m.conn_addrtype = "IP4";
    m.conn_addr = "0.0.0.0";
    m.direction = _direction;
    m.rtcp_mux = true;
    m.msids.clear();
    for (const auto &ms : _sender->_msids)
        m.msids.push_back(ms + " " + mid);

    for (const auto &c : _codecs) {
        m.payload_types.push_back(c.payload_type);
        m.rtpmaps.push_back(c);
        if (!c.fmtp_params.empty())
            m.fmtps.push_back(std::to_string(c.payload_type) + " " +
                              c.fmtp_params);
    }

    // Default RTP header extensions per media kind
    std::string media = infer_media_type(_codecs);
    if (media == "video") {
        m.extmaps = {
            {1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
            {2, "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"},
            {3, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"},
            {4, "http://www.ietf.org/id/"
                "draft-holmer-rmcat-transport-wide-cc-extensions-01"}};
        for (const auto &c : _codecs) {
            if (c.name != "rtx" && c.name != "red" && c.name != "ulpfec" &&
                c.name != "flexfec") {
                m.rtcp_fbs.push_back({c.payload_type, "nack", ""});
                m.rtcp_fbs.push_back({c.payload_type, "nack", "pli"});
                m.rtcp_fbs.push_back({c.payload_type, "goog-remb", ""});
            }
        }
        apply_simulcast_to(m);
    } else if (media == "audio") {
        m.extmaps = {{1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
                     {4, "http://www.ietf.org/id/"
                         "draft-holmer-rmcat-transport-wide-cc-extensions-01"}};
    }

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
                local_codec.encoding_params == remote_codec.encoding_params) {

                // H264: only accept baseline profile (42001f)
                if (local_codec.name == "H264") {
                    for (const auto &f : remote.fmtps) {
                        std::string_view sv = f;
                        auto space = sv.find(' ');
                        if (space == std::string_view::npos)
                            continue;
                        auto pt = sv.substr(0, space);
                        if (pt != std::to_string(remote_codec.payload_type))
                            continue;
                        auto params = sv.substr(space + 1);
                        auto pos = params.find("profile-level-id=");
                        if (pos != std::string_view::npos) {
                            auto plid = params.substr(pos + 17, 6);
                            if (plid != "42001f") {
                                used[j] = true;
                                continue;
                            }
                        }
                    }
                }

                // RTX: verify apt points to an accepted payload type
                if (local_codec.name == "rtx" &&
                    !remote_codec.encoding_params.empty()) {
                    auto ap = remote_codec.encoding_params.find("apt=");
                    if (ap != std::string::npos) {
                        int apt = std::stoi(std::string{
                            remote_codec.encoding_params.substr(ap + 4)});
                        bool found = false;
                        for (auto pt : m.payload_types)
                            if (pt == apt)
                                found = true;
                        if (!found) {
                            used[j] = true;
                            continue;
                        }
                    }
                }

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
                auto params = sv.substr(space + 1);
                for (auto pt_val : m.payload_types) {
                    if (pt == std::to_string(pt_val)) {
                        // RTX: verify apt points to an accepted PT
                        auto ap = params.find("apt=");
                        if (ap != std::string::npos) {
                            int apt =
                                std::stoi(std::string{params.substr(ap + 4)});
                            bool found = false;
                            for (auto p : m.payload_types)
                                if (p == apt)
                                    found = true;
                            if (!found)
                                break;
                        }
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

        // Only generate simulcast if remote offered recv RIDs and local
        // send_encodings match (RFC 8853 §5.3.2: answer MUST NOT add).
        std::string matched_rids;
        if (!remote.rids.empty() && !send_encodings.empty()) {
            for (const auto &rid_line : remote.rids) {
                auto ws = rid_line.find(' ');
                if (ws == std::string::npos)
                    continue;
                auto rid = rid_line.substr(0, ws);
                if (rid_line.find("recv ", ws) == std::string::npos)
                    continue; // only flip recv→send

                // Check if local send_encodings has this rid
                bool matched = false;
                for (const auto &enc : send_encodings)
                    if (enc.rid == rid) {
                        matched = true;
                        break;
                    }
                if (!matched)
                    continue;

                auto pt_str = _codecs.empty()
                                  ? "0"
                                  : std::to_string(_codecs[0].payload_type);
                m.rids.push_back(rid + " send pt=" + pt_str);
                if (!matched_rids.empty())
                    matched_rids += ";";
                matched_rids += rid;
            }
            if (!matched_rids.empty())
                m.simulcast = "send " + matched_rids;
        }

        // SSRC: one per matched simulcast RID, or just _streams[0]
        if (!matched_rids.empty() && _sender->_streams.size() > 1) {
            for (size_t i = 0;
                 i < m.rids.size() && i < _sender->_streams.size(); ++i) {
                auto ssrc = _sender->_streams[i]->ssrc;
                m.ssrcs.push_back(std::to_string(ssrc) + " cname:asiortc");
            }
        } else if (!_sender->_streams.empty()) {
            auto ssrc = _sender->_streams[0]->ssrc;
            m.ssrcs.push_back(std::to_string(ssrc) + " cname:asiortc");
            for (const auto &ms : _sender->_msids)
                m.ssrcs.push_back(std::to_string(ssrc) + " msid:" + ms + " " +
                                  _mid);
        }
    }

    return m;
}

void rtp_transceiver::from_remote_sdp(const sdp_media &remote) {
    _direction = negotiate_direction(_direction, remote.direction);
    if (_receiver) {
        _receiver->_parameters.header_extensions = remote.extmaps;
    }
}

rtp_transceiver::~rtp_transceiver() noexcept {
    if (_sender)
        _sender->stop();
    if (_receiver)
        _receiver->stop();
}

const std::shared_ptr<codecs::decoder> &rtp_receiver::decoder() const noexcept {
    if (_track) {
        auto *mt = static_cast<media_track_impl *>(_track.get());
        return mt->_decoder;
    }
    static const std::shared_ptr<codecs::decoder> empty;
    return empty;
}

} // namespace asiortc

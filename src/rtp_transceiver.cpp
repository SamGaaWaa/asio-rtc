#include "rtp_transceiver.hpp"

#include <algorithm>
#include <unordered_set>

#include "connection_impl.hpp"
#include "media_track_impl.hpp"

namespace asiortc {

static sdp_media_type get_media_type(media_kind k) {
    return k == media_kind::audio ? sdp_media_type::audio
                                  : sdp_media_type::video;
}

rtp_sender::rtp_sender() {
    // _streams.push_back(std::make_shared<rtp_stream>());
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

media_kind rtp_transceiver::kind() const noexcept {
    if (asioice::utils::nceq(_codec.name, "opus"))
        return media_kind::audio;
    return media_kind::video;
}

sdp_media rtp_transceiver::to_offer_sdp_media(std::string mid) const {
    sdp_media m;
    m.mid = std::move(mid);
    m.port = 9;
    m.proto = sdp_proto::UDP_TLS_RTP_SAVPF;
    m.conn_nettype = "IN";
    m.conn_addrtype = "IP4";
    m.conn_addr = "0.0.0.0";
    m.media_type = get_media_type(this->kind());
    m.rtcp_mux = true;
    m.add_rtpmap(this->rtpmap());

    const auto &codec = _codec;
    if (stopped()) {
        m.port = 0;
        m.direction = sdp_direction::inactive;
        return m;
    }

    auto pt = codec.payload_type;

    m.direction = _direction;
    if (_sender->track()) {
        for (const auto &ms : _sender->_msids)
            m.msids.push_back({std::string(ms), _sender->track()->id()});
    }

    if (this->kind() == media_kind::video) {
        uint8_t rtx_pt = static_cast<uint8_t>(pt + 1);
        sdp_rtpmap rtx_rtpmap{rtx_pt, "rtx", codec.clock_rate};
        rtx_rtpmap.add_param("apt", std::to_string((int)pt));
        m.add_rtpmap(std::move(rtx_rtpmap));
        m.extmaps = {
            {1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
            {2, "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"},
            {3, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"},
            {4, "http://www.ietf.org/id/"
                "draft-holmer-rmcat-transport-wide-cc-extensions-01"}};
        m.add_feedback({pt, "nack"});
        m.add_feedback({pt, "nack", "pli"});
        m.add_feedback({pt, "goog-remb"});
    } else {
        m.extmaps = {{1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
                     {4, "http://www.ietf.org/id/"
                         "draft-holmer-rmcat-transport-wide-cc-extensions-01"}};
    }

    if (_direction == sdp_direction::sendrecv ||
        _direction == sdp_direction::sendonly) {
        std::vector<sdp_rid> rids;
        for (const auto &p : _sender->parameters().encodings) {
            if (!p.rid.empty())
                rids.push_back({p.rid});
        }
        m.rids = std::move(rids);
    }

    return m;
}

std::vector<sdp_rtpmap>
rtp_transceiver::match_offer_rtpmaps(const sdp_media &offer,
                                     sdp_media &answer) const {
    const auto &local_c = this->rtpmap();
    auto it = std::ranges::find_if(offer.rtpmaps(), [&local_c](const auto &c) {
        return sdp_rtpmap::is_match(local_c, c);
    });
    if (it == offer.rtpmaps().end()) {
        return {};
    }
    return {*it};
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
    m.direction = negotiate_direction(_direction, remote.direction);

    {
        auto rtpmaps = match_offer_rtpmaps(remote, m);
        for (auto &rtpmap : rtpmaps)
            m.add_rtpmap(std::move(rtpmap));
        if (m.rtpmaps().empty() && !remote.rtpmaps().empty())
            m.add_rtpmap(remote.rtpmaps().front());
    }

    if (_stopped || m.direction == sdp_direction::inactive) {
        m.port = 0;
        m.direction = sdp_direction::inactive;
        return m;
    }

    if (!_sender->_streams.empty()) {
        auto ssrc = _sender->_streams[0]->ssrc;
        m.ssrcs.emplace_back(ssrc, "cname", "asiortc");
        std::string tid = _sender->track() ? _sender->track()->id() : _mid;
        for (const auto &ms : _sender->_msids)
            m.ssrcs.emplace_back(ssrc, "msid", std::format("{} {}", ms, tid));
    }

    m.port = 9;
    m.extmaps = remote.extmaps;
    m.msids = remote.msids;

    return m;
}

void rtp_transceiver::from_remote_offer(const sdp_media &remote) {
    _direction = negotiate_direction(_direction, remote.direction);
    if (_direction == sdp_direction::inactive) {
        return;
    }
    auto c_it = std::ranges::find_if(remote.rtpmaps(), [this](const auto &c) {
        return sdp_rtpmap::is_match(c, this->_codec);
    });
    if (c_it == remote.rtpmaps().end()) {
        _direction = sdp_direction::inactive;
        return;
    }
    if (_direction == sdp_direction::recvonly ||
        _direction == sdp_direction::sendrecv) {
        if (!_receiver->_track)
            _receiver->_track = std::make_shared<media_track_impl>(*c_it);
        else
            _receiver->_track->set_rtpmap(*c_it);
        _receiver->_parameters.header_extensions = remote.extmaps;
    }
    this->_codec.payload_type = c_it->payload_type;
}

void rtp_transceiver::from_remote_answer(const sdp_media &remote) {
    _direction = negotiate_direction(_direction, remote.direction);
    if (_direction == sdp_direction::inactive) {
        return;
    }
    auto c_it = std::ranges::find_if(remote.rtpmaps(), [this](const auto &c) {
        return sdp_rtpmap::is_match(c, this->_codec);
    });
    if (c_it == remote.rtpmaps().end()) {
        _direction = sdp_direction::inactive;
        return;
    }
    if (_direction == sdp_direction::recvonly ||
        _direction == sdp_direction::sendrecv) {
        if (!_receiver->_track)
            _receiver->_track =
                std::make_shared<media_track_impl>(this->_codec);
    }
}

rtp_transceiver::~rtp_transceiver() noexcept {
    if (_sender)
        _sender->stop();
    if (_receiver)
        _receiver->stop();
}

ssrc_context &rtp_receiver::create_ssrc_context(uint32_t ssrc,
                                                ssrc_context_set &ssrc_set) {
    auto it = ssrc_set.lower_bound(ssrc);
    if (it != ssrc_set.end() && it->ssrc() == ssrc)
        return *it;
    for (auto &ctx : _ssrcs) {
        if (ctx.ssrc() == ssrc) {
            ctx.reset_stats();
            ssrc_set.insert(it, ctx);
            return ctx;
        }
    }
    _ssrcs.emplace_back(*this, ssrc);
    ssrc_set.insert(it, _ssrcs.back());
    return _ssrcs.back();
}

} // namespace asiortc

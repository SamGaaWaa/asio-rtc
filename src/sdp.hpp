#pragma once

#include "asiortc/session_description.hpp"
#include "asiortc/media_track.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <optional>
#include <map>
#include <span>

namespace asiortc {

enum class sdp_media_type : char { audio, video, application, text, message };

enum class sdp_proto : char {
    UDP,
    RTP_AVP,
    RTP_SAVP,
    RTP_SAVPF,
    UDP_TLS_RTP_SAVPF,
    DTLS_SCTP,
    UDP_DTLS_SCTP,
    TCP_DTLS_SCTP
};

enum class sdp_setup_role : char { active, passive, actpass, holdconn };

enum class sdp_rid_direction : char { send, recv };

struct sdp_rtcp_fb {
    std::optional<uint8_t> payload_type; // Payload type number or "*" for all
    std::string type;
    std::string subtype;
};

struct sdp_msid {
    std::string stream_id;
    std::string track_id;
};

struct sdp_msid_semantic {
    std::string semantic;
    std::vector<std::string> stream_ids;
};

struct sdp_rtpmap {
    sdp_rtpmap() = default;
    sdp_rtpmap(uint8_t pt, std::string enc_name, uint32_t ck,
               std::optional<std::uint8_t> ch = {})
        : payload_type{pt}, name{std::move(enc_name)}, clock_rate{ck},
          channels{ch} {}

    uint8_t payload_type = 0;
    std::string name;
    uint32_t clock_rate = 0;
    std::optional<std::uint8_t> channels;

    std::string_view params_string() const noexcept { return _params; }
    void set_params_string(std::string params) noexcept {
        _params = std::move(params);
    }
    void add_param(std::string_view name, std::string_view value);
    void remove_param(std::string_view name) noexcept;
    std::optional<std::string_view>
    find_param(std::string_view name) const noexcept;

    static sdp_rtpmap from_media_description(const media_description &desc);
    static bool is_match(const sdp_rtpmap &a, const sdp_rtpmap &b) noexcept;

  private:
    std::string _params{};
};

struct sdp_ssrc {
    uint32_t ssrc = 0;
    std::string attribute;
    std::string value;
};

struct sdp_ssrc_group {
    std::string semantics;
    std::vector<uint32_t> ssrcs;
};

struct sdp_fingerprint {
    std::string algorithm; // Hash algorithm
    std::string value;     // Hex-encoded fingerprint with colons
};

struct sdp_rtcp {
    std::string nettype;
    std::string addrtype;
    std::string addr;
    uint16_t port = 0;
};

struct sdp_crypto {
    std::uint32_t tag;
    std::string suite;
    std::string key_params;
    std::string session_params;
};

struct sdp_simulcast_stream {
    std::string rid;
    bool paused = false;
};

struct sdp_simulcast_layer {
    std::vector<sdp_simulcast_stream> streams;
};

struct sdp_simulcast {
    std::vector<sdp_simulcast_layer> send_layers;
    std::vector<sdp_simulcast_layer> recv_layers;
};

struct sdp_rid {
    std::string id;
    sdp_rid_direction direction = sdp_rid_direction::send;
    std::vector<uint8_t> payload_types;
    std::map<std::string, std::string> params;
};

struct sdp_media {
    sdp_media_type media_type;
    uint16_t port = 9;
    sdp_proto proto = sdp_proto::RTP_AVP;

    std::string mid;
    sdp_direction direction = sdp_direction::sendrecv;
    // std::vector<sdp_fmtp> fmtps;
    std::vector<sdp_extmap> extmaps;
    std::vector<sdp_ssrc> ssrcs;
    std::vector<sdp_ssrc_group> ssrc_groups;
    std::vector<std::string> candidates;
    std::vector<sdp_fingerprint> fingerprints;
    std::optional<sdp_setup_role> setup;
    std::string ice_ufrag;
    std::string ice_pwd;
    std::vector<std::string> ice_options;

    bool rtcp_mux = false;
    bool rtcp_mux_only = false;
    bool rtcp_rsize = false;

    std::optional<sdp_rtcp> rtcp;
    std::vector<sdp_msid> msids;
    std::vector<sdp_crypto> cryptos;

    std::optional<uint16_t> sctp_port;
    std::optional<std::uint32_t>
        max_message_size; ///< a=max-message-size (RFC 8841)

    sdp_simulcast simulcast;
    std::vector<sdp_rid> rids;

    std::optional<std::uint32_t> ptime;    ///< a=ptime (ms)
    std::optional<std::uint32_t> maxptime; ///< a=maxptime (ms)
    std::optional<double> framerate;       ///< a=framerate

    bool end_of_candidates = false; ///< a=end-of-candidates
    bool extmap_allow_mixed = false;

    std::string conn_nettype;
    std::string conn_addrtype;
    std::string conn_addr;

    std::vector<std::pair<std::string, std::string>> attributes;

    std::span<const uint8_t> payload_types() const noexcept {
        auto pts = std::get_if<std::vector<uint8_t>>(&_formats);
        if (!pts)
            return {};
        return *pts;
    }
    std::span<const std::string> formats() const noexcept {
        auto fmts = std::get_if<std::vector<std::string>>(&_formats);
        if (!fmts)
            return {};
        return *fmts;
    }
    void add_format(std::string f) {
        auto fmts = std::get_if<std::vector<std::string>>(&_formats);
        if (!fmts)
            fmts = &_formats.emplace<std::vector<std::string>>();
        fmts->emplace_back(std::move(f));
    }
    void add_format(std::span<const std::string> fs) {
        auto fmts = std::get_if<std::vector<std::string>>(&_formats);
        if (!fmts)
            fmts = &_formats.emplace<std::vector<std::string>>();
        for (const auto &f : fs)
            fmts->emplace_back(f);
    }
    std::span<const sdp_rtcp_fb> rtcp_fbs() const noexcept { return _rtcp_fbs; }

    std::span<const sdp_rtpmap> rtpmaps() const noexcept { return _rtpmaps; }
    std::span<sdp_rtpmap> rtpmaps() noexcept { return _rtpmaps; }

    const sdp_rtpmap *find_rtpmap(uint8_t pt) const noexcept;
    sdp_rtpmap *find_rtpmap(uint8_t pt) noexcept;
    void add_rtpmap(sdp_rtpmap map);

    void add_feedback(sdp_rtcp_fb fb);
    void add_feedback(std::span<const sdp_rtcp_fb> fbs) {
        for (const auto &fb : fbs)
            add_feedback(fb);
    }

  private:
    std::variant<std::monostate, std::vector<uint8_t>,
                 std::vector<std::string>>
        _formats; // Payload type numbers or format strings
    std::vector<sdp_rtpmap> _rtpmaps;
    std::vector<sdp_rtcp_fb> _rtcp_fbs;
};

struct sdp_origin {
    std::string username = "-";
    std::string session_id = "0";
    std::string session_version = "0";
    std::string nettype = "IN";
    std::string addrtype = "IP4";
    std::string addr = "0.0.0.0";
};

enum class sdp_bandwidth_type {
    CT,   ///< Conference Total
    AS,   ///< Application Specific
    TIAS, ///< Transport Independent Application Specific (RFC 3890)
    RS,   ///< RTCP bandwidth — sender (RFC 3556)
    RR    ///< RTCP bandwidth — receiver (RFC 3556)
};

struct sdp_bandwidth {
    sdp_bandwidth_type type = sdp_bandwidth_type::AS;
    std::uint32_t bandwidth = 0;
};

struct sdp_group {
    std::string semantic;
    std::vector<std::string> items;
};

struct session_description final : session_description_interface {
    std::string sdp_type;

    uint8_t version = 0;
    sdp_origin origin;
    std::string session_name = "-";
    std::string uri;
    std::string email;
    std::string phone_number;

    std::string conn_nettype;
    std::string conn_addrtype;
    std::string conn_addr;

    std::optional<sdp_direction> direction;
    std::vector<sdp_bandwidth> bandwidths;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<sdp_group> groups;
    std::string ice_ufrag;
    std::string ice_pwd;
    std::vector<std::string> ice_options;
    bool ice_lite = false;
    std::vector<sdp_fingerprint> fingerprints;
    std::optional<sdp_setup_role> setup;
    bool extmap_allow_mixed = false; ///< a=extmap-allow-mixed (RFC 8285)
    sdp_msid_semantic msid_semantic;
    std::vector<std::string> candidates;
    std::vector<sdp_media> medias;

    std::string to_string() const override;
    std::string_view type() const noexcept override { return this->sdp_type; }
};

// Internal helpers
sdp_direction negotiate_direction(sdp_direction offer,
                                  sdp_direction answer) noexcept;
const char *direction_str(sdp_direction d);

} // namespace asiortc

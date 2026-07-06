#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace asiortc {

enum class sdp_direction { sendrecv, sendonly, recvonly, inactive };

struct sdp_extmap {
    int id = 0;
    std::string uri;
};

struct sdp_rtcp_fb {
    uint8_t payload_type = 0;
    std::string type;
    std::string subtype;
};

struct sdp_codec {
    uint8_t payload_type = 0;
    std::string name;
    uint32_t clock_rate = 0;
    std::string encoding_params;
    std::string fmtp_params;
};

struct sdp_media {
    std::string media_type;
    uint16_t port = 9;
    std::string proto;
    std::vector<uint8_t> payload_types;
    std::vector<std::string> fmts;

    std::string mid;
    sdp_direction direction = sdp_direction::sendrecv;
    bool rtcp_mux = false;

    std::string ice_ufrag;
    std::string ice_pwd;
    std::string fingerprint;
    std::string setup;
    std::vector<std::string> candidates;

    std::string sctpmap;
    uint16_t sctp_port = 0;

    std::vector<sdp_codec> rtpmaps;
    std::vector<std::string> fmtps;
    std::vector<sdp_extmap> extmaps;
    std::vector<sdp_rtcp_fb> rtcp_fbs;
    std::vector<std::string> ssrcs;
    std::vector<std::string> msids;
    std::vector<std::string> rids;
    std::string simulcast;

    std::string conn_nettype;
    std::string conn_addrtype;
    std::string conn_addr;

    std::vector<std::pair<std::string, std::string>> attributes;
};

struct session_description {
    std::string type;
    uint8_t version = 0;

    struct {
        std::string username = "-";
        uint64_t session_id = 0;
        uint64_t session_version = 0;
        std::string nettype = "IN";
        std::string addrtype = "IP4";
        std::string addr = "0.0.0.0";
    } origin;

    std::string session_name = "-";

    struct {
        uint64_t start = 0;
        uint64_t stop = 0;
    } timing;

    std::string conn_nettype;
    std::string conn_addrtype;
    std::string conn_addr;

    std::string msid_semantic;
    std::vector<std::string> msid_tokens;

    std::vector<std::vector<std::string>> bundle_groups;

    std::string ice_ufrag;
    std::string ice_pwd;
    std::string fingerprint;
    std::string setup;
    std::vector<std::string> candidates;
    std::string mid;

    std::vector<std::pair<std::string, std::string>> attributes;

    std::vector<sdp_media> medias;

    std::string to_string() const;
};

session_description parse_sdp(std::string_view sdp_text,
                              std::string type = "offer");

} // namespace asiortc

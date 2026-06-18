#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asiortc {

enum class sdp_direction { sendrecv, sendonly, recvonly, inactive };

struct sdp_codec {
    uint8_t payload_type;
    std::string name;
    uint32_t clock_rate;
    std::string encoding_params;
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
    std::vector<std::string> extmaps;
    std::vector<std::string> ssrcs;
    std::vector<std::string> msids;

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

    std::string ice_ufrag;
    std::string ice_pwd;
    std::string fingerprint;
    std::string setup;
    std::vector<std::string> candidates;
    std::string mid;
    std::vector<std::string> bundle_groups;

    std::vector<sdp_media> medias;

    std::vector<std::pair<std::string, std::string>> attributes;

    std::string to_string() const;
};

session_description parse_sdp(std::string_view sdp_text,
                              std::string type = "offer");

} // namespace asiortc

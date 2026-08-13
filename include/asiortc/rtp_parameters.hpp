#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "asiortc/session_description.hpp" // for sdp_direction, sdp_extmap

namespace asiortc {

struct rtp_encoding_parameters {
    bool active = true;
    std::optional<uint32_t> max_bitrate;
    double scale_resolution_down_by = 1.0;
    std::string scalability_mode;
    std::string rid;
};

struct rtp_rtcp_parameters {
    std::string cname;
    bool reduced_size = false;
};

struct rtp_send_parameters {
    std::string transaction_id;
    std::vector<rtp_encoding_parameters> encodings;
    std::vector<sdp_extmap> header_extensions;
    rtp_rtcp_parameters rtcp;
};

struct rtp_receive_decoding_parameters {
    std::string name;
    uint32_t clock_rate = 0;
    std::optional<std::uint8_t> channels;
};

struct rtp_receive_parameters {
    std::vector<sdp_extmap> header_extensions;
    rtp_rtcp_parameters rtcp;
};

struct rtp_transceiver_init {
    sdp_direction direction = sdp_direction::sendrecv;
    std::vector<rtp_encoding_parameters> send_encodings;
    std::vector<std::string> streams;
    std::vector<rtp_receive_decoding_parameters> recv_decodings;
};

} // namespace asiortc

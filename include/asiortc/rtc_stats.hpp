#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace asiortc {

struct rtc_stats {
    std::string type;
    std::string id;
    std::chrono::system_clock::time_point timestamp;
};

struct rtc_rtp_stream_stats : rtc_stats {
    uint32_t ssrc = 0;
    std::string kind;
    std::string transport_id;
};

struct rtc_received_rtp_stream_stats : rtc_rtp_stream_stats {
    uint64_t packets_received = 0;
    int64_t packets_lost = 0;
    uint32_t jitter = 0;
};

struct rtc_inbound_rtp_stream_stats : rtc_received_rtp_stream_stats {};

struct rtc_remote_inbound_rtp_stream_stats : rtc_received_rtp_stream_stats {
    double round_trip_time = 0;
    double fraction_lost = 0;
};

struct rtc_sent_rtp_stream_stats : rtc_rtp_stream_stats {
    uint64_t packets_sent = 0;
    uint64_t bytes_sent = 0;
};

struct rtc_outbound_rtp_stream_stats : rtc_sent_rtp_stream_stats {
    std::string track_id;
};

struct rtc_remote_outbound_rtp_stream_stats : rtc_sent_rtp_stream_stats {
    uint64_t remote_timestamp = 0;
};

struct rtc_transport_stats : rtc_stats {
    uint64_t packets_sent = 0;
    uint64_t packets_received = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    std::string ice_role;
    std::string dtls_state;
};

using rtc_stats_report =
    std::unordered_map<std::string, std::shared_ptr<rtc_stats>>;

} // namespace asiortc

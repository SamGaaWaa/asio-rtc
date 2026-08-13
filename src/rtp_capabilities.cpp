#include "asiortc/rtp_capabilities.hpp"

#include "sdp.hpp"

namespace asiortc {

rtc_rtp_capabilities get_capabilities(media_kind kind) {
    if (kind == media_kind::video)
        return {
            std::vector<sdp_rtpmap>{
                {96, "VP8", 90000},
                {97, "rtx", 90000},
            },
            {
                {1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
                {3,
                 "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"},
                {4,
                 "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"},
            }};
    return {
        std::vector<sdp_rtpmap>{
            {111, "opus", 48000},
            {63, "telephone-event", 8000},
            {0, "PCMU", 8000},
            {8, "PCMA", 8000},
        },
        {
            {1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
            {4,
             "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"},
        }};
}

} // namespace asiortc

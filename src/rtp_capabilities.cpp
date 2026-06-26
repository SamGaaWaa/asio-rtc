#include "asiortc/rtp_capabilities.hpp"

#include "rtp_transceiver.hpp"

namespace asiortc {

rtc_rtp_capabilities get_capabilities(media_kind kind) {
    if (kind == media_kind::video)
        return {
            default_video_codecs(),
            {
                {1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
                {3,
                 "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"},
                {4,
                 "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"},
            }};
    return {
        default_audio_codecs(),
        {
            {1, "urn:ietf:params:rtp-hdrext:sdes:mid"},
            {4,
             "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"},
        }};
}

} // namespace asiortc

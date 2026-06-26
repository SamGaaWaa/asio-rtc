#pragma once

#include <vector>

#include "asiortc/media_track.hpp"
#include "sdp.hpp"

namespace asiortc {

struct rtc_rtp_capabilities {
    std::vector<sdp_codec> codecs;
    std::vector<sdp_extmap> header_extensions;
};

rtc_rtp_capabilities get_capabilities(media_kind kind);

} // namespace asiortc

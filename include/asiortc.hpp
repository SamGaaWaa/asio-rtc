#pragma once

#include "asiortc/config.hpp"
#include "asiortc/candidate.hpp"
#include "asiortc/configuration.hpp"
#include "asiortc/peer_connection.hpp"
#include "asiortc/media_track.hpp"
#include "asiortc/rtp_capabilities.hpp"
#include "asiortc/rtp_interfaces.hpp"
#include "asiortc/rtp_parameters.hpp"
#include "asiortc/session_description.hpp"
#include "asiortc/task.hpp"

namespace asiortc {

constexpr const char *version() noexcept { return "0.0.1"; }

} // namespace asiortc

#pragma once

#include "asiortc/session_description.hpp"

#include <string_view>

namespace asiortc {

// Internal helpers
std::pair<std::string_view, std::string_view> split_attr(std::string_view attr);
sdp_direction negotiate_direction(sdp_direction offer,
                                  sdp_direction answer) noexcept;
const char *direction_str(sdp_direction d);

} // namespace asiortc

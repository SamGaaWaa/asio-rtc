#pragma once

#include "asiortc/candidate.hpp"
#include "asioice/candidate.hpp"

namespace asiortc {

asioice::candidate to_ice(const asiortc::candidate &c);
asiortc::candidate from_ice(const asioice::candidate &c);

} // namespace asiortc

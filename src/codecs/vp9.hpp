#pragma once

#include "base.hpp"

#include <memory>

namespace asiortc::codecs {

struct encoder;
struct decoder;

std::shared_ptr<encoder> make_vp9_encoder(
    const encoder_params &p = {});

std::shared_ptr<decoder> make_vp9_decoder();

} // namespace asiortc::codecs

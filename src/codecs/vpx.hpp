#pragma once

#include "asiortc/codecs/base.hpp"

#include <memory>

namespace asiortc::codecs {

struct encoder;
struct decoder;

std::shared_ptr<encoder> make_vp8_encoder(const encoder_params &p = {});

std::shared_ptr<decoder> make_vp8_decoder();

} // namespace asiortc::codecs

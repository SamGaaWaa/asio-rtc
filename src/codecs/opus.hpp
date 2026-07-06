#pragma once

#include "asiortc/codecs/base.hpp"

#include <memory>

namespace asiortc::codecs {

struct encoder;
struct decoder;

std::shared_ptr<encoder> make_opus_encoder(const encoder_params &p = {});

std::shared_ptr<decoder> make_opus_decoder();

} // namespace asiortc::codecs

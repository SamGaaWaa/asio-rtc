#pragma once

#include "asiortc/codecs/base.hpp"

#include <memory>

namespace asiortc::ffmpeg {

std::shared_ptr<asiortc::codecs::encoder>
make_vp8_encoder(const asiortc::codecs::encoder_params &p = {});

std::shared_ptr<asiortc::codecs::decoder> make_vp8_decoder();

} // namespace asiortc::ffmpeg

#pragma once

#include "asiortc/codecs/base.hpp"

#include <memory>

namespace asiortc::ffmpeg {

std::shared_ptr<asiortc::codecs::encoder>
make_h264_encoder(const asiortc::codecs::encoder_params &p = {});

std::shared_ptr<asiortc::codecs::decoder> make_h264_decoder();

} // namespace asiortc::ffmpeg

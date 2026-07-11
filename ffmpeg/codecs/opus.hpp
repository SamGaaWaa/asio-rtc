#pragma once

#include "asiortc/codecs/base.hpp"

#include <memory>

namespace asiortc::ffmpeg {
using namespace asiortc::codecs;

std::shared_ptr<asiortc::codecs::encoder>
make_opus_encoder(const asiortc::codecs::encoder_params &p = {});

std::shared_ptr<asiortc::codecs::decoder> make_opus_decoder();

} // namespace asiortc::ffmpeg

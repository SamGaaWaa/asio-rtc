#pragma once

#include <memory>

namespace asiortc::codecs {

struct encoder;
struct decoder;

std::shared_ptr<encoder> make_vp8_encoder(int bitrate = 1000000);
std::shared_ptr<decoder> make_vp8_decoder();

} // namespace asiortc::codecs

#pragma once

#include <memory>

namespace asiortc::codecs {

struct encoder;
struct decoder;

std::shared_ptr<encoder> make_vp9_encoder(int bitrate = 1000000);
std::shared_ptr<decoder> make_vp9_decoder();

} // namespace asiortc::codecs

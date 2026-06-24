#pragma once

#include <memory>

namespace asiortc::codecs {

struct encoder;
struct decoder;

std::shared_ptr<encoder> make_opus_encoder(int bitrate = 64000);
std::shared_ptr<decoder> make_opus_decoder();

} // namespace asiortc::codecs

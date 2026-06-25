#pragma once

#include <memory>

namespace asiortc::codecs {

struct encoder;
struct decoder;

std::shared_ptr<encoder> make_pcmu_encoder(int ptime_ms = 20);
std::shared_ptr<decoder> make_pcmu_decoder();

std::shared_ptr<encoder> make_pcma_encoder(int ptime_ms = 20);
std::shared_ptr<decoder> make_pcma_decoder();

} // namespace asiortc::codecs

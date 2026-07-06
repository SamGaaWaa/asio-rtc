#include "g711.hpp"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "asiortc/media_track.hpp"
#include "asiortc/codecs/base.hpp"

namespace asiortc::codecs {

static const int16_t ulaw_decode_table[256] = {
    -32124, -31100, -30076, -29052, -28028, -27004, -25980, -24956, -23932,
    -22908, -21884, -20860, -19836, -18812, -17788, -16764, -15996, -15484,
    -14972, -14460, -13948, -13436, -12924, -12412, -11900, -11388, -10876,
    -10364, -9852,  -9340,  -8828,  -8316,  -7932,  -7676,  -7420,  -7164,
    -6908,  -6652,  -6396,  -6140,  -5884,  -5628,  -5372,  -5116,  -4860,
    -4604,  -4348,  -4092,  -3900,  -3772,  -3644,  -3516,  -3388,  -3260,
    -3132,  -3004,  -2876,  -2748,  -2620,  -2492,  -2364,  -2236,  -2108,
    -1980,  -1884,  -1820,  -1756,  -1692,  -1628,  -1564,  -1500,  -1436,
    -1372,  -1308,  -1244,  -1180,  -1116,  -1052,  -988,   -924,   -876,
    -844,   -812,   -780,   -748,   -716,   -684,   -652,   -620,   -588,
    -556,   -524,   -492,   -460,   -428,   -396,   -372,   -356,   -340,
    -324,   -308,   -292,   -276,   -260,   -244,   -228,   -212,   -196,
    -180,   -164,   -148,   -132,   -120,   -112,   -104,   -96,    -88,
    -80,    -72,    -64,    -56,    -48,    -40,    -32,    -24,    -16,
    -8,     -1,     32124,  31100,  30076,  29052,  28028,  27004,  25980,
    24956,  23932,  22908,  21884,  20860,  19836,  18812,  17788,  16764,
    15996,  15484,  14972,  14460,  13948,  13436,  12924,  12412,  11900,
    11388,  10876,  10364,  9852,   9340,   8828,   8316,   7932,   7676,
    7420,   7164,   6908,   6652,   6396,   6140,   5884,   5628,   5372,
    5116,   4860,   4604,   4348,   4092,   3900,   3772,   3644,   3516,
    3388,   3260,   3132,   3004,   2876,   2748,   2620,   2492,   2364,
    2236,   2108,   1980,   1884,   1820,   1756,   1692,   1628,   1564,
    1500,   1436,   1372,   1308,   1244,   1180,   1116,   1052,   988,
    924,    876,    844,    812,    780,    748,    716,    684,    652,
    620,    588,    556,    524,    492,    460,    428,    396,    372,
    356,    340,    324,    308,    292,    276,    260,    244,    228,
    212,    196,    180,    164,    148,    132,    120,    112,    104,
    96,     88,     80,     72,     64,     56,     48,     40,     32,
    24,     16,     8,      0,
};

static const int16_t alaw_decode_table[256] = {
    -5504,  -5248,  -6016,  -5760,  -4480,  -4224,  -4992,  -4736,  -7552,
    -7296,  -8064,  -7808,  -6528,  -6272,  -7040,  -6784,  -2752,  -2624,
    -3008,  -2880,  -2240,  -2112,  -2496,  -2368,  -3776,  -3648,  -4032,
    -3904,  -3264,  -3136,  -3520,  -3392,  -22016, -20992, -24064, -23040,
    -17920, -16896, -19968, -18944, -30208, -29184, -32256, -31232, -26112,
    -25088, -28160, -27136, -11008, -10496, -12032, -11520, -8960,  -8448,
    -9984,  -9472,  -15104, -14592, -16128, -15616, -13056, -12544, -14080,
    -13568, -344,   -328,   -376,   -360,   -280,   -264,   -312,   -296,
    -472,   -456,   -504,   -488,   -408,   -392,   -440,   -424,   -88,
    -72,    -120,   -104,   -24,    -8,     -56,    -40,    -216,   -200,
    -248,   -232,   -152,   -136,   -184,   -168,   -1376,  -1312,  -1504,
    -1440,  -1120,  -1056,  -1248,  -1184,  -1888,  -1824,  -2016,  -1952,
    -1632,  -1568,  -1760,  -1696,  -688,   -656,   -752,   -720,   -560,
    -528,   -624,   -592,   -944,   -912,   -1008,  -976,   -816,   -784,
    -880,   -848,   5504,   5248,   6016,   5760,   4480,   4224,   4992,
    4736,   7552,   7296,   8064,   7808,   6528,   6272,   7040,   6784,
    2752,   2624,   3008,   2880,   2240,   2112,   2496,   2368,   3776,
    3648,   4032,   3904,   3264,   3136,   3520,   3392,   22016,  20992,
    24064,  23040,  17920,  16896,  19968,  18944,  30208,  29184,  32256,
    31232,  26112,  25088,  28160,  27136,  11008,  10496,  12032,  11520,
    8960,   8448,   9984,   9472,   15104,  14592,  16128,  15616,  13056,
    12544,  14080,  13568,  344,    328,    376,    360,    280,    264,
    312,    296,    472,    456,    504,    488,    408,    392,    440,
    424,    88,     72,     120,    104,    24,     8,      56,     40,
    216,    200,    248,    232,    152,    136,    184,    168,    1376,
    1312,   1504,   1440,   1120,   1056,   1248,   1184,   1888,   1824,
    2016,   1952,   1632,   1568,   1760,   1696,   688,    656,    752,
    720,    560,    528,    624,    592,    944,    912,    1008,   976,
    816,    784,    880,    848,
};

namespace {

static constexpr int G711_CLOCK = 8000;

static uint8_t linear_to_ulaw(int16_t pcm) {
    int sign = (pcm >> 8) & 0x80;
    if (sign)
        pcm = -pcm;
    if (pcm > 32635)
        pcm = 32635;
    pcm += 0x84;
    int exp = 7;
    for (int mask = 0x4000; (pcm & mask) == 0; mask >>= 1, --exp) {
    }
    int mant = (pcm >> (exp + 2)) & 0x0F;
    return static_cast<uint8_t>(~(sign | (exp << 4) | mant));
}

static int16_t ulaw_to_linear(uint8_t ulaw) { return ulaw_decode_table[ulaw]; }

static uint8_t linear_to_alaw(int16_t pcm) {
    int sign = (pcm >> 8) & 0x80;
    if (!sign)
        pcm = -pcm;
    if (pcm > 32635)
        pcm = 32635;
    int exp = 7;
    for (int mask = 0x4000; (pcm & mask) == 0; mask >>= 1, --exp) {
    }
    int mant = (pcm >> ((exp > 3) ? (exp + 2) : 4)) & 0x0F;
    uint8_t alaw = static_cast<uint8_t>(exp << 4 | mant);
    if (sign)
        alaw |= 0x80;
    return alaw ^ 0x55;
}

static int16_t alaw_to_linear(uint8_t alaw) { return alaw_decode_table[alaw]; }

class G711EncoderImpl final : public encoder {
  public:
    using encode_func = uint8_t (*)(int16_t);
    G711EncoderImpl(encode_func fn, int ptime_ms)
        : _encode(fn), _frame_samples(G711_CLOCK * ptime_ms / 1000) {}

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    encode(const media_frame &frame, bool force_keyframe) override {
        (void)force_keyframe;
        int samples = static_cast<int>(frame.data.size()) / 2;
        if (samples < _frame_samples)
            return {{}, 0};
        std::vector<uint8_t> payload(_frame_samples);
        auto *s16 = reinterpret_cast<const int16_t *>(frame.data.data());
        for (int i = 0; i < _frame_samples; ++i)
            payload[i] = _encode(s16[i]);
        uint32_t ts = _ts;
        _ts += _frame_samples;
        return {{std::move(payload)}, ts};
    }

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data,
         uint32_t timestamp) override {
        uint32_t ts = _ts;
        _ts += static_cast<uint32_t>(encoded_data.size());
        return {{encoded_data}, ts};
    }

  private:
    encode_func _encode;
    int _frame_samples;
    uint32_t _ts = 0;
};

class G711DecoderImpl final : public decoder {
  public:
    using decode_func = int16_t (*)(uint8_t);
    explicit G711DecoderImpl(decode_func fn) : _decode(fn) {}

    std::vector<media_frame> decode(const std::vector<uint8_t> &rtp_payload,
                                    uint32_t timestamp) override {
        std::vector<media_frame> frames;
        media_frame mf;
        mf.kind = media_kind::audio;
        mf.format = media_format::pcm_s16le;
        mf.timestamp = timestamp;
        mf.data.resize(rtp_payload.size() * 2);
        auto *s16 = reinterpret_cast<int16_t *>(mf.data.data());
        for (size_t i = 0; i < rtp_payload.size(); ++i)
            s16[i] = _decode(rtp_payload[i]);
        frames.push_back(std::move(mf));
        return frames;
    }

  private:
    decode_func _decode;
};

} // namespace

std::shared_ptr<encoder> make_pcmu_encoder(int ptime_ms) {
    return std::make_shared<G711EncoderImpl>(linear_to_ulaw, ptime_ms);
}

std::shared_ptr<decoder> make_pcmu_decoder() {
    return std::make_shared<G711DecoderImpl>(ulaw_to_linear);
}

std::shared_ptr<encoder> make_pcma_encoder(int ptime_ms) {
    return std::make_shared<G711EncoderImpl>(linear_to_alaw, ptime_ms);
}

std::shared_ptr<decoder> make_pcma_decoder() {
    return std::make_shared<G711DecoderImpl>(alaw_to_linear);
}

} // namespace asiortc::codecs

#include "vpx.hpp"
#include "asioice/config.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h> // 用于 av_rescale_q
}

#include <cstring>
#include <random>
#include <stdexcept>
#include <iostream>
#include <vector>

#include "asiortc/media_track.hpp"
#include "asiortc/codecs/base.hpp"
#include "codecs/vpx_descriptor.hpp"

namespace asiortc::ffmpeg {
using namespace asiortc::codecs;

static uint32_t to_rtp_timestamp(int64_t pts, AVRational time_base) {
    if (time_base.den == 0)
        return static_cast<uint32_t>(pts);
    // 使用 av_rescale_q 防止计算溢出并提高精度
    return static_cast<uint32_t>(av_rescale_q(pts, time_base, {1, 90000}));
}

// --- Vp8EncoderImpl ---

namespace {

static constexpr int PACKET_MAX = 1300;

class Vp8EncoderImpl : public encoder {
  public:
    Vp8EncoderImpl(const encoder_params &p)
        : _bitrate(p.bitrate.value_or(1'000'000)),
          _max_framerate(p.max_framerate.value_or(30)) {

        static thread_local std::random_device rd;
        std::mt19937 gen(rd());
        _picture_id = gen() & 0x7FFF;
    }

    ~Vp8EncoderImpl() {
        if (_ctx) {
            avcodec_free_context(&_ctx);
        }
    }

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    encode(const media_frame &frame, bool force_keyframe) override {
        if (!_ctx || _width != frame.width || _height != frame.height) {
            _width = frame.width;
            _height = frame.height;
            _init_context();
        }

        std::unique_ptr<AVFrame, void (*)(AVFrame *)> f(
            av_frame_alloc(), +[](AVFrame *f) { av_frame_free(&f); });
        if (!f)
            throw std::runtime_error{"av_frame_alloc failed"};
        f->format = _ctx->pix_fmt;
        f->width = _ctx->width;
        f->height = _ctx->height;

        if (av_image_fill_arrays(f->data, f->linesize, frame.data.data(),
                                 static_cast<AVPixelFormat>(f->format),
                                 f->width, f->height, 1) < 0) {
            ICE_IN_DEBUG { std::cerr << "av_image_fill_arrays failed\n"; }
            throw std::runtime_error{"av_image_fill_arrays failed"};
        }

        f->pts = frame.timestamp;

        if (force_keyframe)
            f->pict_type = AV_PICTURE_TYPE_I;

        int ret = avcodec_send_frame(_ctx, f.get());
        f.reset();
        if (ret < 0)
            return {{}, 0};

        std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        if (!pkt)
            throw std::runtime_error{"av_packet_alloc failed"};

        std::vector<uint8_t> encoded;
        int64_t last_pts = AV_NOPTS_VALUE;

        while (true) {
            ret = avcodec_receive_packet(_ctx, pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            last_pts = pkt->pts;
            encoded.insert(encoded.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt.get());
        }

        if (encoded.empty())
            return {{}, 0};

        uint32_t rtp_ts = to_rtp_timestamp(last_pts, _ctx->time_base);

        // 生成 RTP 包
        auto payloads = _packetize(encoded);

        // 更新 Picture ID
        _picture_id = (_picture_id + 1) & 0x7FFF;

        return {std::move(payloads), rtp_ts};
    }

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data,
         uint32_t timestamp) override {
        auto payloads = _packetize(encoded_data);
        _picture_id = (_picture_id + 1) & 0x7FFF;
        return {payloads, timestamp};
    }

    void set_parameters(const encoder_params &p) override {
        if (p.bitrate) {
            _bitrate = *p.bitrate;
            if (_ctx) {
                _ctx->bit_rate = _bitrate;
                // 更新编码器实时码率
                av_opt_set_int(_ctx->priv_data, "target-bitrate", _bitrate, 0);
            }
        }
        if (p.max_framerate && _ctx &&
            (_ctx->framerate.num != *p.max_framerate ||
             _ctx->framerate.den != 1)) {
            _ctx->framerate = {*p.max_framerate, 1};
            _ctx->gop_size = *p.max_framerate * 2;
        }
    }

  private:
    void _init_context() {
        if (_ctx)
            avcodec_free_context(&_ctx);

        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
        _ctx = avcodec_alloc_context3(codec);
        _ctx->width = _width;
        _ctx->height = _height;
        _ctx->bit_rate = _bitrate;
        _ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        _ctx->time_base = {1, 90000}; // 输入时间基设为 90kHz
        _ctx->framerate = {_max_framerate, 1};
        _ctx->gop_size = _max_framerate * 2;

        // VP8 实时编码优化参数
        av_opt_set_int(_ctx->priv_data, "cpu-used", 6,
                       0); // 0-6, 越高越快但质量下降
        av_opt_set(_ctx->priv_data, "deadline", "realtime", 0);
        av_opt_set(_ctx->priv_data, "error-resilient", "1", 0); // 增加容错

        int ret = avcodec_open2(_ctx, codec, nullptr);
        if (ret < 0) {
            throw std::runtime_error{"avcodec_open2 failed for VP8"};
        }
    }

    std::vector<std::vector<uint8_t>>
    _packetize(const std::vector<uint8_t> &buf) {
        std::vector<std::vector<uint8_t>> payloads;

        vpx_payload_descriptor desc;
        desc.partition_start = true;
        desc.partition_id = 0;
        desc.picture_id = _picture_id; // 关键帧和普通帧共用一个序列

        size_t off = 0;
        while (off < buf.size()) {
            // 1. 生成当前分片的描述符
            auto dbytes = desc.bytes();

            // 2. 计算负载大小（总包大小限制 - 描述符大小）
            size_t chunk =
                std::min(buf.size() - off,
                         static_cast<size_t>(PACKET_MAX - dbytes.size()));

            std::vector<uint8_t> p;
            p.reserve(dbytes.size() + chunk);

            // 3. 拼接 RTP Payload (Descriptor + Data)
            p.insert(p.end(), dbytes.begin(), dbytes.end());
            p.insert(p.end(), buf.begin() + off, buf.begin() + off + chunk);

            payloads.push_back(std::move(p));

            // 4. 更新状态
            desc.partition_start = false; // 后续分片 Start bit = 0
            off += chunk;
        }
        return payloads;
    }

    AVCodecContext *_ctx = nullptr;
    int _width = 0, _height = 0;
    int _bitrate = 0;
    int _max_framerate = 0;
    uint16_t _picture_id = 0;
};

// --- Vp8DecoderImpl ---

class Vp8DecoderImpl : public decoder {
  public:
    Vp8DecoderImpl() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_VP8);
        if (!codec)
            throw std::runtime_error{"VP8 Decoder not found"};

        _ctx = avcodec_alloc_context3(codec);

        // 设置输入包的时间基为 90kHz
        _ctx->pkt_timebase = {1, 90000};

        int ret = avcodec_open2(_ctx, codec, nullptr);
        if (ret < 0)
            throw std::runtime_error{"avcodec_open2 failed for VP8 decoder"};
    }

    ~Vp8DecoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
    }

    std::vector<media_frame> decode(const std::vector<uint8_t> &rtp_payload,
                                    uint32_t timestamp) override {
        if (rtp_payload.empty())
            return {};

        std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        if (av_new_packet(pkt.get(), static_cast<int>(rtp_payload.size())) < 0)
            return {};
        std::memcpy(pkt->data, rtp_payload.data(), rtp_payload.size());
        pkt->pts = timestamp;

        int ret = avcodec_send_packet(_ctx, pkt.get());
        if (ret < 0)
            return {};

        std::vector<media_frame> frames;
        std::unique_ptr<AVFrame, void (*)(AVFrame *)> f(
            av_frame_alloc(), +[](AVFrame *f) { av_frame_free(&f); });

        while (true) {
            ret = avcodec_receive_frame(_ctx, f.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;

            media_frame mf;
            mf.kind = media_kind::video;
            mf.format = media_format::yuv420p;

            // 修正时间戳：将内部 PTS 转换回 90kHz
            mf.timestamp = static_cast<uint32_t>(
                av_rescale_q(f->pts, _ctx->time_base, {1, 90000}));

            mf.width = f->width;
            mf.height = f->height;

            int buf_size = av_image_get_buffer_size(
                static_cast<AVPixelFormat>(f->format), f->width, f->height, 1);
            mf.data.resize(buf_size);
            av_image_copy_to_buffer(
                mf.data.data(), buf_size, f->data, f->linesize,
                static_cast<AVPixelFormat>(f->format), f->width, f->height, 1);

            frames.push_back(std::move(mf));
            av_frame_unref(f.get());
        }
        return frames;
    }

  private:
    AVCodecContext *_ctx = nullptr;
};

} // namespace

std::shared_ptr<encoder> make_vp8_encoder(const encoder_params &p) {
    return std::make_shared<Vp8EncoderImpl>(p);
}

std::shared_ptr<decoder> make_vp8_decoder() {
    return std::make_shared<Vp8DecoderImpl>();
}

} // namespace asiortc::ffmpeg

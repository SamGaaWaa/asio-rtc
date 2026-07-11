#include "vp9.hpp"
#include "asioice/config.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <cstring>
#include <random>
#include <stdexcept>
#include <iostream>

#include "asiortc/media_track.hpp"
#include "asiortc/codecs/base.hpp"
#include "codecs/vpx_descriptor.hpp"

namespace asiortc::ffmpeg {
using namespace asiortc::codecs;

static uint32_t to_rtp_timestamp(int64_t pts, AVRational time_base) {
    if (time_base.den == 0)
        return static_cast<uint32_t>(pts);
    return static_cast<uint32_t>(pts * 90000 * time_base.num / time_base.den);
}

namespace {

static constexpr int PACKET_MAX = 1300;

class Vp9EncoderImpl : public encoder {
  public:
    Vp9EncoderImpl(const encoder_params &p)
        : _bitrate(p.bitrate.value_or(1'000'000)),
          _max_framerate(p.max_framerate.value_or(30)) {

        static thread_local std::random_device rd;
        std::mt19937 gen(rd());
        _picture_id = gen() & 0x7FFF;
    }

    ~Vp9EncoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
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
        auto payloads = _packetize(encoded);
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
            if (_ctx)
                _ctx->bit_rate = _bitrate;
        }
        if (p.max_framerate && _ctx &&
            (_ctx->framerate.num != *p.max_framerate ||
             _ctx->framerate.den != 1)) {
            _ctx->framerate = {*p.max_framerate, 1};
            _ctx->gop_size = *p.max_framerate * 60;
        }
    }

  private:
    void _init_context() {
        if (_ctx)
            avcodec_free_context(&_ctx);

        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
        _ctx = avcodec_alloc_context3(codec);
        _ctx->width = _width;
        _ctx->height = _height;
        _ctx->bit_rate = _bitrate;
        _ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        _ctx->time_base = {1, 90000};
        _ctx->framerate = {_max_framerate, 1};
        _ctx->gop_size = _max_framerate * 60;
        _ctx->profile = 0;
        av_opt_set_int(_ctx->priv_data, "cpu-used", 5, 0);
        av_opt_set(_ctx->priv_data, "deadline", "realtime", 0);
        av_opt_set_int(_ctx->priv_data, "row-mt", 1, 0);
        avcodec_open2(_ctx, codec, nullptr);
    }

    std::vector<std::vector<uint8_t>>
    _packetize(const std::vector<uint8_t> &buf) {
        std::vector<std::vector<uint8_t>> payloads;
        vpx_payload_descriptor desc;
        desc.partition_start = true;
        desc.partition_id = 0;
        desc.picture_id = _picture_id;

        size_t off = 0;
        while (off < buf.size()) {
            auto dbytes = desc.bytes();
            size_t chunk =
                std::min(buf.size() - off, PACKET_MAX - dbytes.size());
            std::vector<uint8_t> p;
            p.reserve(dbytes.size() + chunk);
            p.insert(p.end(), dbytes.begin(), dbytes.end());
            p.insert(p.end(), buf.begin() + off, buf.begin() + off + chunk);
            payloads.push_back(std::move(p));
            desc.partition_start = false;
            off += chunk;
        }
        return payloads;
    }

    AVCodecContext *_ctx = nullptr;
    int _width, _height, _bitrate, _max_framerate;
    uint16_t _picture_id = 0;
};

class Vp9DecoderImpl : public decoder {
  public:
    Vp9DecoderImpl() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
        _ctx = avcodec_alloc_context3(codec);
        avcodec_open2(_ctx, codec, nullptr);
    }

    ~Vp9DecoderImpl() {
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
            mf.timestamp =
                static_cast<uint32_t>(f->pts != AV_NOPTS_VALUE ? f->pts : 0);
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

std::shared_ptr<encoder> make_vp9_encoder(const encoder_params &p) {
    return std::make_shared<Vp9EncoderImpl>(p);
}

std::shared_ptr<decoder> make_vp9_decoder() {
    return std::make_shared<Vp9DecoderImpl>();
}

} // namespace asiortc::ffmpeg

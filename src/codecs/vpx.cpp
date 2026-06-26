#include "vpx.hpp"
#include "asioice/config.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>
#include <iostream>

#include "asiortc/media_track.hpp"
#include "base.hpp"
#include "vpx_descriptor.hpp"

namespace asiortc::codecs {

static uint32_t to_rtp_timestamp(int64_t pts, AVRational time_base) {
    if (time_base.den == 0)
        return static_cast<uint32_t>(pts);
    return static_cast<uint32_t>(pts * 90000 * time_base.num / time_base.den);
}

// --- vpx_payload_descriptor ---

vpx_payload_descriptor
vpx_payload_descriptor::parse(const uint8_t *data, size_t len,
                              size_t &consumed) {
    vpx_payload_descriptor d;
    consumed = 0;
    if (len < 1)
        return d;

    uint8_t b0 = data[0];
    bool extended = (b0 >> 7) & 1;
    d.partition_start = (b0 >> 4) & 1;
    d.partition_id = b0 & 0xF;
    size_t pos = 1;

    if (extended) {
        if (len < pos + 1)
            return d;
        uint8_t ext = data[pos++];
        if (ext & 0x80) {
            if (len < pos + (data[pos] & 0x80 ? 2 : 1))
                return d;
            if (data[pos] & 0x80) {
                d.picture_id = ((data[pos] & 0x7F) << 8) | data[pos + 1];
                pos += 2;
            } else {
                d.picture_id = data[pos];
                pos += 1;
            }
        }
        if (ext & 0x40) {
            if (len < pos + 1)
                return d;
            d.tl0picidx = data[pos++];
        }
        if (ext & 0x30) {
            if (len < pos + 1)
                return d;
            uint8_t tk = data[pos++];
            if (ext & 0x20)
                d.tid = {(tk >> 6) & 3, (tk >> 5) & 1};
            if (ext & 0x10)
                d.keyidx = tk & 0x1F;
        }
    }

    consumed = pos;
    return d;
}

std::vector<uint8_t> vpx_payload_descriptor::bytes() const {
    std::vector<uint8_t> data;
    uint8_t octet = (partition_start << 4) | (partition_id & 0xF);

    bool has_ext = picture_id || tl0picidx || tid || keyidx;
    if (has_ext) {
        uint8_t ext = 0;
        if (picture_id)
            ext |= 0x80;
        if (tl0picidx)
            ext |= 0x40;
        if (tid)
            ext |= 0x20;
        if (keyidx)
            ext |= 0x10;
        data.push_back(0x80 | octet);
        data.push_back(ext);

        if (picture_id) {
            if (*picture_id < 128)
                data.push_back(static_cast<uint8_t>(*picture_id));
            else {
                uint16_t v = 0x8000 | *picture_id;
                data.push_back(static_cast<uint8_t>(v >> 8));
                data.push_back(static_cast<uint8_t>(v & 0xFF));
            }
        }
        if (tl0picidx)
            data.push_back(*tl0picidx);
        if (tid || keyidx) {
            uint8_t tk = 0;
            if (tid)
                tk |= (tid->first << 6) | (tid->second << 5);
            if (keyidx)
                tk |= *keyidx & 0x1F;
            data.push_back(tk);
        }
    } else {
        data.push_back(octet);
    }
    return data;
}

// --- Vp8EncoderImpl ---

namespace {

static constexpr int PACKET_MAX = 1300;
class Vp8EncoderImpl : public encoder {
  public:
    Vp8EncoderImpl(int bitrate)
        : _bitrate(bitrate) {

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
        if (!_ctx || _width != frame.width ||
            _height != frame.height) {
            _width = frame.width;
            _height = frame.height;
            _init_context();
        }

        std::unique_ptr<AVFrame, void(*)(AVFrame*)> f(
            av_frame_alloc(), +[](AVFrame *f) { av_frame_free(&f); });
        if (!f)
            throw std::runtime_error{"av_frame_alloc failed"};
        f->format = _ctx->pix_fmt;
        f->width = _ctx->width;
        f->height = _ctx->height;

        if (av_image_fill_arrays(f->data, f->linesize,
                                  frame.data.data(),
                                  static_cast<AVPixelFormat>(f->format),
                                  f->width, f->height, 1) < 0) {
            ICE_IN_DEBUG{ std::cerr << "av_image_fill_arrays failed\n"; }
            throw std::runtime_error{"av_image_fill_arrays failed"};
        }

        f->pts = frame.timestamp;

        if (force_keyframe)
            f->pict_type = AV_PICTURE_TYPE_I;

        int ret = avcodec_send_frame(_ctx, f.get());
        f.reset();
        if (ret < 0)
            return {{}, 0};

        std::unique_ptr<AVPacket, void(*)(AVPacket*)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        if (!pkt)
            throw std::runtime_error{"av_packet_alloc failed"};
        std::vector<uint8_t> encoded;
        while (true) {
            ret = avcodec_receive_packet(_ctx, pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            encoded.insert(encoded.end(), pkt->data,
                           pkt->data + pkt->size);
            av_packet_unref(pkt.get());
        }
        if (encoded.empty())
            return {{}, 0};

        uint32_t rtp_ts = to_rtp_timestamp(pkt->pts, _ctx->time_base);
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

    void set_bitrate(int bitrate) override {
        _bitrate = bitrate;
        if (_ctx)
            _ctx->bit_rate = bitrate;
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
        _ctx->time_base = {1, 90000};
        _ctx->framerate = {30, 1};
        _ctx->gop_size = 3000;
        av_opt_set_int(_ctx->priv_data, "cpu-used", 5, 0);
        av_opt_set(_ctx->priv_data, "deadline", "realtime", 0);
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
            size_t chunk = std::min(buf.size() - off, PACKET_MAX - dbytes.size());
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
    int _width, _height, _bitrate;
    uint16_t _picture_id = 0;
};

// --- Vp8DecoderImpl ---

class Vp8DecoderImpl : public decoder {
  public:
    Vp8DecoderImpl() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_VP8);
        _ctx = avcodec_alloc_context3(codec);
        avcodec_open2(_ctx, codec, nullptr);
    }

    ~Vp8DecoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
    }

    std::vector<media_frame>
    decode(const std::vector<uint8_t> &rtp_payload,
           uint32_t timestamp) override {
        std::unique_ptr<AVPacket, void(*)(AVPacket*)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        pkt->data = const_cast<uint8_t *>(rtp_payload.data());
        pkt->size = static_cast<int>(rtp_payload.size());
        pkt->pts = timestamp;

        int ret = avcodec_send_packet(_ctx, pkt.get());
        if (ret < 0)
            return {};

        std::vector<media_frame> frames;
        std::unique_ptr<AVFrame, void(*)(AVFrame*)> f(
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
                static_cast<AVPixelFormat>(f->format), f->width,
                f->height, 1);
            mf.data.resize(buf_size);
            av_image_copy_to_buffer(mf.data.data(), buf_size,
                                    f->data, f->linesize,
                                    static_cast<AVPixelFormat>(f->format),
                                    f->width, f->height, 1);

            frames.push_back(std::move(mf));
            av_frame_unref(f.get());
        }
        return frames;
    }

  private:
    AVCodecContext *_ctx = nullptr;
};

} // namespace

std::shared_ptr<encoder> make_vp8_encoder(int bitrate) {
    return std::make_shared<Vp8EncoderImpl>(bitrate);
}

std::shared_ptr<decoder> make_vp8_decoder() {
    return std::make_shared<Vp8DecoderImpl>();
}

} // namespace asiortc::codecs

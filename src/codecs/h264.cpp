#include "h264.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <cstring>
#include <random>
#include <stdexcept>

#include "asiortc/media_track.hpp"
#include "base.hpp"

namespace asiortc::codecs {

static uint32_t to_rtp_timestamp(int64_t pts, AVRational time_base) {
    if (time_base.den == 0)
        return static_cast<uint32_t>(pts);
    return static_cast<uint32_t>(pts * 90000 * time_base.num / time_base.den);
}

namespace {

static constexpr int PACKET_MAX = 1300;

static const uint8_t ANNEXB_START_4[] = {0x00, 0x00, 0x00, 0x01};
static const uint8_t ANNEXB_START_3[] = {0x00, 0x00, 0x01};

static std::vector<std::vector<uint8_t>>
_annexb_to_nalus(const uint8_t *data, size_t len) {
    std::vector<std::vector<uint8_t>> nalus;
    size_t pos = 0;
    while (pos < len) {
        while (pos < len && data[pos] == 0x00)
            ++pos;
        if (pos >= len)
            break;
        while (pos < len && data[pos] == 0x00)
            ++pos;
        if (pos >= len)
            break;
        if (data[pos] != 0x01)
            break;

        size_t start = pos + 1;
        size_t end = start;
        while (end + 4 <= len) {
            if (memcmp(data + end, ANNEXB_START_4, 4) == 0)
                break;
            if (end + 3 <= len &&
                memcmp(data + end, ANNEXB_START_3, 3) == 0)
                break;
            ++end;
        }
        if (end > start)
            nalus.push_back(std::vector<uint8_t>(data + start, data + end));
        pos = end;
    }
    return nalus;
}

class H264EncoderImpl : public encoder {
  public:
    H264EncoderImpl(int bitrate) : _bitrate(bitrate) {}

    ~H264EncoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
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
                                  f->width, f->height, 1) < 0)
            throw std::runtime_error{"av_image_fill_arrays failed"};

        f->pts = frame.timestamp;

        if (force_keyframe)
            f->pict_type = AV_PICTURE_TYPE_I;

        int ret = avcodec_send_frame(_ctx, f.get());
        f.reset();
        if (ret < 0)
            return {{}, 0};

        std::unique_ptr<AVPacket, void(*)(AVPacket*)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        std::vector<uint8_t> encoded;
        int64_t last_pts = AV_NOPTS_VALUE;
        while (true) {
            ret = avcodec_receive_packet(_ctx, pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            last_pts = pkt->pts;
            encoded.insert(encoded.end(), pkt->data,
                           pkt->data + pkt->size);
            av_packet_unref(pkt.get());
        }
        if (encoded.empty())
            return {{}, 0};

        uint32_t rtp_ts =
            to_rtp_timestamp(last_pts, _ctx->time_base);
        auto payloads = _packetize(encoded);
        return {std::move(payloads), rtp_ts};
    }

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data,
         uint32_t timestamp) override {
        auto payloads = _packetize(encoded_data);
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

        const AVCodec *codec =
            avcodec_find_encoder(AV_CODEC_ID_H264);
        _ctx = avcodec_alloc_context3(codec);
        _ctx->width = _width;
        _ctx->height = _height;
        _ctx->bit_rate = _bitrate;
        _ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        _ctx->time_base = {1, 90000};
        _ctx->framerate = {30, 1};
        _ctx->gop_size = 3000;
        _ctx->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
        _ctx->level = 31;
        av_opt_set(_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(_ctx->priv_data, "tune", "zerolatency", 0);
        avcodec_open2(_ctx, codec, nullptr);
    }

    std::vector<std::vector<uint8_t>>
    _packetize(const std::vector<uint8_t> &buf) {
        auto nalus = _annexb_to_nalus(buf.data(), buf.size());
        std::vector<std::vector<uint8_t>> payloads;

        // STAP-A for SPS+PPS combo (small NALs packed together)
        // For now just use single NAL unit or FU-A per NAL

        for (auto &nal : nalus) {
            if (nal.empty())
                continue;
            uint8_t nalu_type = nal[0] & 0x1F;

            if (nal.size() <= static_cast<size_t>(PACKET_MAX)) {
                payloads.push_back(std::move(nal));
            } else {
                // FU-A fragmentation
                size_t off = 1;
                while (off < nal.size()) {
                    size_t chunk = std::min(nal.size() - off,
                                            static_cast<size_t>(PACKET_MAX - 2));
                    bool start = (off == 1);
                    bool end = (off + chunk >= nal.size());
                    std::vector<uint8_t> fu;
                    fu.reserve(2 + chunk);
                    fu.push_back(
                        static_cast<uint8_t>((nal[0] & 0xE0) | 28));
                    fu.push_back(
                        static_cast<uint8_t>(
                            (nalu_type & 0x1F) |
                            (start ? 0x80 : 0) |
                            (end ? 0x40 : 0)));
                    fu.insert(fu.end(), nal.begin() + off,
                              nal.begin() + off + chunk);
                    payloads.push_back(std::move(fu));
                    off += chunk;
                }
            }
        }
        return payloads;
    }

    AVCodecContext *_ctx = nullptr;
    int _width, _height, _bitrate;
};

class H264DecoderImpl : public decoder {
  public:
    H264DecoderImpl() {
        const AVCodec *codec =
            avcodec_find_decoder(AV_CODEC_ID_H264);
        _ctx = avcodec_alloc_context3(codec);
        avcodec_open2(_ctx, codec, nullptr);
    }

    ~H264DecoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
    }

    std::vector<media_frame>
    decode(const std::vector<uint8_t> &rtp_payload,
           uint32_t timestamp) override {
        if (rtp_payload.empty())
            return {};

        std::vector<uint8_t> nal;
        uint8_t first_byte = rtp_payload[0];
        uint8_t nalu_type = first_byte & 0x1F;

        if (nalu_type == 28) {
            if (rtp_payload.size() < 2)
                return {};
            uint8_t fu_header = rtp_payload[1];
            bool start = (fu_header & 0x80) != 0;
            if (start) {
                _fu_buffer.clear();
                uint8_t reconstructed_nal =
                    static_cast<uint8_t>((first_byte & 0xE0) |
                                         (fu_header & 0x1F));
                _fu_buffer.push_back(reconstructed_nal);
            }
            _fu_buffer.insert(_fu_buffer.end(),
                              rtp_payload.begin() + 2,
                              rtp_payload.end());
            if (!(fu_header & 0x40))
                return {};
            nal = std::move(_fu_buffer);
            _fu_buffer.clear();
        } else {
            nal = rtp_payload;
        }

        std::vector<uint8_t> annexb = {0x00, 0x00, 0x00, 0x01};
        annexb.insert(annexb.end(), nal.begin(), nal.end());

        std::unique_ptr<AVPacket, void(*)(AVPacket*)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        pkt->data = annexb.data();
        pkt->size = static_cast<int>(annexb.size());
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
            mf.timestamp = static_cast<uint32_t>(
                f->pts != AV_NOPTS_VALUE ? f->pts : 0);
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
    std::vector<uint8_t> _fu_buffer;
};

} // namespace

std::shared_ptr<encoder> make_h264_encoder(int bitrate) {
    return std::make_shared<H264EncoderImpl>(bitrate);
}

std::shared_ptr<decoder> make_h264_decoder() {
    return std::make_shared<H264DecoderImpl>();
}

} // namespace asiortc::codecs

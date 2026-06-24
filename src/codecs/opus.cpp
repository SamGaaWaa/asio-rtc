#include "opus.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <cstring>

#include "asiortc/media_track.hpp"
#include "base.hpp"

namespace asiortc::codecs {

static uint32_t to_rtp_timestamp(int64_t pts, AVRational time_base,
                                 int clock_rate) {
    if (time_base.den == 0)
        return static_cast<uint32_t>(pts);
    return static_cast<uint32_t>(pts * clock_rate * time_base.num /
                                 time_base.den);
}

namespace {

class OpusEncoderImpl : public encoder {
  public:
    OpusEncoderImpl(int bitrate) : _bitrate(bitrate) {}

    ~OpusEncoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
    }

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    encode(const media_frame &frame, bool force_keyframe) override {
        (void)force_keyframe;

        int channels =
            static_cast<int>(frame.data.size()) / (_frame_samples * 2);
        if (channels < 1 || channels > 8)
            return {{}, 0};

        if (!_ctx || _channels != channels) {
            _channels = channels;
            _init_context();
        }

        AVFrame *f = av_frame_alloc();
        f->format = _ctx->sample_fmt;
        f->nb_samples = _frame_samples;
        av_channel_layout_copy(&f->ch_layout, &_ctx->ch_layout);
        f->pts = frame.timestamp;
        av_frame_get_buffer(f, 0);

        int buf_size = av_samples_get_buffer_size(
            nullptr, _channels, _frame_samples, _ctx->sample_fmt, 0);
        if (static_cast<int>(frame.data.size()) >= buf_size)
            std::memcpy(f->data[0], frame.data.data(), buf_size);

        int ret = avcodec_send_frame(_ctx, f);
        av_frame_free(&f);
        if (ret < 0)
            return {{}, 0};

        AVPacket *pkt = av_packet_alloc();
        std::vector<uint8_t> encoded;
        while (true) {
            ret = avcodec_receive_packet(_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            encoded.insert(encoded.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }

        uint32_t rtp_ts =
            to_rtp_timestamp(pkt->pts, _ctx->time_base, 48000);
        av_packet_free(&pkt);

        if (encoded.empty())
            return {{}, 0};

        return {{std::move(encoded)}, rtp_ts};
    }

    std::pair<std::vector<std::vector<uint8_t>>, uint32_t>
    pack(const std::vector<uint8_t> &encoded_data,
         uint32_t timestamp) override {
        return {{encoded_data}, timestamp};
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

        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
        _ctx = avcodec_alloc_context3(codec);
        _ctx->bit_rate = _bitrate;
        _ctx->sample_fmt = AV_SAMPLE_FMT_S16;
        _ctx->sample_rate = 48000;
        _ctx->time_base = {1, 48000};
        _ctx->frame_size = _frame_samples;

        av_channel_layout_default(&_ctx->ch_layout, _channels);

        avcodec_open2(_ctx, codec, nullptr);
    }

    AVCodecContext *_ctx = nullptr;
    int _channels = 0;
    int _bitrate = 64000;
    int _frame_samples = 960;
};

class OpusDecoderImpl : public decoder {
  public:
    OpusDecoderImpl() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        _ctx = avcodec_alloc_context3(codec);
        _ctx->sample_rate = 48000;
        _ctx->request_sample_fmt = AV_SAMPLE_FMT_S16;
        avcodec_open2(_ctx, codec, nullptr);
    }

    ~OpusDecoderImpl() {
        if (_ctx)
            avcodec_free_context(&_ctx);
    }

    std::vector<media_frame>
    decode(const std::vector<uint8_t> &rtp_payload,
           uint32_t timestamp) override {
        AVPacket *pkt = av_packet_alloc();
        pkt->data = const_cast<uint8_t *>(rtp_payload.data());
        pkt->size = static_cast<int>(rtp_payload.size());
        pkt->pts = timestamp;

        int ret = avcodec_send_packet(_ctx, pkt);
        av_packet_free(&pkt);
        if (ret < 0)
            return {};

        std::vector<media_frame> frames;
        AVFrame *f = av_frame_alloc();
        while (true) {
            ret = avcodec_receive_frame(_ctx, f);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;

            media_frame mf;
            mf.kind = media_kind::audio;
            mf.timestamp =
                static_cast<uint32_t>(f->pts != AV_NOPTS_VALUE ? f->pts : 0);
            mf.payload_type = 0;

            int channels = f->ch_layout.nb_channels;
            int sample_size = av_get_bytes_per_sample(
                static_cast<AVSampleFormat>(f->format));
            int data_size = f->nb_samples * channels * sample_size;
            mf.data.resize(data_size);

            bool planar =
                av_sample_fmt_is_planar(static_cast<AVSampleFormat>(f->format));
            if (planar) {
                for (int ch = 0; ch < channels; ++ch)
                    for (int s = 0; s < f->nb_samples; ++s)
                        std::memcpy(
                            mf.data.data() +
                                (s * channels + ch) * sample_size,
                            f->data[ch] + s * sample_size, sample_size);
            } else {
                std::memcpy(mf.data.data(), f->data[0], data_size);
            }

            frames.push_back(std::move(mf));
            av_frame_unref(f);
        }
        av_frame_free(&f);
        return frames;
    }

  private:
    AVCodecContext *_ctx = nullptr;
};

} // namespace

std::shared_ptr<encoder> make_opus_encoder(int bitrate) {
    return std::make_shared<OpusEncoderImpl>(bitrate);
}

std::shared_ptr<decoder> make_opus_decoder() {
    return std::make_shared<OpusDecoderImpl>();
}

} // namespace asiortc::codecs

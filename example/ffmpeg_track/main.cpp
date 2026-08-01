#include "asiortc.hpp"
#include "asioice/detail/scope_guard.hpp"

#include <deque>

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
namespace asioice {
namespace net = boost::asio;
}
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
#else
#error "Requires Boost.Asio"
#endif

#include "json.hpp"

#include "ffmpeg/codecs/opus.hpp"
#include "ffmpeg/codecs/vpx.hpp"
#include "ffmpeg/codecs/h264.hpp"
#include "ffmpeg/codecs/vp9.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <chrono>
#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8086;

std::string s_test_file = "";

static task<void> ws_send(ws_t &ws, const nlohmann::json &msg) {
    ws.text(true);
    auto d = msg.dump();
    auto [ec, n] = co_await ws.async_write(net::buffer(d),
                                           net::as_tuple(utils::use_sender));
    if (ec)
        std::cerr << "ws err: " << ec.message() << '\n';
}

static task<nlohmann::json> ws_recv(ws_t &ws) {
    beast::flat_buffer buf;
    auto [ec, n] =
        co_await ws.async_read(buf, net::as_tuple(utils::use_sender));
    if (ec)
        throw std::runtime_error("ws recv: " + ec.message());
    auto j = nlohmann::json::parse(beast::buffers_to_string(buf.data()));
    buf.clear();
    co_return j;
}

struct ffmpeg_track {
    static std::vector<std::shared_ptr<media_track>>
    open(const std::string &filepath, net::io_context &ctx);
};

namespace {

struct ffmpeg_source;

struct ffmpeg_video_track : public media_track {
    ffmpeg_video_track(std::shared_ptr<ffmpeg_source> src,
                       std::chrono::milliseconds frame_dur, std::string id,
                       net::io_context &ctx)
        : _src(std::move(src)), _id(std::move(id)), _frame_duration(frame_dur),
          _timer(ctx) {}

    media_kind kind() const noexcept override { return media_kind::video; }
    media_format format() const noexcept override;
    const std::string &id() const noexcept override { return _id; }
    track_state ready_state() const noexcept override { return _state; }
    void stop() noexcept override { _state = track_state::ended; }

    asioice::task<std::optional<media_frame>> recv() override;

    std::shared_ptr<ffmpeg_source> _src;
    std::string _id;
    track_state _state = track_state::live;
    std::chrono::milliseconds _frame_duration;
    net::steady_timer _timer;
};

struct ffmpeg_audio_track : public media_track {
    ffmpeg_audio_track(std::shared_ptr<ffmpeg_source> src, std::string id,
                       net::io_context &ctx)
        : _src(std::move(src)), _id(std::move(id)), _timer(ctx) {}

    media_kind kind() const noexcept override { return media_kind::audio; }
    media_format format() const noexcept override {
        return media_format::pcm_s16le;
    }
    const std::string &id() const noexcept override { return _id; }
    track_state ready_state() const noexcept override { return _state; }
    void stop() noexcept override { _state = track_state::ended; }

    asioice::task<std::optional<media_frame>> recv() override;

    std::shared_ptr<ffmpeg_source> _src;
    std::string _id;
    track_state _state = track_state::live;
    uint32_t _apts = 0;
    net::steady_timer _timer;
};

struct ffmpeg_source : std::enable_shared_from_this<ffmpeg_source> {
    ffmpeg_source(const std::string &filepath, net::io_context &ctx)
        : _ctx(ctx) {
        if (avformat_open_input(&_fmt_ctx, filepath.c_str(), nullptr, nullptr) <
            0) {
            _state = track_state::ended;
            throw std::runtime_error("Cannot open file: " + filepath);
        }
        if (avformat_find_stream_info(_fmt_ctx, nullptr) < 0) {
            throw std::runtime_error("Cannot find stream info");
        }

        _vstream = av_find_best_stream(_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       nullptr, 0);
        _astream = av_find_best_stream(_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1,
                                       nullptr, 0);

        // --- Video Init ---
        if (_vstream >= 0) {
            auto *stream = _fmt_ctx->streams[_vstream];
            auto codec_id = stream->codecpar->codec_id;
            if (codec_id == AV_CODEC_ID_VP8)
                _vformat = media_format::vp8;
            else if (codec_id == AV_CODEC_ID_VP9)
                _vformat = media_format::vp9;
            else if (codec_id == AV_CODEC_ID_H264)
                _vformat = media_format::h264;
            else
                _vstream = -1;

            _width = stream->codecpar->width;
            _height = stream->codecpar->height;

            _fps_num = stream->avg_frame_rate.num;
            _fps_den = stream->avg_frame_rate.den;
            if (_fps_num <= 0 || _fps_den <= 0) {
                _fps_num = 30;
                _fps_den = 1;
            }
        }

        // --- Audio Init ---
        if (_astream >= 0) {
            auto *stream = _fmt_ctx->streams[_astream];
            const AVCodec *codec =
                avcodec_find_decoder(stream->codecpar->codec_id);
            _actx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(_actx, stream->codecpar);
            if (_actx->sample_rate != 48000) {
                std::cerr << "audio: sample rate " << _actx->sample_rate
                          << " not 48000, skipping audio\n";
                avcodec_free_context(&_actx);
                _actx = nullptr;
                _astream = -1;
            } else {
                _actx->request_sample_fmt = AV_SAMPLE_FMT_S16;
                avcodec_open2(_actx, codec, nullptr);
                _ach = _actx->ch_layout.nb_channels;
                if (_ach < 1 || _ach > 2)
                    _ach = 2;
            }
        }

        static thread_local std::random_device rd;
        uint32_t base_ts = rd() & 0x7FFFFFFF;
        _vpts = base_ts;
        _apts = base_ts;
    }

    ~ffmpeg_source() {
        if (_actx)
            avcodec_free_context(&_actx);
        if (_fmt_ctx)
            avformat_close_input(&_fmt_ctx);
    }

    // Decode one packet and put into queues
    void _read_next() {
        if (_state == track_state::ended)
            return;

        std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        int ret = av_read_frame(_fmt_ctx, pkt.get());

        if (ret == AVERROR_EOF) {
            _rewind();
            return;
        } else if (ret < 0) {
            return;
        }

        // Process Video — passthrough encoded data
        if (pkt->stream_index == _vstream) {
            std::vector<uint8_t> data(pkt->size);
            std::memcpy(data.data(), pkt->data, pkt->size);
            av_packet_unref(pkt.get());

            media_frame mf;
            mf.kind = media_kind::video;
            mf.format = _vformat;
            mf.timestamp = _vpts;
            mf.data = std::move(data);

            _vpts += 3000;
            vqueue.push_back(std::move(mf));
            return;
        }

        // Process Audio
        if (pkt->stream_index == _astream && _actx) {
            ret = avcodec_send_packet(_actx, pkt.get());
            av_packet_unref(pkt.get());

            if (ret < 0)
                return;

            AVFrame *frame = av_frame_alloc();
            while ((ret = avcodec_receive_frame(_actx, frame)) == 0) {
                auto fmt = static_cast<AVSampleFormat>(frame->format);
                int sample_size = av_get_bytes_per_sample(fmt);
                bool planar = av_sample_fmt_is_planar(fmt);
                int out_ch = frame->ch_layout.nb_channels;

                auto read_sample = [&](int ch, int s) -> int16_t {
                    if (ch >= out_ch)
                        return 0;
                    const uint8_t *src;
                    if (planar)
                        src = frame->data[ch] + s * sample_size;
                    else
                        src = frame->data[0] + (s * out_ch + ch) * sample_size;
                    if (fmt == AV_SAMPLE_FMT_FLTP || fmt == AV_SAMPLE_FMT_FLT) {
                        float f;
                        std::memcpy(&f, src, 4);
                        if (f > 1.0f)
                            f = 1.0f;
                        if (f < -1.0f)
                            f = -1.0f;
                        return static_cast<int16_t>(f * 32767.0f);
                    }
                    if (fmt == AV_SAMPLE_FMT_S32P || fmt == AV_SAMPLE_FMT_S32) {
                        int32_t v;
                        std::memcpy(&v, src, 4);
                        return static_cast<int16_t>(v >> 16);
                    }
                    int16_t v;
                    std::memcpy(&v, src, 2);
                    return v;
                };

                size_t old = _apcm.size();
                _apcm.resize(old + frame->nb_samples * _ach * 2);
                for (int s = 0; s < frame->nb_samples; ++s)
                    for (int ch = 0; ch < _ach; ++ch) {
                        int16_t sample = read_sample(ch, s);
                        std::memcpy(_apcm.data() + old + (s * _ach + ch) * 2,
                                    &sample, 2);
                    }
                _apts += 960;
            }
            av_frame_free(&frame);
            return;
        }

        av_packet_unref(pkt.get());
    }

    void _rewind() {
        if (_vstream >= 0)
            av_seek_frame(_fmt_ctx, _vstream, 0, AVSEEK_FLAG_BACKWARD);
        if (_astream >= 0)
            av_seek_frame(_fmt_ctx, _astream, 0, AVSEEK_FLAG_BACKWARD);
        if (_actx)
            avcodec_flush_buffers(_actx);

        static std::random_device rd;
        uint32_t base_ts = rd() & 0x7FFFFFFF;
        _vpts = base_ts;
        _apts = base_ts;
        vqueue.clear();
        _apcm.clear();

        ++_loops;
        if (_loops % 10 == 0)
            std::cout << "ffmpeg loop " << _loops << '\n';
    }

    track_state _state = track_state::live;
    std::deque<media_frame> vqueue;
    std::vector<uint8_t> _apcm; // Resampled 48k S16 Stereo data
    std::shared_ptr<ffmpeg_video_track> _vtrack;
    std::shared_ptr<ffmpeg_audio_track> _atrack;

    AVFormatContext *_fmt_ctx = nullptr;
    AVCodecContext *_actx = nullptr;

    int _vstream = -1, _astream = -1;
    int _width = 0, _height = 0, _ach = 2;
    media_format _vformat = media_format::unknown;
    uint32_t _vpts = 1, _apts = 1;
    int _fps_num = 30, _fps_den = 1;
    net::io_context &_ctx;
    uint64_t _loops = 0;
};

asioice::task<std::optional<media_frame>> ffmpeg_video_track::recv() {
    _timer.expires_after(_frame_duration);
    auto [ec] = co_await _timer.async_wait(net::as_tuple(utils::use_sender));
    if (ec || _state == track_state::ended)
        co_return std::nullopt;

    while (_src->vqueue.empty() && _state == track_state::live) {
        _src->_read_next();
    }

    if (_src->vqueue.empty())
        co_return std::nullopt;

    auto frame = std::move(_src->vqueue.front());
    _src->vqueue.pop_front();
    co_return frame;
}

asioice::task<std::optional<media_frame>> ffmpeg_audio_track::recv() {
    _timer.expires_after(std::chrono::milliseconds(20));
    auto [ec] = co_await _timer.async_wait(net::as_tuple(utils::use_sender));
    if (ec || _state == track_state::ended)
        co_return std::nullopt;

    // 20ms @ 48kHz Stereo = 960 samples * 2 channels * 2 bytes = 3840 bytes
    const int kFrameBytes = 960 * 2 * 2;

    while (static_cast<int>(_src->_apcm.size()) < kFrameBytes) {
        _src->_read_next();
    }

    media_frame mf;
    mf.kind = media_kind::audio;
    mf.format = media_format::pcm_s16le;
    mf.timestamp = _apts;

    mf.data.assign(_src->_apcm.begin(), _src->_apcm.begin() + kFrameBytes);
    _src->_apcm.erase(_src->_apcm.begin(), _src->_apcm.begin() + kFrameBytes);

    _apts += 960;
    co_return mf;
}

} // namespace

media_format ffmpeg_video_track::format() const noexcept {
    return _src->_vformat;
}

std::vector<std::shared_ptr<media_track>>
ffmpeg_track::open(const std::string &filepath, net::io_context &ctx) {
    auto src = std::make_shared<ffmpeg_source>(filepath, ctx);
    std::vector<std::shared_ptr<media_track>> tracks;

    if (src->_vstream >= 0 && src->_vformat != media_format::unknown) {
        auto frame_dur =
            std::chrono::milliseconds(src->_fps_den * 1000 / src->_fps_num);
        auto vt =
            std::make_shared<ffmpeg_video_track>(src, frame_dur, filepath, ctx);
        src->_vtrack = vt;
        tracks.push_back(vt);
        std::cout << "ffmpeg_track: " << src->_width << "x" << src->_height
                  << " fps=" << src->_fps_num << "/" << src->_fps_den
                  << " file=" << filepath << '\n';
    }

    if (src->_astream >= 0 && src->_actx) {
        auto at =
            std::make_shared<ffmpeg_audio_track>(src, filepath + "_audio", ctx);
        src->_atrack = at;
        tracks.push_back(at);
        std::cout << "ffmpeg_audio_track: output=48000/S16/Stereo file="
                  << filepath << '\n';
    }

    return tracks;
}

struct ffmpeg_recorder {
    ffmpeg_recorder(const std::string &path, int, int, int fps_num, int fps_den,
                    int sample_rate, int channels)
        : _fps_num(fps_num), _fps_den(fps_den), _sample_rate(sample_rate),
          _ach(channels) {
        avformat_alloc_output_context2(&_fmt, nullptr, "webm", path.c_str());
        if (!_fmt)
            throw std::runtime_error{"avformat_alloc_output_context2"};
        _vst = avformat_new_stream(_fmt, nullptr);
        _ast = avformat_new_stream(_fmt, nullptr);
        avio_open(&_fmt->pb, path.c_str(), AVIO_FLAG_WRITE);

        const AVCodec *vc = avcodec_find_encoder(AV_CODEC_ID_VP8);
        _vctx = avcodec_alloc_context3(vc);
        _vctx->width = 640;
        _vctx->height = 480;
        _vctx->pix_fmt = AV_PIX_FMT_YUV420P;
        _vctx->time_base = {_fps_den, _fps_num};
        _vctx->framerate = {_fps_num, _fps_den};
        _vctx->bit_rate = 1'000'000;
        av_opt_set(_vctx->priv_data, "deadline", "realtime", 0);
        av_opt_set_int(_vctx->priv_data, "cpu-used", 5, 0);
        avcodec_open2(_vctx, vc, nullptr);
        avcodec_parameters_from_context(_vst->codecpar, _vctx);
        _vst->time_base = _vctx->time_base;

        const AVCodec *ac = avcodec_find_encoder(AV_CODEC_ID_OPUS);
        _actx = avcodec_alloc_context3(ac);
        _actx->sample_fmt = AV_SAMPLE_FMT_S16;
        _actx->sample_rate = sample_rate;
        _actx->bit_rate = 64'000;
        _actx->time_base = {1, sample_rate};
        av_channel_layout_default(&_actx->ch_layout, channels);
        avcodec_open2(_actx, ac, nullptr);
        avcodec_parameters_from_context(_ast->codecpar, _actx);
        _ast->time_base = _actx->time_base;

        if (avformat_write_header(_fmt, nullptr) < 0)
            throw std::runtime_error{"avformat_write_header"};
        avio_flush(_fmt->pb);
        _header_written = true;
        std::cout << "Recording to " << path << "\n";
    }

    ~ffmpeg_recorder() { close(); }

    void write_video(const media_frame &yuv) {
        if (_vctx->width != yuv.width || _vctx->height != yuv.height) {
            avcodec_send_frame(_vctx, nullptr);
            avcodec_free_context(&_vctx);
            const AVCodec *vc = avcodec_find_encoder(AV_CODEC_ID_VP8);
            _vctx = avcodec_alloc_context3(vc);
            _vctx->width = yuv.width;
            _vctx->height = yuv.height;
            _vctx->pix_fmt = AV_PIX_FMT_YUV420P;
            _vctx->time_base = {_fps_den, _fps_num};
            _vctx->framerate = {_fps_num, _fps_den};
            _vctx->bit_rate = 1'000'000;
            av_opt_set(_vctx->priv_data, "deadline", "realtime", 0);
            av_opt_set_int(_vctx->priv_data, "cpu-used", 5, 0);
            avcodec_open2(_vctx, vc, nullptr);
        }

        std::unique_ptr<AVFrame, void (*)(AVFrame *)> f(
            av_frame_alloc(), +[](AVFrame *f) { av_frame_free(&f); });
        f->format = _vctx->pix_fmt;
        f->width = _vctx->width;
        f->height = _vctx->height;
        if (av_image_fill_arrays(f->data, f->linesize, yuv.data.data(),
                                 AV_PIX_FMT_YUV420P, f->width, f->height,
                                 1) < 0)
            return;
        f->pts = _vpts++;
        avcodec_send_frame(_vctx, f.get());
        std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        while (avcodec_receive_packet(_vctx, pkt.get()) == 0) {
            av_packet_rescale_ts(pkt.get(), _vctx->time_base, _vst->time_base);
            pkt->stream_index = _vst->index;
            av_interleaved_write_frame(_fmt, pkt.get());
            av_packet_unref(pkt.get());
        }
    }

    void write_audio(const media_frame &pcm) {
        int sr = pcm.sample_rate ? pcm.sample_rate : _sample_rate;
        int ch = pcm.channels ? pcm.channels : _ach;

        if (_actx->sample_rate != sr || _actx->ch_layout.nb_channels != ch) {
            avcodec_send_frame(_actx, nullptr);
            avcodec_free_context(&_actx);
            const AVCodec *ac = avcodec_find_encoder(AV_CODEC_ID_OPUS);
            _actx = avcodec_alloc_context3(ac);
            _actx->sample_fmt = AV_SAMPLE_FMT_S16;
            _actx->sample_rate = sr;
            _actx->bit_rate = 64'000;
            _actx->time_base = {1, sr};
            av_channel_layout_default(&_actx->ch_layout, ch);
            avcodec_open2(_actx, ac, nullptr);
            _ach = ch;
        }

        std::unique_ptr<AVFrame, void (*)(AVFrame *)> f(
            av_frame_alloc(), +[](AVFrame *f) { av_frame_free(&f); });
        f->format = _actx->sample_fmt;
        f->nb_samples = _actx->frame_size ? _actx->frame_size : 960;
        f->sample_rate = _actx->sample_rate;
        av_channel_layout_copy(&f->ch_layout, &_actx->ch_layout);
        av_frame_get_buffer(f.get(), 0);

        int buf_size = av_samples_get_buffer_size(nullptr, ch, f->nb_samples,
                                                  _actx->sample_fmt, 0);
        if (!f->data[0] || buf_size < 0)
            return;
        if (static_cast<int>(pcm.data.size()) >= buf_size)
            std::memcpy(f->data[0], pcm.data.data(), buf_size);
        f->pts = _apts;
        _apts += f->nb_samples;
        avcodec_send_frame(_actx, f.get());
        std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
            av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
        while (avcodec_receive_packet(_actx, pkt.get()) == 0) {
            av_packet_rescale_ts(pkt.get(), _actx->time_base, _ast->time_base);
            pkt->stream_index = _ast->index;
            av_interleaved_write_frame(_fmt, pkt.get());
            av_packet_unref(pkt.get());
        }
    }

  public:
    void close() {
        if (_vctx) {
            avcodec_send_frame(_vctx, nullptr);
            std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
                av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
            while (avcodec_receive_packet(_vctx, pkt.get()) == 0) {
                av_packet_rescale_ts(pkt.get(), _vctx->time_base,
                                     _vst->time_base);
                pkt->stream_index = _vst->index;
                av_interleaved_write_frame(_fmt, pkt.get());
                av_packet_unref(pkt.get());
            }
            avcodec_free_context(&_vctx);
        }
        if (_actx) {
            avcodec_send_frame(_actx, nullptr);
            std::unique_ptr<AVPacket, void (*)(AVPacket *)> pkt(
                av_packet_alloc(), +[](AVPacket *p) { av_packet_free(&p); });
            while (avcodec_receive_packet(_actx, pkt.get()) == 0) {
                av_packet_rescale_ts(pkt.get(), _actx->time_base,
                                     _ast->time_base);
                pkt->stream_index = _ast->index;
                av_interleaved_write_frame(_fmt, pkt.get());
                av_packet_unref(pkt.get());
            }
            avcodec_free_context(&_actx);
        }
        if (_fmt) {
            if (_header_written)
                av_write_trailer(_fmt);
            avio_closep(&_fmt->pb);
            avformat_free_context(_fmt);
            _fmt = nullptr;
        }
    }

    AVFormatContext *_fmt = nullptr;
    AVCodecContext *_vctx = nullptr, *_actx = nullptr;
    AVStream *_vst = nullptr, *_ast = nullptr;
    bool _header_written = false;
    int _fps_num = 30, _fps_den = 1, _sample_rate = 48000, _ach = 2;
    int64_t _vpts = 0, _apts = 0;
};

static task<void> ffmpeg_session(net::io_context &ctx, ws_ptr ws) {
    asioice::utils::scope_guard on_exit(
        []() noexcept { std::cout << "ffmpeg_session: exited\n"; });
    std::cout << "WS connected (asiortc ffmpeg_track demo)\n";
    utils::scheduler sched{ctx};

    exec::async_scope scope;
    net::steady_timer timer(ctx);

    peer_connection conn(
        ctx.get_executor(),
        configuration{.ice_servers{.urls = {"stun:14.29.112.241:20002",
                                            "stun:stun.l.google.com:19302"}}});

    conn.register_encoder("opus", [](const codecs::encoder_params &p) {
        return ffmpeg::make_opus_encoder(p);
    });

    conn.register_decoder("VP8", [] { return ffmpeg::make_vp8_decoder(); });
    conn.register_decoder("H264", [] { return ffmpeg::make_h264_decoder(); });
    conn.register_decoder("VP9", [] { return ffmpeg::make_vp9_decoder(); });
    conn.register_decoder("opus", [] { return ffmpeg::make_opus_decoder(); });

    auto recorder = std::make_shared<ffmpeg_recorder>("recv.webm", 1280, 720,
                                                      30, 1, 48000, 2);

    conn.on_track([recorder, &ctx,
                   &scope](rtp_receiver_interface receiver,
                           std::shared_ptr<media_track> track,
                           std::vector<std::string> msids,
                           rtp_transceiver_interface transceiver) {
        std::cout << "New track: kind="
                  << (track->kind() == media_kind::audio ? "audio" : "video")
                  << " id=" << track->id() << " mid=" << transceiver.mid()
                  << '\n';
        scope.spawn(stdexec::starts_on(
            utils::scheduler{ctx}, [](auto r, auto t, auto rec) -> task<void> {
                while (true) {
                    auto frame = co_await t->recv();
                    if (!frame)
                        break;
                    auto d = r.decoder();
                    if (d) {
                        auto decoded = d->decode(frame->data, frame->timestamp);
                        for (auto &mf : decoded) {
                            if (mf.kind == media_kind::video)
                                rec->write_video(mf);
                            else
                                rec->write_audio(mf);
                        }
                    }
                }
            }(receiver, track, recorder)));
    });

    auto tracks = ffmpeg_track::open(s_test_file, ctx);
    if (tracks.empty()) {
        std::cerr << "ffmpeg_track failed to open file\n";
        co_return;
    }

    for (const auto &track : tracks) {
        conn.add_track(track, "ffmpeg");
    }

    std::cout << "Waiting for browser offer...\n";
    auto msg = co_await ws_recv(*ws);
    auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer");
    std::cout << "Offer: medias=" << offer.medias.size() << '\n';
    co_await conn.set_remote_description(std::move(offer));

    auto answer = co_await conn.create_answer();
    co_await conn.set_local_description(
        parse_sdp(answer.to_string(), "answer"));

    {
        for (int i = 0; i < 20 && conn.ice_gathering_state() !=
                                      ice_gathering_state_t::complete;
             ++i) {
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(utils::use_sender);
        }
    }

    const auto *local_desc = conn.local_description();
    if (!local_desc) {
        co_return;
    }
    auto answer_sdp = local_desc->to_string();
    std::cout << "\n=== ANSWER SDP ===\n"
              << answer_sdp << "=== END ANSWER ===\n\n";
    co_await ws_send(*ws, {{"type", "answer"}, {"sdp", answer_sdp}});
    std::cout << "Sent answer, waiting for peer_connection...\n";

    std::cout << "Waiting for ICE+DTLS+SRTP...\n";
    while (conn.connection_state() != connection_state_t::connected &&
           conn.connection_state() != connection_state_t::failed)
        co_await conn.on_connection_state_changed();
    if (conn.connection_state() != connection_state_t::connected) {
        std::cerr << "Failed to connect\n";
        co_return;
    }
    std::cout << "SRTP active, ffmpeg video flowing (ctrl-c to stop)\n";

    // Run for 60 seconds
    net::steady_timer loop_timer(ctx);
    loop_timer.expires_after(std::chrono::seconds(40));
    auto [ec] =
        co_await loop_timer.async_wait(net::as_tuple(utils::use_sender));
    if (ec)
        co_return;

    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));
    std::cout << "Done\n";
    recorder->close();
}

static task<void> http_session(net::io_context &ctx,
                               net::ip::tcp::socket sock) {
    beast::flat_buffer buf;
    http::request<http::string_body> req;
    auto [ec, n] = co_await http::async_read(sock, buf, req,
                                             net::as_tuple(utils::use_sender));
    if (ec)
        co_return;
    if (websocket::is_upgrade(req)) {
        auto ws = std::make_shared<ws_t>(std::move(sock));
        ws->set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::server));
        auto [wec] =
            co_await ws->async_accept(req, net::as_tuple(utils::use_sender));
        if (wec) {
            std::cerr << "WS handshake: " << wec.message() << '\n';
            co_return;
        }
        try {
            co_await ffmpeg_session(ctx, ws);
        } catch (const std::exception &e) {
            std::cerr << "Session error: " << e.what() << '\n';
        }
        co_return;
    }
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "asiortc");
    res.set(http::field::content_type, "text/html");
    static constexpr char html[] = {
#embed "index.html"
        , '\0'};
    res.body() = html;
    res.prepare_payload();
    co_await http::async_write(sock, res, net::as_tuple(utils::use_sender));
}

static task<void> listener(net::io_context &ctx) {
    net::ip::tcp::acceptor acc(
        ctx, net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), PORT));
    std::cout << "Server on ws://localhost:" << PORT << "/ws\n";
    while (true) {
        auto [ec, sock] =
            co_await acc.async_accept(net::as_tuple(utils::use_sender));
        if (ec)
            continue;
        exec::start_detached(http_session(ctx, std::move(sock)));
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cout << "ffmpeg_track_example [file]\n";
        return -1;
    }
    s_test_file = argv[1];
    std::cout << std::unitbuf;
    net::io_context ctx;
    asiortc::set_logger(std::make_shared<logger_interface>(),
                        ctx.get_executor());
    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, listener(ctx)));
    ctx.run();
}

#include "connection_impl.hpp"
#include "rtp.hpp"
#include "sdp.hpp"
#include "srtp_transport.hpp"

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

#include "codecs/vpx.hpp"
#include "codecs/opus.hpp"
#include "codecs/h264.hpp"

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
#include <string>
#include <vector>

using namespace asioice;
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
        ffmpeg_video_track(std::shared_ptr<ffmpeg_source> src, std::chrono::milliseconds frame_dur, std::string id, net::io_context &ctx)
            : _src(std::move(src)), _id(std::move(id)), _frame_duration(frame_dur), _timer(ctx) {}

        media_kind kind() const noexcept override { return media_kind::video; }
        std::string id() const noexcept override { return _id; }
        track_state ready_state() const noexcept override { return _state; }
        void stop() override { _state = track_state::ended; }

        asioice::task<std::optional<media_frame>> recv() override;

        std::shared_ptr<ffmpeg_source> _src;
        std::string _id;
        track_state _state = track_state::live;
        std::chrono::milliseconds _frame_duration;
        net::steady_timer _timer;
    };

    struct ffmpeg_audio_track : public media_track {
        ffmpeg_audio_track(std::shared_ptr<ffmpeg_source> src, std::string id, net::io_context &ctx)
            : _src(std::move(src)), _id(std::move(id)), _timer(ctx) {}

        media_kind kind() const noexcept override { return media_kind::audio; }
        std::string id() const noexcept override { return _id; }
        track_state ready_state() const noexcept override { return _state; }
        void stop() override { _state = track_state::ended; }

        asioice::task<std::optional<media_frame>> recv() override;

        std::shared_ptr<ffmpeg_source> _src;
        std::string _id;
        track_state _state = track_state::live;
        uint32_t _apts = 0;
        net::steady_timer _timer;
    };

    struct ffmpeg_source : std::enable_shared_from_this<ffmpeg_source> {
        ffmpeg_source(const std::string &filepath, net::io_context &ctx) : _ctx(ctx) {
            if (avformat_open_input(&_fmt_ctx, filepath.c_str(), nullptr, nullptr) < 0) {
                _state = track_state::ended;
                throw std::runtime_error("Cannot open file: " + filepath);
            }
            if (avformat_find_stream_info(_fmt_ctx, nullptr) < 0) {
                throw std::runtime_error("Cannot find stream info");
            }

            _vstream = av_find_best_stream(_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            _astream = av_find_best_stream(_fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

            // --- Video Init ---
            if (_vstream >= 0) {
                auto *stream = _fmt_ctx->streams[_vstream];
                const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
                _vctx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(_vctx, stream->codecpar);
                avcodec_open2(_vctx, codec, nullptr);

                _width = _vctx->width;
                _height = _vctx->height;

                _fps_num = stream->avg_frame_rate.num;
                _fps_den = stream->avg_frame_rate.den;
                if (_fps_num <= 0 || _fps_den <= 0) {
                    _fps_num = 30; _fps_den = 1;
                }
            }

            // --- Audio Init ---
            if (_astream >= 0) {
                auto *stream = _fmt_ctx->streams[_astream];
                const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
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
                    if (_ach < 1 || _ach > 2) _ach = 2;
                }
            }
        }

        ~ffmpeg_source() {
            if (_vctx) avcodec_free_context(&_vctx);
            if (_actx) avcodec_free_context(&_actx);
            if (_fmt_ctx) avformat_close_input(&_fmt_ctx);
        }

        // Decode one packet and put into queues
        void _read_next() {
            if (_state == track_state::ended) return;

            AVPacket *pkt = av_packet_alloc();
            int ret = av_read_frame(_fmt_ctx, pkt);
            
            if (ret == AVERROR_EOF) {
                _rewind();
                av_packet_free(&pkt);
                return;
            } else if (ret < 0) {
                av_packet_free(&pkt);
                return;
            }

            // Process Video
            if (pkt->stream_index == _vstream && _vctx) {
                ret = avcodec_send_packet(_vctx, pkt);
                av_packet_unref(pkt);
                av_packet_free(&pkt);

                if (ret < 0) return;

                AVFrame *frame = av_frame_alloc();
                ret = avcodec_receive_frame(_vctx, frame);
                
                if (ret == 0) {
                    media_frame mf;
                    mf.kind = media_kind::video;
                    mf.ssrc = 0xDEADBEEF;
                    mf.timestamp = _vpts;
                    mf.payload_type = 96;
                    mf.width = frame->width;
                    mf.height = frame->height;

                    int y_size = mf.width * mf.height;
                    mf.data.resize(y_size + y_size / 2);
                    std::memcpy(mf.data.data(), frame->data[0], y_size);
                    std::memcpy(mf.data.data() + y_size, frame->data[1],
                                y_size / 4);
                    std::memcpy(mf.data.data() + y_size + y_size / 4,
                                frame->data[2], y_size / 4);

                    _vpts += 3000;
                    vqueue.push_back(std::move(mf));
                }
                av_frame_free(&frame);
                return;
            }

            // Process Audio
            if (pkt->stream_index == _astream && _actx) {
                ret = avcodec_send_packet(_actx, pkt);
                av_packet_unref(pkt);
                av_packet_free(&pkt);

                if (ret < 0) return;

                AVFrame *frame = av_frame_alloc();
                while ((ret = avcodec_receive_frame(_actx, frame)) == 0) {
                    auto fmt = static_cast<AVSampleFormat>(frame->format);
                    int sample_size = av_get_bytes_per_sample(fmt);
                    bool planar = av_sample_fmt_is_planar(fmt);
                    int out_ch = frame->ch_layout.nb_channels;

                    auto read_sample = [&](int ch, int s) -> int16_t {
                        if (ch >= out_ch) return 0;
                        const uint8_t *src;
                        if (planar)
                            src = frame->data[ch] + s * sample_size;
                        else
                            src = frame->data[0] +
                                  (s * out_ch + ch) * sample_size;
                        if (fmt == AV_SAMPLE_FMT_FLTP ||
                            fmt == AV_SAMPLE_FMT_FLT) {
                            float f;
                            std::memcpy(&f, src, 4);
                            if (f > 1.0f) f = 1.0f;
                            if (f < -1.0f) f = -1.0f;
                            return static_cast<int16_t>(f * 32767.0f);
                        }
                        if (fmt == AV_SAMPLE_FMT_S32P ||
                            fmt == AV_SAMPLE_FMT_S32) {
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
                            std::memcpy(_apcm.data() + old +
                                            (s * _ach + ch) * 2,
                                        &sample, 2);
                        }
                    _apts += 960;
                }
                av_frame_free(&frame);
                return;
            }

            av_packet_unref(pkt);
            av_packet_free(&pkt);
        }

        void _rewind() {
            if (_vstream >= 0) av_seek_frame(_fmt_ctx, _vstream, 0, AVSEEK_FLAG_BACKWARD);
            if (_astream >= 0) av_seek_frame(_fmt_ctx, _astream, 0, AVSEEK_FLAG_BACKWARD);
            if (_vctx) avcodec_flush_buffers(_vctx);
            if (_actx) avcodec_flush_buffers(_actx);
            
            _vpts = 0;
            _apts = 0;
            vqueue.clear();
            _apcm.clear();
            
            ++_loops;
            if (_loops % 10 == 0) std::cout << "ffmpeg loop " << _loops << '\n';
        }

        track_state _state = track_state::live;
        std::deque<media_frame> vqueue;
        std::vector<uint8_t> _apcm; // Resampled 48k S16 Stereo data
        std::shared_ptr<ffmpeg_video_track> _vtrack;
        std::shared_ptr<ffmpeg_audio_track> _atrack;

        AVFormatContext *_fmt_ctx = nullptr;
        AVCodecContext *_vctx = nullptr, *_actx = nullptr;

        int _vstream = -1, _astream = -1;
        int _width = 0, _height = 0, _ach = 2;
        uint32_t _vpts = 0, _apts = 0;
        int _fps_num = 30, _fps_den = 1;
        net::io_context &_ctx;
        uint64_t _loops = 0;
    };

    asioice::task<std::optional<media_frame>> ffmpeg_video_track::recv() {
        _timer.expires_after(_frame_duration);
        auto [ec] = co_await _timer.async_wait(net::as_tuple(utils::use_sender));
        if (ec || _state == track_state::ended) co_return std::nullopt;

        while (_src->vqueue.empty() && _state == track_state::live) {
            _src->_read_next();
        }

        if (_src->vqueue.empty()) co_return std::nullopt;

        auto frame = std::move(_src->vqueue.front());
        _src->vqueue.pop_front();
        co_return frame;
    }

    asioice::task<std::optional<media_frame>> ffmpeg_audio_track::recv() {
        _timer.expires_after(std::chrono::milliseconds(20));
        auto [ec] = co_await _timer.async_wait(net::as_tuple(utils::use_sender));
        if (ec || _state == track_state::ended) co_return std::nullopt;

        // 20ms @ 48kHz Stereo = 960 samples * 2 channels * 2 bytes = 3840 bytes
        const int kFrameBytes = 960 * 2 * 2;

        while (static_cast<int>(_src->_apcm.size()) < kFrameBytes) {
            _src->_read_next();
        }

        media_frame mf;
        mf.kind = media_kind::audio;
        mf.ssrc = 0xBEEF;
        mf.timestamp = _apts;
        mf.payload_type = 111;
        
        mf.data.assign(_src->_apcm.begin(), _src->_apcm.begin() + kFrameBytes);
        _src->_apcm.erase(_src->_apcm.begin(), _src->_apcm.begin() + kFrameBytes);
        
        _apts += 960; 
        co_return mf;
    }

} // namespace

std::vector<std::shared_ptr<media_track>> ffmpeg_track::open(const std::string &filepath, net::io_context &ctx) {
    auto src = std::make_shared<ffmpeg_source>(filepath, ctx);
    std::vector<std::shared_ptr<media_track>> tracks;

    if (src->_vstream >= 0 && src->_vctx) {
        auto frame_dur = std::chrono::milliseconds(src->_fps_den * 1000 / src->_fps_num);
        auto vt = std::make_shared<ffmpeg_video_track>(src, frame_dur, filepath, ctx);
        src->_vtrack = vt;
        tracks.push_back(vt);
        std::cout << "ffmpeg_track: " << src->_width << "x" << src->_height 
                  << " fps=" << src->_fps_num << "/" << src->_fps_den << " file=" << filepath << '\n';
    }

    if (src->_astream >= 0 && src->_actx) {
        auto at = std::make_shared<ffmpeg_audio_track>(src, filepath + "_audio", ctx);
        src->_atrack = at;
        tracks.push_back(at);
        std::cout << "ffmpeg_audio_track: output=48000/S16/Stereo file=" << filepath << '\n';
    }

    return tracks;
}

static task<void> ffmpeg_session(net::io_context &ctx, ws_ptr ws) {
    asioice::utils::scope_guard on_exit([]()noexcept {
        std::cout << "ffmpeg_session: exited\n";
    });
    std::cout << "WS connected (asiortc ffmpeg_track demo)\n";
    utils::scheduler sched{ctx};

    exec::async_scope scope;
    net::steady_timer timer(ctx);

    auto conn = std::make_shared<connection_impl>(ctx.get_executor(), configuration{
        .ice_servers{
            .urls = {"stun:14.29.112.241:20002"}
        }
    });

    conn->register_encoder("VP8", [](int bps) {
        return codecs::make_vp8_encoder(bps ? bps : 1000000);
    });
    conn->register_encoder("H264", [](int bps) {
        return codecs::make_h264_encoder(bps ? bps : 1000000);
    });
    conn->register_encoder("opus", [](int bps) {
        (void)bps;
        return codecs::make_opus_encoder(64000);
    });

    conn->on_track([](std::shared_ptr<rtp_receiver> receiver,
                       std::shared_ptr<media_track> track,
                       std::vector<std::string> msids,
                       std::shared_ptr<rtp_transceiver> transceiver) {
        std::cout << "New track: kind="
                  << (track->kind() == media_kind::audio ? "audio" : "video")
                  << " id=" << track->id()
                  << " mid=" << transceiver->mid()
                  << " msids=" << msids.size() << '\n';
    });

    auto tracks = ffmpeg_track::open(s_test_file, ctx);
    if (tracks.empty()) {
        std::cerr << "ffmpeg_track failed to open file\n";
        conn->close();
        co_return;
    }

    for (auto &track : tracks) {
        if (track->kind() == media_kind::video) {
            auto video_tr = conn->add_transceiver(
                media_kind::video,
                {.direction = sdp_direction::sendonly,
                 .streams = {"camera"}});
            video_tr->sender()->set_track(track);
            std::cout << "Set video track on sender mid="
                      << video_tr->mid() << '\n';
        } else {
            auto audio_tr = conn->add_transceiver(
                media_kind::audio,
                {.direction = sdp_direction::sendonly,
                 .streams = {"mic"}});
            audio_tr->sender()->set_track(track);
            std::cout << "Set audio track on sender mid="
                      << audio_tr->mid() << '\n';
        }
    }

    auto offer = co_await conn->create_offer();
    co_await conn->set_local_description(
        parse_sdp(offer.to_string(), "offer"));

    {
        for (int i = 0;
             i < 10 && conn->ice_gathering_state() !=
                          ice_gathering_state_t::complete;
             ++i) {
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(utils::use_sender);
        }
    }

    const auto *local_desc = conn->local_description();
    if (!local_desc) {
        conn->close();
        co_return;
    }
    auto offer_sdp = local_desc->to_string();
    std::cout << "\n=== OFFER SDP ===\n"
              << offer_sdp << "=== END OFFER ===\n\n";
    co_await ws_send(*ws, {{"type", "offer"}, {"sdp", offer_sdp}});
    std::cout << "Sent offer, waiting for answer...\n";

    auto msg = co_await ws_recv(*ws);
    auto answer = parse_sdp(msg["sdp"].get<std::string>(), "answer");
    std::cout << "Answer: medias=" << answer.medias.size() << '\n';

    co_await conn->set_remote_description(std::move(answer));

    conn->on_rtp_rtcp_packet([&](asioice::io_buffer_ptr buf) {
        auto pkt = rtp::rtp_packet::parse(buf->data(), buf->size());
        if (pkt)
            std::cout << "RTP recv PT=" << (int)pkt->payload_type
                      << " seq=" << pkt->sequence_number
                      << " payload=" << pkt->payload.size() << "B\n";
    });

    std::cout << "Waiting for ICE+DTLS+SRTP...\n";
    while (conn->connection_state() != connection_state_t::connected &&
            conn->connection_state() != connection_state_t::failed)
        co_await conn->on_connection_state_changed();
    // for (int i = 0; i < 200 && !srtp; ++i) {
    //     timer.expires_after(std::chrono::milliseconds(50));
    //     co_await timer.async_wait(utils::use_sender);
    //     srtp = conn->srtp();
    // }
    // auto srtp = conn->srtp();
    if (conn->connection_state() != connection_state_t::connected) {
        std::cerr << "Failed to connect\n";
        conn->close();
        co_return;
    }
    std::cout << "SRTP active, ffmpeg video flowing (ctrl-c to stop)\n";

    // Run indefinitely
    net::steady_timer loop_timer(ctx);
    while (true) {
        loop_timer.expires_after(std::chrono::seconds(3600));
        auto [ec] = co_await loop_timer.async_wait(
            net::as_tuple(utils::use_sender));
        if (ec)
            break;
    }

    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));
    std::cout << "Done\n";
    conn->close();
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
    res.body() = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>asiortc ffmpeg_track</title>
<style>
 body{font-family:monospace;background:#111;color:#eee;margin:20px}
 video{width:640px;background:#222;border:1px solid #444}
 h3{margin:0 0 10px}
 #log{margin-top:10px;padding:8px;background:#1a1a1a;max-height:150px;
      overflow-y:auto;font-size:12px;border:1px solid #333}
</style>
</head>
<body>
<h3>asiortc ffmpeg_track</h3>
<video id="v" autoplay playsinline muted></video>
<pre id="log"></pre>
<script>
const log=document.getElementById('log');
const v=document.getElementById('v');
let unmuted=false;
function L(m){log.textContent+=m+'\n';log.scrollTop=log.scrollHeight}
function T(s){var d=new Date();return d.getHours()+':'+d.getMinutes()+':'+d.getSeconds()+'.'+d.getMilliseconds()+' '+s;}
// One-time click anywhere to unmute (Chrome autoplay policy)
document.addEventListener('click',()=>{
 if(!unmuted&&v.srcObject){v.muted=false;unmuted=true;L(T('audio unmuted'));}
});
(async()=>{
 const ws=new WebSocket('ws://'+location.host+'/ws');
 const pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});
 var combined=null;
 pc.ontrack=e=>{
  L(T('TRACK kind='+e.track.kind+' readyState='+e.track.readyState+' mid='+(e.transceiver?e.transceiver.mid:'?')));
  if(!combined) combined=new MediaStream();
  combined.addTrack(e.track);
  v.srcObject=combined;
 };
 pc.oniceconnectionstatechange=()=>L(T('ICE: '+pc.iceConnectionState));
 pc.onconnectionstatechange=()=>L(T('Conn: '+pc.connectionState));
 pc.onsignalingstatechange=()=>L(T('Sig: '+pc.signalingState));
 ws.onopen=()=>L(T('WS open'));
 ws.onclose=()=>L(T('WS closed'));
 ws.onmessage=async e=>{
  const m=JSON.parse(e.data);
  L(T('Got '+m.type));
  await pc.setRemoteDescription(new RTCSessionDescription({type:m.type,sdp:m.sdp}));
  const a=await pc.createAnswer();
  await pc.setLocalDescription(a);
  await new Promise(r=>{
   if(pc.iceGatheringState==='complete')r();
   else pc.addEventListener('icegatheringstatechange',function h(){
    if(pc.iceGatheringState==='complete'){pc.removeEventListener('icegatheringstatechange',h);r();}});
  });
  const full=pc.localDescription;
  L(T('Sending answer ('+full.sdp.split('\\r\\n').filter(l=>l.startsWith('a=candidate')).length+' cand)'));
  ws.send(JSON.stringify({type:'answer',sdp:full.sdp}));
  // poll stats after 5s
  setTimeout(async()=>{
   try{var s=await pc.getStats();s.forEach(v=>{if(v.type==='inbound-rtp')L(T('STATS '+v.kind+' pkts='+v.packetsReceived+' lost='+v.packetsLost));});}catch(x){}
  },5000);
 };
})().catch(e=>L('FATAL: '+e));
</script>
</body>
</html>)html";
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
    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, listener(ctx)));
    ctx.run();
}

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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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

struct ffmpeg_track : public media_track {
    ffmpeg_track(const std::string &filepath, net::io_context &ctx)
        : _kind(media_kind::video), _id(filepath), _timer(ctx) {

        int ret = avformat_open_input(&_fmt_ctx, filepath.c_str(),
                                       nullptr, nullptr);
        if (ret < 0) {
            std::cerr << "avformat_open_input failed\n";
            _state = track_state::ended;
            return;
        }

        avformat_find_stream_info(_fmt_ctx, nullptr);
        _stream_idx = av_find_best_stream(_fmt_ctx, AVMEDIA_TYPE_VIDEO,
                                          -1, -1, nullptr, 0);
        if (_stream_idx < 0) {
            std::cerr << "no video stream found\n";
            _state = track_state::ended;
            return;
        }

        AVStream *stream = _fmt_ctx->streams[_stream_idx];
        const AVCodec *codec =
            avcodec_find_decoder(stream->codecpar->codec_id);
        _codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(_codec_ctx, stream->codecpar);
        if (avcodec_open2(_codec_ctx, codec, nullptr) < 0) {
            std::cerr << "avcodec_open2 failed\n";
            _state = track_state::ended;
            return;
        }

        _width = _codec_ctx->width;
        _height = _codec_ctx->height;

        int fps_num = stream->avg_frame_rate.num;
        int fps_den = stream->avg_frame_rate.den;
        if (fps_num <= 0 || fps_den <= 0) {
            fps_num = 30;
            fps_den = 1;
        }
        _frame_duration = std::chrono::milliseconds(
            fps_den * 1000 / fps_num);

        std::cout << "ffmpeg_track: " << _width << "x" << _height
                  << " fps=" << fps_num << "/" << fps_den
                  << " file=" << filepath << '\n';
    }

    ~ffmpeg_track() {
        if (_codec_ctx) avcodec_free_context(&_codec_ctx);
        if (_fmt_ctx) avformat_close_input(&_fmt_ctx);
    }

    media_kind kind() const noexcept override { return _kind; }
    std::string id() const noexcept override { return _id; }
    track_state ready_state() const noexcept override { return _state; }
    void stop() override { _state = track_state::ended; }

    asioice::task<std::optional<media_frame>> recv() override {
        _timer.expires_after(_frame_duration);
        auto [ec] = co_await _timer.async_wait(
            net::as_tuple(utils::use_sender));
        if (ec || _state == track_state::ended)
            co_return std::nullopt;

        for (int tries = 0; tries < 100; ++tries) {
            AVPacket *pkt = av_packet_alloc();
            int ret = av_read_frame(_fmt_ctx, pkt);
            if (ret == AVERROR_EOF || ret < 0) {
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                av_seek_frame(_fmt_ctx, _stream_idx, 0,
                              AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(_codec_ctx);
                _pts = 0;
                ++_loops;
                if (_loops % 10 == 0)
                    std::cout << "ffmpeg loop " << _loops << '\n';
                continue;
            }

            if (pkt->stream_index != _stream_idx) {
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                continue;
            }

            ret = avcodec_send_packet(_codec_ctx, pkt);
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            if (ret < 0)
                continue;

            AVFrame *frame = av_frame_alloc();
            ret = avcodec_receive_frame(_codec_ctx, frame);
            if (ret == AVERROR(EAGAIN)) {
                av_frame_free(&frame);
                continue;
            }
            if (ret < 0) {
                av_frame_free(&frame);
                co_return std::nullopt;
            }

            media_frame mf;
            mf.kind = media_kind::video;
            mf.ssrc = 0xDEADBEEF;
            mf.timestamp = _pts;
            mf.payload_type = 96;
            mf.width = frame->width;
            mf.height = frame->height;

            int y_size = frame->width * frame->height;
            mf.data.resize(y_size + y_size / 2);
            std::memcpy(mf.data.data(), frame->data[0], y_size);
            std::memcpy(mf.data.data() + y_size, frame->data[1],
                        y_size / 4);
            std::memcpy(mf.data.data() + y_size + y_size / 4,
                        frame->data[2], y_size / 4);

            av_frame_free(&frame);
            _pts += 3000;
            co_return mf;
        }

        co_return std::nullopt;
    }

  private:
    media_kind _kind;
    std::string _id;
    track_state _state = track_state::live;
    std::chrono::milliseconds _frame_duration{33};
    uint32_t _pts = 0;
    int _width = 0, _height = 0;
    uint64_t _loops = 0;
    net::steady_timer _timer;

    AVFormatContext *_fmt_ctx = nullptr;
    AVCodecContext *_codec_ctx = nullptr;
    int _stream_idx = -1;
};

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
            .urls = {"stun:stun.l.google.com:19302"}
        }
    });

    conn->register_encoder("VP8", [](int bps) {
        return codecs::make_vp8_encoder(bps ? bps : 1000000);
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

    auto video_tr = conn->add_transceiver(
        media_kind::video,
        {.direction = sdp_direction::sendonly,
         .streams = {"camera"}});

    auto track = std::make_shared<ffmpeg_track>(s_test_file, ctx);
    if (track->stopped()) {
        std::cerr << "ffmpeg_track failed to start\n";
        conn->close();
        co_return;
    }
    video_tr->sender()->set_track(track);
    std::cout << "Set ffmpeg_track on sender mid=" << video_tr->mid()
              << '\n';

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

    auto srtp = conn->srtp();

    conn->on_rtp_rtcp_packet([&](asioice::io_buffer_ptr buf) {
        auto pkt = rtp::rtp_packet::parse(buf->data(), buf->size());
        if (pkt)
            std::cout << "RTP recv PT=" << (int)pkt->payload_type
                      << " seq=" << pkt->sequence_number
                      << " payload=" << pkt->payload.size() << "B\n";
    });

    std::cout << "Waiting for ICE+DTLS+SRTP...\n";
    for (int i = 0; i < 200 && !srtp; ++i) {
        timer.expires_after(std::chrono::milliseconds(50));
        co_await timer.async_wait(utils::use_sender);
        srtp = conn->srtp();
    }
    if (!srtp) {
        std::cerr << "SRTP setup timeout\n";
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
function L(m){log.textContent+=m+'\n';log.scrollTop=log.scrollHeight}
(async()=>{
 const ws=new WebSocket('ws://'+location.host+'/ws');
 const pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});
 pc.ontrack=e=>{L('track: '+e.track.kind);v.srcObject=e.streams[0]};
 pc.oniceconnectionstatechange=()=>L('ICE: '+pc.iceConnectionState);
 pc.onconnectionstatechange=()=>L('Conn: '+pc.connectionState);
 ws.onopen=()=>L('WS connected, waiting for offer...');
 ws.onmessage=async e=>{
  const m=JSON.parse(e.data);
  L('Got '+m.type);
  await pc.setRemoteDescription(new RTCSessionDescription({type:m.type,sdp:m.sdp}));
  const a=await pc.createAnswer();
  await pc.setLocalDescription(a);
  L('ICE gathering...');
  await new Promise(r=>{
   if(pc.iceGatheringState==='complete')r();
   else pc.addEventListener('icegatheringstatechange',function h(){
    if(pc.iceGatheringState==='complete'){pc.removeEventListener('icegatheringstatechange',h);r();}});
  });
  const full=pc.localDescription;
  L('Sending answer ('+full.sdp.split('\\r\\n').filter(l=>l.startsWith('a=candidate')).length+' candidates)');
  ws.send(JSON.stringify({type:'answer',sdp:full.sdp}));
 };
 ws.onclose=()=>L('WS closed');
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

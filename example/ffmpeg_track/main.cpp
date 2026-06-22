#include "connection_impl.hpp"
#include "rtp.hpp"
#include "sdp.hpp"
#include "srtp_transport.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/process/v2/shell.hpp>
namespace asioice {
namespace net = boost::asio;
}
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace bp = boost::process::v2;
#else
#error "Requires Boost.Asio"
#endif

#include "json.hpp"

#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>

#include <chrono>
#include <filesystem>
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
        : _kind(media_kind::video), _id(filepath), _ctx(ctx),
          _child_pipe(_ctx) {
        std::vector<std::string> ff_args;
        ff_args.push_back("-re");
        if (filepath.find("://") == std::string::npos &&
            filepath.find('.') == std::string::npos) {
            ff_args.push_back("-f");
            ff_args.push_back("lavfi");
        }
        ff_args.push_back("-i");
        ff_args.push_back(filepath);
        ff_args.push_back("-an");
        if (filepath.ends_with(".ivf") || filepath.ends_with(".webm")) {
            ff_args.push_back("-c:v");
            ff_args.push_back("copy");
        } else {
            ff_args.push_back("-c:v");
            ff_args.push_back("libvpx");
            ff_args.push_back("-cpu-used");
            ff_args.push_back("5");
            ff_args.push_back("-deadline");
            ff_args.push_back("realtime");
            ff_args.push_back("-b:v");
            ff_args.push_back("1M");
        }
        ff_args.push_back("-f");
        ff_args.push_back("ivf");
        ff_args.push_back("-");

        try {
            std::string args_str = "/usr/bin/ffmpeg ";
            for (const auto &arg : ff_args) {
                args_str += arg;
                args_str += ' ';
            }
            // args_str += '\n';
            std::cout << "CMD: " << args_str << "\n\n";
            bp::shell sh(args_str);
            auto exe = sh.exe();
            _child = bp::process(_ctx, "/usr/bin/ffmpeg", ff_args,
                                 bp::process_stdio{{}, _child_pipe, {}});
        } catch (const std::exception &e) {
            std::cerr << "ffmpeg launch failed: " << e.what() << '\n';
            _state = track_state::ended;
            return;
        }

        if (!_child->running()) {
            std::cerr << "ffmpeg failed to start\n";
            _state = track_state::ended;
            return;
        }
        std::cout << "ffmpeg pid=" << _child->id() << " reading " << filepath
                  << '\n';
    }

    media_kind kind() const noexcept override { return _kind; }
    std::string id() const noexcept override { return _id; }
    track_state ready_state() const noexcept override { return _state; }

    void stop() override {
        _state = track_state::ended;
        if (_child->running()) {
            _child->terminate();
            _child->wait();
        }
    }

    asioice::task<std::optional<media_frame>> recv() override {
        // Skip 32-byte IVF header
        if (!_ivf_header_done) {
            auto ec = co_await _read_exact(32);
            if (ec) {
                std::cerr << "IVF header read: " << ec.message() << '\n';
                co_return std::nullopt;
            }
            _ivf_header_done = true;
        }

        if (_state == track_state::ended)
            co_return std::nullopt;

        // Read 12-byte IVF frame header: 4 LE size + 8 LE PTS
        auto ec = co_await _read_exact(12);
        if (ec) {
            if (ec != net::error::eof && ec != net::error::operation_aborted)
                std::cerr << "IVF frame hdr: " << ec.message() << '\n';
            stop();
            co_return std::nullopt;
        }

        uint32_t fsz = static_cast<uint32_t>(_buf[0]) |
                       (static_cast<uint32_t>(_buf[1]) << 8) |
                       (static_cast<uint32_t>(_buf[2]) << 16) |
                       (static_cast<uint32_t>(_buf[3]) << 24);

        // Read VP8 frame data
        ec = co_await _read_exact(fsz);
        if (ec) {
            std::cerr << "IVF frame data: " << ec.message() << '\n';
            stop();
            co_return std::nullopt;
        }

        media_frame frame;
        frame.kind = media_kind::video;
        frame.ssrc = 0xDEADBEEF;
        frame.timestamp = _ts;
        frame.payload_type = 96;
        frame.marker = true;
        // Prepend VP8 payload descriptor (S=1, PID=0)
        frame.data.reserve(1 + _buf.size());
        frame.data.push_back(0x10);
        frame.data.insert(frame.data.end(), _buf.begin(), _buf.end());

        // Force first frame as keyframe (clear VP8 frame tag bit 7)
        if (_frame_count == 0)
            frame.data[1] &= 0x7F;
        ++_frame_count;

        _ts += 3000;
        co_return frame;
    }

  private:
    asioice::task<boost::system::error_code> _read_exact(size_t n) {
        _buf.resize(n);
        size_t off = 0;
        while (off < n) {
            auto [ec, count] = co_await net::async_read(
                _child_pipe, net::buffer(_buf.data() + off, n - off),
                net::as_tuple(utils::use_sender));
            if (ec)
                co_return ec;
            if (count == 0)
                co_return net::error::eof;
            off += count;
        }
        co_return boost::system::error_code{};
    }

    media_kind _kind;
    std::string _id;
    track_state _state = track_state::live;
    net::io_context &_ctx;
    net::readable_pipe _child_pipe;
    std::optional<bp::process> _child{};
    std::vector<uint8_t> _buf;
    bool _ivf_header_done = false;
    uint32_t _ts = 0;
    uint32_t _frame_count = 0;
};

static task<void> ffmpeg_session(net::io_context &ctx, ws_ptr ws) {
    std::cout << "WS connected (asiortc ffmpeg_track demo)\n";
    utils::scheduler sched{ctx};

    exec::async_scope scope;
    net::steady_timer timer(ctx);

    auto conn = std::make_shared<connection_impl>(
        ctx.get_executor(), asiortc::configuration{.ice_servers{
                                .urls = {"stun:14.29.112.241:20002"}}});

    conn->on_track([](std::shared_ptr<rtp_receiver> receiver,
                      std::shared_ptr<media_track> track,
                      std::vector<std::string> msids,
                      std::shared_ptr<rtp_transceiver> transceiver) {
        std::cout << "New track: kind="
                  << (track->kind() == media_kind::audio ? "audio" : "video")
                  << " id=" << track->id() << " mid=" << transceiver->mid()
                  << " msids=" << msids.size() << '\n';
    });

    auto video_tr = conn->add_transceiver(
        media_kind::video,
        {.direction = sdp_direction::sendonly, .streams = {"camera"}});

    // Use testsrc if no file argument
    // const char *src = "testsrc2=duration=60:size=640x480:rate=30";
    auto track = std::make_shared<ffmpeg_track>(s_test_file, ctx);
    if (track->stopped()) {
        std::cerr << "ffmpeg_track failed to start\n";
        conn->close();
        co_return;
    }
    video_tr->sender()->set_track(track);
    std::cout << "Set ffmpeg_track on sender mid=" << video_tr->mid() << '\n';

    auto offer = co_await conn->create_offer();
    co_await conn->set_local_description(parse_sdp(offer.to_string(), "offer"));

    // if (!conn->can_trickle_ice_candidates()) {
    if (1) {
        for (int i = 0; i < 10 && conn->ice_gathering_state() !=
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
    std::cout << "Answer:" << msg["sdp"].get<std::string>() << '\n';

    co_await conn->set_remote_description(std::move(answer));

    conn->on_new_ssrc([&](uint32_t ssrc, std::span<const uint8_t>) -> bool {
        std::cout << "New SSRC: 0x" << std::hex << ssrc << std::dec << '\n';
        return true;
    });

    conn->on_rtp_rtcp_packet([&](asioice::io_buffer_ptr buf) {
        auto pkt = rtp::rtp_packet::parse(buf->data(), buf->size());
        if (pkt)
            std::cout << "RTP recv PT=" << (int)pkt->payload_type
                      << " seq=" << pkt->sequence_number
                      << " payload=" << pkt->payload.size() << "B\n";
    });

    std::cout << "Waiting for ICE+DTLS+SRTP...\n";
    while (conn->connection_state() == connection_state_t::init ||
           conn->connection_state() == connection_state_t::connecting) {
        co_await conn->on_connection_state_changed();
    }
    if (conn->connection_state() != connection_state_t::connected) {
        std::cerr << "Connection timeout\n";
        conn->close();
        co_return;
    } else {
        std::cout << "Connected\n";
    }
    std::cout << "SRTP active, ffmpeg video flowing via track API\n";

    timer.expires_after(std::chrono::seconds(30));
    co_await timer.async_wait(utils::use_sender);

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
 .err{color:#f66}
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
 const pc=new RTCPeerConnection({iceServers:[{urls:'stun:14.29.112.241:20002'}]});
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
     if(pc.iceGatheringState==='complete'){
      pc.removeEventListener('icegatheringstatechange',h);
      r();}});
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

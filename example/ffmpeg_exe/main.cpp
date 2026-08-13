#include "asiortc.hpp"
#include "asiortc/any_stream_track.hpp"
#include "asioice/detail/scope_guard.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/process/v2/popen.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/environment.hpp>
namespace asioice {
namespace net = boost::asio;
}
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace process = boost::process::v2;
#else
#error "Requires Boost.Asio"
#endif

#include "json.hpp"

#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8086;

static std::string s_file_path;

//

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

//

struct H264AnnexBSplitter {
    std::vector<uint8_t> buf;
    std::vector<uint8_t> sps_pps;
    std::deque<media_frame> frame_q;
    uint32_t ts = 0;
    bool first_au = true;

    std::optional<media_frame> operator()(std::span<const uint8_t> data,
                                          std::size_t &consumed) noexcept {
        if (!frame_q.empty()) {
            auto f = std::move(frame_q.front());
            frame_q.pop_front();
            return f;
        }

        buf.insert(buf.end(), data.begin(), data.end());
        consumed = data.size();

        size_t i = 0;
        size_t au_data_start = 0;
        size_t last_start = 0;
        bool in_au = false;

        while (i + 3 < buf.size()) {
            if (buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x01) {
                size_t start_offset = i + 3;
                size_t sc_pos = i;
                i += 3;
                process_nal(start_offset, sc_pos, au_data_start, last_start,
                            in_au, first_au);
                last_start = start_offset;
            } else if (i + 3 < buf.size() && buf[i] == 0x00 &&
                       buf[i + 1] == 0x00 && buf[i + 2] == 0x00 &&
                       buf[i + 3] == 0x01) {
                size_t start_offset = i + 4;
                size_t sc_pos = i;
                i += 4;
                process_nal(start_offset, sc_pos, au_data_start, last_start,
                            in_au, first_au);
                last_start = start_offset;
            } else {
                ++i;
            }
        }

        if (last_start > 0 && !in_au) {
            buf.erase(buf.begin(), buf.begin() + last_start);
        } else if (au_data_start > 0) {
            buf.erase(buf.begin(), buf.begin() + au_data_start);
        }

        if (!frame_q.empty()) {
            auto f = std::move(frame_q.front());
            frame_q.pop_front();
            return f;
        }
        return std::nullopt;
    }

  private:
    void process_nal(size_t nal_start, size_t start_code_pos,
                     size_t &au_data_start, size_t &last_start, bool &in_au,
                     bool &first) noexcept {
        if (last_start == 0)
            return;
        if (nal_start >= buf.size() || last_start >= buf.size())
            return;

        uint8_t nal_type = buf[nal_start] & 0x1F;

        if (nal_type == 7 || nal_type == 8) {
            sps_pps.clear();
            sps_pps.insert(sps_pps.end(), buf.begin() + last_start,
                           buf.begin() + nal_start);
            return;
        }

        if (nal_type == 9 || nal_type == 1 || nal_type == 5) {
            if (au_data_start > 0 && au_data_start < start_code_pos) {
                media_frame mf;
                mf.kind = media_kind::video;
                mf.format = media_format::h264;
                mf.timestamp = ts;
                mf.data.assign(buf.begin() + au_data_start,
                               buf.begin() + start_code_pos);
                if (!sps_pps.empty()) {
                    mf.data.insert(mf.data.begin(), sps_pps.begin(),
                                   sps_pps.end());
                }
                ts += 3000;
                frame_q.push_back(std::move(mf));
            }
            first = false;
            in_au = true;
            au_data_start = start_code_pos;
            return;
        }

        in_au = true;
        if (au_data_start == 0) {
            au_data_start = start_code_pos;
            first = false;
        }
    }
};

//

static task<std::string> probe_codec_video(net::io_context &ctx) {
    auto exe = process::environment::find_executable("ffprobe");
    std::vector<std::string> args = {"-v",
                                     "error",
                                     "-select_streams",
                                     "v:0",
                                     "-show_entries",
                                     "stream=codec_name",
                                     "-of",
                                     "default=noprint_wrappers=1:nokey=1",
                                     s_file_path};
    auto pipe = process::popen(ctx.get_executor(), exe, args);
    std::vector<uint8_t> buf(512);
    auto [ec, n] = co_await net::async_read(pipe, net::buffer(buf),
                                            net::transfer_at_least(1),
                                            net::as_tuple(utils::use_sender));
    std::string result;
    if (!ec && n > 0) {
        result.assign(reinterpret_cast<char *>(buf.data()),
                      reinterpret_cast<char *>(buf.data()) + n);
        while (!result.empty() &&
               (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
    }
    co_return result;
}

//

static task<void> ffmpeg_exe_session(net::io_context &ctx, ws_ptr ws) {
    asioice::utils::scope_guard on_exit(
        []() noexcept { std::cout << "ffmpeg_exe_session: exited\n"; });
    std::cout << "WS connected (asiortc ffmpeg_exe demo)\n";
    utils::scheduler sched{ctx};

    exec::async_scope scope;
    net::steady_timer timer(ctx);

    peer_connection conn(
        ctx.get_executor(),
        configuration{.ice_servers{.urls = {"stun:14.29.112.241:20002",
                                            "stun:stun.l.google.com:19302"}}});

    conn.on_track([](rtp_receiver_interface, std::shared_ptr<media_track>,
                     std::vector<std::string>,
                     rtp_transceiver_interface transceiver) {
        std::cout << "New incoming track: mid=" << transceiver.mid() << '\n';
    });

    auto codec = co_await probe_codec_video(ctx);
    std::cout << "Input file: " << s_file_path << "\n"
              << "Video codec: " << codec << "\n";

    auto ffmpeg_path = process::environment::find_executable("ffmpeg").string();

    std::vector<std::string> args;
    args.insert(args.end(),
                {"-re", "-stream_loop", "-1", "-i", s_file_path, "-an"});

    if (codec == "h264") {
        std::cout << "Detected H.264 input, stream copy\n";
        args.insert(args.end(), {"-c:v", "copy"});
    } else {
        std::cout << "Encoding to H.264 (libx264 ultrafast zerolatency)\n";
        args.insert(args.end(), {"-c:v", "libx264", "-preset", "ultrafast",
                                 "-tune", "zerolatency", "-g", "60"});
    }

    args.insert(args.end(),
                {"-bsf:v", "h264_metadata=aud=insert", "-f", "h264", "pipe:1"});

    auto pipe = process::popen(ctx.get_executor(), ffmpeg_path, args);

    H264AnnexBSplitter splitter;
    any_stream_track track(
        std::move(pipe),
        [splitter = std::move(splitter)](std::span<const uint8_t> data,
                                         std::size_t &consumed) mutable noexcept
        -> std::optional<media_frame> { return splitter(data, consumed); },
        media_kind::video, media_format::h264);
    track.set_max_cache_size(16 << 20);

    conn.add_track(std::make_shared<any_stream_track>(std::move(track)),
                   "ffmpeg");

    std::cout << "Waiting for browser offer...\n";
    auto msg = co_await ws_recv(*ws);
    auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer");
    if (!offer) {
        std::cerr << "parse_sdp failed\n";
        co_return;
    }
    std::cout << "\n=== OFFER SDP ===\n"
              << offer->to_string() << "=== END OFFER ===\n\n";
    co_await conn.set_remote_description(std::move(offer));

    {
        auto answer = co_await conn.create_answer();
        co_await conn.set_local_description(std::move(answer));
    }

    {
        for (int i = 0; i < 20 && conn.ice_gathering_state() !=
                                      ice_gathering_state_t::complete;
             ++i) {
            timer.expires_after(std::chrono::seconds(1));
            co_await timer.async_wait(utils::use_sender);
        }
    }

    const auto *local_desc = conn.local_description();
    if (!local_desc)
        co_return;

    auto answer_sdp = local_desc->to_string();
    std::cout << "\n=== ANSWER SDP ===\n"
              << answer_sdp << "=== END ANSWER ===\n\n";
    co_await ws_send(*ws, {{"type", "answer"}, {"sdp", answer_sdp}});
    std::cout << "Sent answer, waiting for connection...\n";

    while (conn.connection_state() != connection_state_t::connected &&
           conn.connection_state() != connection_state_t::failed)
        co_await conn.on_connection_state_changed();
    if (conn.connection_state() != connection_state_t::connected) {
        std::cerr << "Failed to connect\n";
        co_return;
    }
    std::cout << "Connected, video flowing (ctrl-c to stop)\n";

    {
        net::steady_timer loop_timer(ctx);
        loop_timer.expires_after(std::chrono::seconds(3600));
        co_await loop_timer.async_wait(utils::use_sender);
    }

    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));
    std::cout << "Done\n";
}

//

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
            co_await ffmpeg_exe_session(ctx, ws);
        } catch (const std::exception &e) {
            std::cerr << "Session error: " << e.what() << '\n';
        }
        co_return;
    }
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "asiortc");
    res.set(http::field::content_type, "text/html");
    static constexpr const char *html = R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>asiortc ffmpeg_exe</title>
<style>
body{font-family:monospace;background:#111;color:#eee;margin:20px}
video{width:640px;background:#222;border:1px solid #444}
h3{margin:0 0 10px}
#log{margin-top:10px;padding:8px;background:#1a1a1a;max-height:150px;overflow-y:auto;font-size:12px;border:1px solid #333}
</style>
</head>
<body>
<h3>asiortc ffmpeg_exe</h3>
<video id="v" autoplay playsinline></video>
<pre id="log"></pre>
<script>
const log=document.getElementById('log');
const v=document.getElementById('v');
function L(m){log.textContent+=m+'\n';log.scrollTop=log.scrollHeight}
function T(s){var d=new Date();return d.getHours()+':'+d.getMinutes()+':'+d.getSeconds()+'.'+d.getMilliseconds()+' '+s;}
(async()=>{
const ws=new WebSocket('ws://'+location.host+'/ws');
const pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});
var combined=null;
pc.ontrack=e=>{
L(T('TRACK kind='+e.track.kind+' readyState='+e.track.readyState+' mid='+(e.transceiver?e.transceiver.mid:'?')));
if(!combined)combined=new MediaStream();
combined.addTrack(e.track);
v.srcObject=combined;
};
pc.oniceconnectionstatechange=()=>L(T('ICE: '+pc.iceConnectionState));
pc.onconnectionstatechange=()=>L(T('Conn: '+pc.connectionState));
pc.onsignalingstatechange=()=>L(T('Sig: '+pc.signalingState));
ws.onopen=async()=>{
L(T('WS open, creating offer'));
pc.addTransceiver('video',{direction:'recvonly'});
const o=await pc.createOffer();
await pc.setLocalDescription(o);
await new Promise(r=>{
if(pc.iceGatheringState==='complete')r();
else pc.addEventListener('icegatheringstatechange',function h(){
if(pc.iceGatheringState==='complete'){pc.removeEventListener('icegatheringstatechange',h);r();}
});
});
const full=pc.localDescription;
L(T('Sending offer ('+full.sdp.split('\\r\\n').filter(l=>l.startsWith('a=candidate')).length+' cand)'));
ws.send(JSON.stringify({type:'offer',sdp:full.sdp}));
};
ws.onclose=()=>L(T('WS closed'));
ws.onmessage=async e=>{
const m=JSON.parse(e.data);
L(T('Got '+m.type));
await pc.setRemoteDescription(new RTCSessionDescription({type:m.type,sdp:m.sdp}));
};
})().catch(e=>L('FATAL: '+e));
</script>
</body>
</html>
)";
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
        std::cout << "Usage: ffmpeg_exe <file>\n";
        return -1;
    }
    s_file_path = argv[1];

    std::cout << std::unitbuf;
    net::io_context ctx;
    asiortc::set_logger(std::make_shared<logger_interface>(),
                        ctx.get_executor());
    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, listener(ctx)));
    ctx.run();
}

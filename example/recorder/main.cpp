#include "asiortc.hpp"
#include "asioice/detail/scope_guard.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
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

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8087;

static std::string s_output_path = "recording.webm";

//
// VP8 IVF muxing (DKIF container)
//

struct vp8_ivf_muxer {
    bool header_written = false;

    static bool keyframe_dims(std::span<const uint8_t> data, uint16_t &w,
                              uint16_t &h) noexcept {
        if (data.size() < 10)
            return false;
        if ((data[0] & 0x01) != 0)
            return false;
        if (data[3] != 0x9D || data[4] != 0x01 || data[5] != 0x2A)
            return false;
        w = static_cast<uint16_t>((data[6] | (data[7] << 8)) & 0x3FFF);
        h = static_cast<uint16_t>((data[8] | (data[9] << 8)) & 0x3FFF);
        return true;
    }

    static std::array<uint8_t, 32> header(uint16_t w, uint16_t h) noexcept {
        std::array<uint8_t, 32> b{};
        b[0] = 'D';
        b[1] = 'K';
        b[2] = 'I';
        b[3] = 'F';
        b[4] = 0;
        b[5] = 0;
        b[6] = 32;
        b[7] = 0;
        b[8] = 'V';
        b[9] = 'P';
        b[10] = '8';
        b[11] = '0';
        b[12] = static_cast<uint8_t>(w & 0xFF);
        b[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
        b[14] = static_cast<uint8_t>(h & 0xFF);
        b[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
        uint32_t rate = 90000;
        b[16] = static_cast<uint8_t>(rate & 0xFF);
        b[17] = static_cast<uint8_t>((rate >> 8) & 0xFF);
        b[18] = static_cast<uint8_t>((rate >> 16) & 0xFF);
        b[19] = static_cast<uint8_t>((rate >> 24) & 0xFF);
        b[20] = 1;
        return b;
    }

    static std::array<uint8_t, 12> frame_header(uint32_t size,
                                                uint64_t ts) noexcept {
        std::array<uint8_t, 12> b{};
        b[0] = static_cast<uint8_t>(size & 0xFF);
        b[1] = static_cast<uint8_t>((size >> 8) & 0xFF);
        b[2] = static_cast<uint8_t>((size >> 16) & 0xFF);
        b[3] = static_cast<uint8_t>((size >> 24) & 0xFF);
        for (int i = 0; i < 8; ++i)
            b[4 + i] = static_cast<uint8_t>((ts >> (8 * i)) & 0xFF);
        return b;
    }
};

//
// WebSocket signaling helpers
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
// Recording loop: pull VP8 frames from the receiver track, mux into IVF and
// write to ffmpeg's stdin.
//

static task<void> record_loop(std::shared_ptr<media_track> track,
                              std::shared_ptr<process::popen> ffmpeg) {
    asioice::utils::scope_guard on_exit(
        []() noexcept { std::cout << "record_loop: exited\n"; });

    vp8_ivf_muxer muxer;
    uint64_t frames = 0;
    uint64_t ts_base = 0;
    bool have_base = false;
    auto &stdin_pipe = ffmpeg->get_stdin();

    while (true) {
        auto mfs = co_await track->recv(std::span<const encode_target>{});
        if (mfs.empty())
            co_return;

        for (auto &mf : mfs) {
            if (mf.data.empty())
                continue;

            if (!muxer.header_written) {
                uint16_t w = 0, h = 0;
                if (!vp8_ivf_muxer::keyframe_dims(mf.data, w, h))
                    continue;
                auto hdr = vp8_ivf_muxer::header(w, h);
                auto [ec, n] =
                    co_await net::async_write(stdin_pipe, net::buffer(hdr),
                                              net::as_tuple(utils::use_sender));
                if (ec)
                    co_return;
                muxer.header_written = true;
                ts_base = mf.timestamp;
                have_base = true;
                std::cout << "IVF header written: " << w << "x" << h << "\n";
            }

            uint64_t ts = have_base ? (mf.timestamp - ts_base) : mf.timestamp;
            auto fh = vp8_ivf_muxer::frame_header(
                static_cast<uint32_t>(mf.data.size()), ts);
            auto [ec, n] = co_await net::async_write(
                stdin_pipe, net::buffer(fh), net::as_tuple(utils::use_sender));
            if (ec)
                co_return;
            std::tie(ec, n) =
                co_await net::async_write(stdin_pipe, net::buffer(mf.data),
                                          net::as_tuple(utils::use_sender));
            if (ec)
                co_return;
            ++frames;
        }
    }
}

//
// Per-WebSocket session
//

static task<void> recorder_session(net::io_context &ctx, ws_ptr ws) {
    asioice::utils::scope_guard on_exit(
        []() noexcept { std::cout << "recorder_session: exited\n"; });
    std::cout << "WS connected (asiortc recorder)\n";
    utils::scheduler sched{ctx};

    exec::async_scope scope;
    net::steady_timer timer(ctx);

    peer_connection conn(
        ctx.get_executor(),
        configuration{.ice_servers{.urls = {"stun:stun.l.google.com:19302"}}});

    std::shared_ptr<media_track> recv_video_track;
    conn.on_track([&](rtp_receiver_interface,
                      std::shared_ptr<media_track> track,
                      std::vector<std::string>,
                      rtp_transceiver_interface transceiver) {
        std::cout << "Incoming track: "
                  << (track && track->kind() == media_kind::video ? "video"
                                                                  : "audio")
                  << " mid=" << transceiver.mid() << '\n';
        if (track && track->kind() == media_kind::video && !recv_video_track)
            recv_video_track = std::move(track);
    });

    conn.add_transceiver(media_description::make_default(media_format::vp8),
                         {.direction = sdp_direction::recvonly});

    auto ffmpeg_path = process::environment::find_executable("ffmpeg").string();
    if (ffmpeg_path.empty()) {
        std::cerr << "ffmpeg not found in PATH\n";
        co_return;
    }
    std::vector<std::string> args = {"-y",   "-loglevel",  "error",  "-f",
                                     "ivf",  "-i",         "pipe:0", "-c:v",
                                     "copy", s_output_path};
    auto ffmpeg =
        std::make_shared<process::popen>(ctx.get_executor(), ffmpeg_path, args);

    std::cout << "Waiting for browser offer...\n";
    auto msg = co_await ws_recv(*ws);
    auto offer_str = msg["sdp"].get<std::string>();
    std::cout << "\n[OFFER]:\n" << offer_str << "\n\n";
    auto offer = parse_sdp(offer_str, "offer");
    if (!offer) {
        std::cerr << "parse_sdp failed\n";
        co_return;
    }
    co_await conn.set_remote_description(std::move(offer));

    {
        auto answer = co_await conn.create_answer();
        std::cout << "\n[ANSWER]:\n" << answer->to_string() << "\n\n";
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
    std::cout << "Connected, recording to " << s_output_path << "\n";

    if (recv_video_track) {
        scope.spawn(stdexec::starts_on(
            sched, record_loop(std::move(recv_video_track), ffmpeg)));
    } else {
        std::cerr << "No video track received\n";
    }

    try {
        while (true) {
            auto m = co_await ws_recv(*ws);
            if (m.contains("type") && m["type"].get<std::string>() == "stop")
                break;
        }
    } catch (const std::exception &) {
        // WS closed by the browser
    }

    std::cout << "Stopping recording...\n";
    scope.request_stop();
    co_await (scope.on_empty() | stdexec::continues_on(sched));

    ffmpeg->get_stdin().close();
    auto [ec, code] =
        co_await ffmpeg->async_wait(net::as_tuple(utils::use_sender));
    std::cout << "ffmpeg exited with code " << code << "\n"
              << "Saved to " << s_output_path << "\n";
}

//
// HTTP + WebSocket server
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
            co_await recorder_session(ctx, ws);
        } catch (const std::exception &e) {
            std::cerr << "Session error: " << e.what() << '\n';
        }
        co_return;
    }
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "asiortc");
    res.set(http::field::content_type, "text/html");
    static constexpr const char html[] = {
#embed "index.html"
        , '\0'};
    res.body() = std::string{html};
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
    if (argc >= 2)
        s_output_path = argv[1];

    std::cout << std::unitbuf;
    net::io_context ctx;
    asiortc::set_logger(std::make_shared<logger_interface>(),
                        ctx.get_executor());
    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, listener(ctx)));
    ctx.run();
}

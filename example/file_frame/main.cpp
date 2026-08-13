#include "asiortc.hpp"
#include "asiortc/queue_track.hpp"
#include "asioice/detail/scope_guard.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace asiortc;

using ws_t = websocket::stream<beast::tcp_stream>;
using ws_ptr = std::shared_ptr<ws_t>;
static const uint16_t PORT = 8086;

static std::string s_input_dir;

namespace fs = std::filesystem;

struct file_frame_track : queue_track {
    file_frame_track(media_description desc) : queue_track(std::move(desc)) {}
};

static std::vector<std::string> list_frame_files(const std::string &dir,
                                                 const std::string &ext) {
    std::vector<std::string> files;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() == ("." + ext))
            files.push_back(entry.path().filename().string());
    }
    std::ranges::sort(files);
    return files;
}

static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char *>(data.data()), sz);
    return data;
}

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

static task<void> probe_file(net::io_context &ctx, int &fps_num, int &fps_den) {
    auto exe = process::environment::find_executable("ffprobe");
    std::vector<std::string> args = {"-v",
                                     "error",
                                     "-select_streams",
                                     "v:0",
                                     "-show_entries",
                                     "stream=r_frame_rate",
                                     "-of",
                                     "default=noprint_wrappers=1:nokey=1",
                                     "placeholder"};
    args.back() = s_input_dir + "/../../test.webm";

    auto pipe = process::popen(ctx.get_executor(), exe, args);
    std::vector<uint8_t> buf(512);
    auto [ec, n] = co_await net::async_read(pipe, net::buffer(buf),
                                            net::transfer_at_least(1),
                                            net::as_tuple(utils::use_sender));
    if (!ec && n > 0) {
        std::string result(reinterpret_cast<char *>(buf.data()),
                           reinterpret_cast<char *>(buf.data()) + n);
        while (!result.empty() &&
               (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
        auto slash = result.find('/');
        if (slash != std::string::npos) {
            fps_num = std::stoi(result.substr(0, slash));
            fps_den = std::stoi(result.substr(slash + 1));
        }
    }
    if (fps_num <= 0) {
        fps_num = 30;
        fps_den = 1;
    }
}

//

static task<void> file_frame_session(net::io_context &ctx, ws_ptr ws) {
    asioice::utils::scope_guard on_exit(
        []() noexcept { std::cout << "file_frame_session: exited\n"; });
    std::cout << "WS connected (asiortc file_frame demo)\n";
    utils::scheduler sched{ctx};

    auto video_files = list_frame_files(s_input_dir + "/video_frame", "h264");
    auto audio_files = list_frame_files(s_input_dir + "/audio_frame", "opus");
    std::cout << "Video frames: " << video_files.size() << "\n"
              << "Audio frames: " << audio_files.size() << "\n";

    int fps_num = 0, fps_den = 0;
    co_await probe_file(ctx, fps_num, fps_den);

    auto video_ms = std::chrono::milliseconds(fps_den * 1000 / fps_num);
    auto audio_ms = std::chrono::milliseconds(20);
    std::cout << "FPS: " << fps_num << "/" << fps_den
              << "  video_interval: " << video_ms.count() << "ms\n";

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

    auto video_track = std::make_shared<file_frame_track>(
        media_description::make_default(media_format::h264));
    auto audio_track = std::make_shared<file_frame_track>(
        media_description::make_default(media_format::opus));

    conn.add_transceiver(media_description::make_default(media_format::h264));
    conn.add_transceiver(media_description::make_default(media_format::opus));
    conn.add_track(video_track, {"file_frame"});
    conn.add_track(audio_track, {"file_frame"});

    std::cout << "Waiting for browser offer...\n";
    auto msg = co_await ws_recv(*ws);
    auto offer = parse_sdp(msg["sdp"].get<std::string>(), "offer");
    if (!offer) {
        std::cerr << "parse_sdp failed\n";
        co_return;
    }
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
    std::cout << "Connected, video+audio flowing (ctrl-c to stop)\n";

    scope.spawn(stdexec::starts_on(
        sched,
        [](auto video_track, auto video_files, net::any_io_executor ex,
           std::chrono::milliseconds video_ms,
           std::string input_dir) -> task<void> {
            net::steady_timer t(ex);
            uint32_t ts = 0;
            while (true) {
                for (size_t i = 0;; ++i) {
                    if (i == video_files.size())
                        i = 0;
                    t.expires_after(video_ms);
                    auto [ec] =
                        co_await t.async_wait(net::as_tuple(utils::use_sender));
                    if (ec)
                        co_return;

                    auto data =
                        read_file(input_dir + "/video_frame/" + video_files[i]);
                    if (data.empty())
                        co_return;

                    media_frame mf;
                    mf.kind = media_kind::video;
                    mf.format = media_format::h264;
                    mf.timestamp = ts;
                    mf.data = std::move(data);
                    video_track->push_frame(std::move(mf));
                    ts += 3000;
                }
            }
        }(video_track, video_files, ctx.get_executor(), video_ms,
                                  s_input_dir)));

    scope.spawn(stdexec::starts_on(
        sched,
        [](auto audio_track, auto audio_files, net::any_io_executor ex,
           std::chrono::milliseconds audio_ms,
           std::string input_dir) -> task<void> {
            net::steady_timer t(ex);
            uint32_t ts = 0;
            while (true) {
                for (size_t i = 0;; ++i) {
                    if (i == audio_files.size())
                        i = 0;
                    t.expires_after(audio_ms);
                    auto [ec] =
                        co_await t.async_wait(net::as_tuple(utils::use_sender));
                    if (ec)
                        co_return;

                    auto data =
                        read_file(input_dir + "/audio_frame/" + audio_files[i]);
                    if (data.empty())
                        co_return;

                    media_frame mf;
                    mf.kind = media_kind::audio;
                    mf.format = media_format::opus;
                    mf.timestamp = ts;
                    mf.data = std::move(data);
                    audio_track->push_frame(std::move(mf));
                    ts += 960;
                }
            }
        }(audio_track, audio_files, ctx.get_executor(), audio_ms,
                                  s_input_dir)));

    while (true) {
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
            co_await file_frame_session(ctx, ws);
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
    if (argc < 2) {
        std::cout << "Usage: file_frame <dir>\n"
                  << "  <dir>/video_frame/*.h264, <dir>/audio_frame/*.opus\n";
        return -1;
    }
    s_input_dir = argv[1];

    std::cout << std::unitbuf;
    net::io_context ctx;
    asiortc::set_logger(std::make_shared<logger_interface>(),
                        ctx.get_executor());
    exec::start_detached(
        stdexec::starts_on(utils::scheduler{ctx}, listener(ctx)));
    ctx.run();
}

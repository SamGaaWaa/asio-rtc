#include "asiortc/any_stream_track.hpp"
#include "asiortc/media_frame.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#error "Requires Boost.Asio"
#else
#include <boost/asio/io_context.hpp>
#include <boost/process/v2/popen.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/environment.hpp>
namespace asiortc {
namespace net = boost::asio;
}
namespace process = boost::process::v2;
#endif

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <optional>

using namespace asiortc;

namespace mpeg {} // namespace mpeg

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": "       \
                      << #cond << "\n";                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

static task<void> test_video_file(net::io_context &ctx,
                                  const std::string &file) {
    auto exe = boost::process::v2::environment::find_executable("ffmpeg");

    std::vector<std::string> args = {"-re", "-i",   file, "-map",   "0",
                                     "-c",  "copy", "-f", "mpegts", "pipe:1"};

    auto pipe = process::popen(ctx.get_executor(), exe, args);
    int frame_count = 0;

    any_stream_track track(
        std::move(pipe),
        [&](std::span<const uint8_t> data,
            std::size_t &consumed) noexcept -> std::optional<media_frame> {
            if (data.size() < 188) {
                return {};
            }
            consumed = 188;
            media_frame f;
            f.kind = media_kind::video;
            f.format = media_format::unknown;
            f.timestamp = 0;
            f.width = 0;
            f.height = 0;
            f.data = std::vector<uint8_t>(data.begin(), data.begin() + 188);
            frame_count++;
            return f;
        },
        media_kind::video, media_format::unknown);
    track.set_max_cache_size(4096);

    for (int i = 0; track.ready_state() != track_state::ended; ++i) {
        auto frame = co_await track.recv();
        if (!frame)
            break;
        if (i % 10 == 0)
            std::cout << "frame " << i + 1 << '\n';
    }

    std::cout << "  video file popen OK (" << frame_count << " frames)\n";
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cout << "Usage: any_stream_track_test [file]\n";
        return 1;
    }
    std::string file_path = argv[1];

    net::io_context ctx;

    exec::start_detached(stdexec::starts_on(
        asioice::utils::scheduler{ctx},
        [](net::io_context &ctx, const std::string &file_path) -> task<void> {
            try {
                co_await test_video_file(ctx, file_path);
                std::cout << "ALL TESTS PASSED\n";
            } catch (const std::exception &e) {
                std::cerr << "FAIL: exception: " << e.what() << '\n';
                std::exit(1);
            }
        }(ctx, file_path)));

    ctx.run();
}

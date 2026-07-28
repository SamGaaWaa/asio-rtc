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

#include "mpegts_demuxer.hpp"

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <optional>

using namespace asiortc;

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": "       \
                      << #cond << "\n";                                        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

static task<void> test_ts_file(net::io_context &ctx, const std::string &file) {
    auto exe = boost::process::v2::environment::find_executable("ffmpeg");

    std::vector<std::string> args = {"-i",   file, "-map",   "0",     "-c",
                                     "copy", "-f", "mpegts", "pipe:1"};

    auto pipe = process::popen(ctx.get_executor(), exe, args);

    detail::MPEGTSDemuxer demuxer;
    int video_frames = 0;
    int audio_frames = 0;
    int ts_packets = 0;

    any_stream_track track(
        std::move(pipe),
        [&](std::span<const uint8_t> data,
            std::size_t &consumed) noexcept -> std::optional<media_frame> {
            ts_packets++;
            auto r = demuxer.parse(data, consumed);
            if (r)
                return r;
            return {};
        },
        media_kind::video, media_format::unknown);
    track.set_max_cache_size(1 << 20);

    for (int i = 0; track.ready_state() != track_state::ended; ++i) {
        auto frame = co_await track.recv();
        if (!frame)
            break;

        if (frame->kind == media_kind::video) {
            video_frames++;
        } else {
            audio_frames++;
        }
    }

    for (;;) {
        std::size_t dummy = 0;
        auto frame = demuxer.parse({}, dummy);
        if (!frame)
            break;
        if (frame->kind == media_kind::video)
            video_frames++;
        else
            audio_frames++;
    }

    std::cout << "  TS demux: " << ts_packets << " packets, " << video_frames
              << " video frames, " << audio_frames << " audio frames\n";

    ASSERT(ts_packets > 0);
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
        [](net::io_context &ctx, const std::string &fp) -> task<void> {
            try {
                co_await test_ts_file(ctx, fp);
                std::cout << "ALL TESTS PASSED\n";
            } catch (const std::exception &e) {
                std::cerr << "FAIL: exception: " << e.what() << '\n';
                std::exit(1);
            }
        }(ctx, file_path)));

    ctx.run();
}

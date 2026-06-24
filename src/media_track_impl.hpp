#pragma once

#include <memory>
#include <string>

#include "asiortc/media_track.hpp"
#include "asioice/config.hpp"
#include "jitter_buffer.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#else
#include <asio/as_tuple.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
namespace asiortc {
namespace net = asio;
}
#endif

namespace asiortc {

struct media_track_impl : public media_track {
    media_track_impl(media_kind k, std::string track_id, net::io_context &ctx);

    media_kind kind() const noexcept override { return _kind; }
    std::string id() const noexcept override { return _id; }
    track_state ready_state() const noexcept override { return _state; }
    void stop() override;
    asioice::task<std::optional<media_frame>> recv() override;

    void push_frame(media_frame frame);

  private:
    media_kind _kind;
    std::string _id;
    track_state _state = track_state::live;
    net::io_context &_ctx;

    jitter_buffer _jitter;
};

} // namespace asiortc

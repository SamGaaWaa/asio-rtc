#pragma once

#include "asiortc/media_track.hpp"
#include "asiortc/detail/uuid.hpp"
#include "asioice/detail/async_queue.hpp"

#include <stdexcept>

namespace asiortc {

struct queue_track : media_track {
    queue_track(media_kind k, std::size_t max_cache_frame_count = 1024)
        : _kind(k), _id(utils::uuid()), _q(max_cache_frame_count) {}

    media_kind kind() const noexcept override { return _kind; }

    media_format format() const noexcept override { return _format; }

    const std::string &id() const noexcept override { return _id; }

    track_state ready_state() const noexcept override { return _state; }

    void stop() noexcept override {
        _q.close();
        _state = track_state::ended;
    }

    asiortc::task<std::optional<media_frame>> recv() override {
        auto frame = _q.try_pop();
        if (frame)
            co_return frame;
        co_return co_await _q.async_pop_stoppable();
    }

    void push_frame(media_frame frame) {
        if (_state == track_state::ended)
            return;
        if (_format == media_format::unknown)
            _format = frame.format;
        else if (_format != frame.format)
            throw std::runtime_error("queue_track: format mismatch");
        _q.push(std::move(frame));
    }

  protected:
    void set_format(media_format fmt) noexcept { _format = fmt; }

    void set_id(std::string id) noexcept { _id = std::move(id); }

    asioice::async_queue<media_frame> &queue() noexcept { return _q; }

    const asioice::async_queue<media_frame> &queue() const noexcept {
        return _q;
    }

  private:
    media_kind _kind;
    media_format _format = media_format::unknown;
    std::string _id;
    track_state _state = track_state::live;
    asioice::async_queue<media_frame> _q;
};

} // namespace asiortc
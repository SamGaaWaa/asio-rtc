#pragma once

#include "asiortc/media_track.hpp"
#include "asiortc/detail/uuid.hpp"
#include "asioice/detail/async_queue.hpp"

#include <span>
#include <stdexcept>

namespace asiortc {

struct queue_track : media_track {
    queue_track(media_description desc,
                std::size_t max_cache_frame_count = 1024)
        : _desc(std::move(desc)), _id(utils::uuid()),
          _q(max_cache_frame_count) {
        if (desc.format == media_format::unknown)
            throw std::invalid_argument{"desc.format == media_format::unknown"};
    }

    media_kind kind() const noexcept override {
        switch (_desc.format) {
        case media_format::opus:
            return media_kind::audio;
        case media_format::vp8:
            return media_kind::video;
        case media_format::h264:
            return media_kind::video;
        case media_format::vp9:
            return media_kind::video;
        default:
            return media_kind::video;
        }
    }

    media_description description() const noexcept override { return _desc; }

    const std::string &id() const noexcept override { return _id; }

    track_state ready_state() const noexcept override { return _state; }

    void stop() noexcept override {
        _q.close();
        _state = track_state::ended;
    }

    asiortc::task<std::vector<media_frame>>
    recv(std::span<const encode_target> layers) override {
        auto frame = _q.try_pop();
        if (frame) {
            std::vector<media_frame> result;
            result.push_back(std::move(*frame));
            co_return result;
        }
        auto f = co_await _q.async_pop_stoppable();
        if (!f)
            co_return std::vector<media_frame>{};
        std::vector<media_frame> result;
        result.push_back(std::move(*f));
        co_return result;
    }

    void push_frame(media_frame frame) {
        if (_state == track_state::ended)
            return;
        if (_desc.format != frame.format)
            throw std::runtime_error("queue_track: format mismatch");
        _q.push(std::move(frame));
    }

  protected:
    void set_description(const media_description &desc) noexcept {
        _desc = desc;
    }

    void set_id(std::string id) noexcept { _id = std::move(id); }

    asioice::async_queue<media_frame> &queue() noexcept { return _q; }

    const asioice::async_queue<media_frame> &queue() const noexcept {
        return _q;
    }

  private:
    media_description _desc;
    std::string _id;
    track_state _state = track_state::live;
    asioice::async_queue<media_frame> _q;
};

} // namespace asiortc

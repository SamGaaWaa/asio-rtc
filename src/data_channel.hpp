#pragma once

#include "asioice/config.hpp"
#include "asioice/basic_agent.hpp"
#include "asioice/data_channel.hpp"
#include "asioice/task.hpp"

#if ASIOICE_USE_BOOST_ASIO > 0
#include <boost/asio/ip/udp.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#else
#include <asio/ip/udp.hpp>
namespace asiortc {
namespace net = asio;
}
#endif

#include <memory>

namespace asiortc {

struct connection_impl;

struct data_channel {
    using asioice_dc =
        typename asioice::data_channel_manager<asioice::sctp::transport<
            asioice::ssl::dtls_transport<typename asioice::basic_agent<
                net::ip::udp::socket>::ice_transport_type>>>::data_channel;
    using message = asioice::data_channel_message;
    using options = asioice::data_channel_options;
    using state_t = typename asioice_dc::state_t;
    using data_channel_priority = asioice::impl::data_channel_priority;

    uint16_t stream_id() const noexcept { return _options.stream_id; }
    const std::string &label() const noexcept { return _label; }
    const std::string &protocol() const noexcept { return _options.protocol; }
    bool ordered() const noexcept { return _options.ordered; }
    state_t state() const noexcept {
        if (!_channel)
            return state_t::connecting;
        return _channel->state();
    }
    data_channel_priority priority() const noexcept {
        return _options.priority;
    }

    asioice::task<bool> open();

    auto send_text(std::string_view text) { return _channel->send_text(text); }
    auto send_binary(std::span<const uint8_t> data) {
        return _channel->send_binary(data);
    }
    auto read() { return _channel->read(); }
    void close() noexcept { _channel->close(); }

  private:
    friend struct connection_impl;

    data_channel(std::weak_ptr<connection_impl> conn, std::string label,
                 options opts)
        : _conn{std::move(conn)}, _label{std::move(label)},
          _options{std::move(opts)} {}

    std::weak_ptr<connection_impl> _conn;
    std::string _label;
    options _options;
    std::shared_ptr<asioice_dc> _channel;
};

} // namespace asiortc

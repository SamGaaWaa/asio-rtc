#include "data_channel.hpp"
#include "connection_impl.hpp"

namespace asiortc {

asiortc::task<bool> data_channel::open() {
    if (_channel)
        co_return true;
    auto conn = _conn.lock();
    if (!conn)
        co_return false;
    auto *manager = conn->data_channel_manager();
    if (!manager)
        co_return false;
    _channel = co_await manager->create_data_channel(_label, _options);
    if (!_channel)
        co_return false;
    _options = _channel->options();
    co_return true;
}

} // namespace asiortc
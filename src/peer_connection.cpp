#include "asiortc/peer_connection.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#include <asio/dispatch.hpp>
namespace asiortc {
namespace net = asio;
}
#else
#include <boost/asio/dispatch.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#endif

#include "candidate_convert.hpp"
#include "connection_impl.hpp"
#include "rtp_transceiver.hpp"
#include "data_channel.hpp"

#include <cassert>

namespace asiortc {

static void throw_if_nullptr(const auto &ptr, const char *msg) {
    if (ptr == nullptr)
        throw std::runtime_error(std::string{"null pointer: "} + msg);
}

void set_logger(std::shared_ptr<logger_interface> logger,
                net::any_io_executor ex) {
    net::dispatch(
        [l = std::move(logger)]() mutable { samlog::set_logger(std::move(l)); },
        std::move(ex), [] {});
}

peer_connection::peer_connection(net::any_io_executor ex, configuration cfg)
    : _impl{std::make_shared<connection_impl>(std::move(ex), std::move(cfg))} {}

peer_connection::executor_type peer_connection::get_executor() const {
    throw_if_nullptr(_impl, "get_executor");
    return _impl->get_executor();
}

void peer_connection::set_logger(std::shared_ptr<logger_interface> logger) {
    ::asiortc::set_logger(std::move(logger), get_executor());
}

task<std::unique_ptr<session_description_interface>> peer_connection::create_offer() {
    throw_if_nullptr(_impl, "create_offer");
    return _impl->create_offer();
}

task<std::unique_ptr<session_description_interface>> peer_connection::create_answer() {
    throw_if_nullptr(_impl, "create_answer");
    return _impl->create_answer();
}

task<void> peer_connection::set_local_description(std::unique_ptr<session_description_interface> desc) {
    throw_if_nullptr(_impl, "set_local_description");
    throw_if_nullptr(desc, "set_local_description: desc == nullptr");
    return _impl->set_local_description(std::unique_ptr<session_description>(static_cast<session_description*>(desc.release())));
}

task<void> peer_connection::set_remote_description(std::unique_ptr<session_description_interface> desc) {
    throw_if_nullptr(_impl, "set_remote_description");
    throw_if_nullptr(desc, "set_remote_description: desc == nullptr");
    return _impl->set_remote_description(std::unique_ptr<session_description>(static_cast<session_description*>(desc.release())));
}

const session_description_interface *peer_connection::local_description() const noexcept {
    throw_if_nullptr(_impl, "local_description");
    return _impl->local_description();
}

bool peer_connection::can_trickle_ice_candidates() const noexcept {
    throw_if_nullptr(_impl, "can_trickle_ice_candidates");
    return _impl->can_trickle_ice_candidates();
}

asiortc::task<void> peer_connection::add_ice_candidate(asiortc::candidate c) {
    throw_if_nullptr(_impl, "add_ice_candidate");
    if (!(co_await _impl->add_ice_candidate(to_ice(c))))
        throw std::runtime_error("add_ice_candidate failed");
}

asiortc::task<void> peer_connection::add_ice_candidate() {
    throw_if_nullptr(_impl, "add_ice_candidate");
    if (!(co_await _impl->add_ice_candidate()))
        throw std::runtime_error("add_ice_candidate failed");
}

void peer_connection::on_candidates(on_candidates_cb cb) {
    throw_if_nullptr(_impl, "on_candidates");
    _impl->on_candidates(
        [cb = std::move(cb)](std::span<const asioice::candidate> cands) {
            std::vector<candidate> out;
            out.reserve(cands.size());
            for (const auto &c : cands)
                out.push_back(from_ice(c));
            cb(std::move(out));
        });
}

ice_connection_state_t peer_connection::ice_connection_state() const noexcept {
    if (!_impl)
        return ice_connection_state_t::closed;
    return _impl->ice_connection_state();
}

asiortc::task<void> peer_connection::on_ice_connection_state_changed() {
    throw_if_nullptr(_impl, "on_ice_connection_state_changed");
    co_await _impl->on_ice_connection_state_changed();
}

ice_gathering_state_t peer_connection::ice_gathering_state() const noexcept {
    if (!_impl)
        return ice_gathering_state_t::init;
    return _impl->ice_gathering_state();
}

asiortc::task<void> peer_connection::on_ice_gathering_state_changed() {
    throw_if_nullptr(_impl, "on_ice_gathering_state_changed");
    co_await _impl->on_ice_gathering_state_changed();
}

connection_state_t peer_connection::connection_state() const noexcept {
    if (!_impl)
        return connection_state_t::closed;
    return _impl->connection_state();
}

asiortc::task<void> peer_connection::on_connection_state_changed() {
    throw_if_nullptr(_impl, "on_connection_state_changed");
    co_await _impl->on_connection_state_changed();
}

signaling_state_t peer_connection::signaling_state() const noexcept {
    if (!_impl)
        return signaling_state_t::stable;
    return _impl->signaling_state();
}

rtp_transceiver_interface
peer_connection::add_transceiver(std::shared_ptr<media_track> track,
                                 rtp_transceiver_init init) {
    throw_if_nullptr(_impl, "add_transceiver");
    auto tr = _impl->add_transceiver(std::move(track), std::move(init));
    rtp_transceiver_interface iface;
    iface._impl = std::move(tr);
    return iface;
}

rtp_transceiver_interface
peer_connection::add_transceiver(const media_description &desc,
                                 rtp_transceiver_init init) {
    throw_if_nullptr(_impl, "add_transceiver");
    auto tr = _impl->add_transceiver(desc, std::move(init));
    rtp_transceiver_interface iface;
    iface._impl = std::move(tr);
    return iface;
}

std::vector<rtp_transceiver_interface>
peer_connection::get_transceivers() const {
    throw_if_nullptr(_impl, "get_transceivers");
    std::vector<rtp_transceiver_interface> out;
    out.reserve(_impl->transceivers().size());
    for (const auto &tr : _impl->transceivers()) {
        rtp_transceiver_interface ti;
        ti._impl = tr;
        out.push_back(std::move(ti));
    }
    return out;
}

rtp_sender_interface
peer_connection::add_track(std::shared_ptr<media_track> track,
                           std::vector<std::string> streams) {
    throw_if_nullptr(_impl, "add_track");
    auto sender = _impl->add_track(std::move(track), std::move(streams));
    rtp_sender_interface iface;
    iface._impl = std::move(sender);
    return iface;
}

void peer_connection::replace_track(const rtp_sender_interface &sender,
                                    std::shared_ptr<media_track> track) {
    throw_if_nullptr(_impl, "replace_track");
    if (!sender || !track)
        throw std::runtime_error{"replace_track: null sender or track"};
    const auto &existing = sender._impl->track();
    if (existing && existing->kind() != track->kind())
        throw std::runtime_error{"replace_track: track kind mismatch"};
    sender._impl->set_track(std::move(track));
}

data_channel_interface
peer_connection::create_data_channel(std::string label,
                                     data_channel_options options) {
    throw_if_nullptr(_impl, "create_data_channel");
    auto dc = _impl->create_data_channel(
        std::move(label),
        asiortc::data_channel::options{
            .ordered = options.ordered,
            .max_packet_life_time = options.max_packet_life_time,
            .max_retransmits = options.max_retransmits,
            .protocol = std::move(options.protocol),
            .negotiated = options.negotiated,
            .stream_id = options.stream_id,
            .priority = [](auto pri) {
                switch (pri) {
                case data_channel_priority::very_low:
                    return asiortc::data_channel::data_channel_priority::
                        very_low;
                case data_channel_priority::low:
                    return asiortc::data_channel::data_channel_priority::low;
                case data_channel_priority::medium:
                    return asiortc::data_channel::data_channel_priority::medium;
                case data_channel_priority::high:
                    return asiortc::data_channel::data_channel_priority::high;
                }
                std::unreachable();
            }(options.priority)});
    data_channel_interface iface;
    iface._impl = std::move(dc);
    return iface;
}

void peer_connection::on_track(on_track_cb cb) {
    throw_if_nullptr(_impl, "on_track");
    _impl->on_track([cb = std::move(cb)](std::shared_ptr<rtp_receiver> recv,
                                         std::shared_ptr<media_track> track,
                                         std::vector<std::string> msids,
                                         std::shared_ptr<rtp_transceiver> tr) {
        rtp_receiver_interface ri;
        ri._impl = std::move(recv);
        rtp_transceiver_interface ti;
        ti._impl = std::move(tr);
        cb(std::move(ri), std::move(track), std::move(msids), std::move(ti));
    });
}

void peer_connection::on_data_channel(on_data_channel_cb cb) {
    throw_if_nullptr(_impl, "on_data_channel");
    _impl->on_remote_channel([cb = std::move(cb)](auto dc) {
        data_channel_interface iface;
        iface._impl = std::move(dc);
        cb(std::move(iface));
    });
}

rtc_stats_report peer_connection::get_stats() const {
    throw_if_nullptr(_impl, "get_stats");
    return _impl->get_stats();
}

detail::packet_stream &peer_connection::rtp_send_buffer() noexcept {
    assert(_impl);
    return _impl->rtp_send_buffer();
}

void peer_connection::rewrite_rtp_packet(
    std::span<uint8_t> data, const rtp_sender_interface &sender) noexcept {
    assert(_impl && sender);
    _impl->rewrite_rtp_packet(data, *sender._impl);
}

std::span<uint8_t>
peer_connection::encrypt_rtp(std::span<const uint8_t> data,
                             std::span<uint8_t> buf) noexcept {
    assert(_impl);
    return _impl->encrypt_rtp(data, buf);
}

void peer_connection::update_sender_status_after_send_rtp(
    std::size_t octet, std::size_t encrypted,
    const rtp_sender_interface &sender) noexcept {
    assert(_impl && sender);
    _impl->update_sender_status_after_send_rtp(octet, encrypted, *sender._impl);
}

void peer_connection::close() noexcept {
    if (_impl)
        _impl->close();
}

// data channel impl
uint16_t data_channel_interface::stream_id() const noexcept {
    assert(_impl != nullptr);
    return _impl->stream_id();
}

const std::string &data_channel_interface::label() const noexcept {
    assert(_impl != nullptr);
    return _impl->label();
}

const std::string &data_channel_interface::protocol() const noexcept {
    assert(_impl != nullptr);
    return _impl->protocol();
}

bool data_channel_interface::ordered() const noexcept {
    assert(_impl != nullptr);
    return _impl->ordered();
}

data_channel_ready_state_t data_channel_interface::state() const noexcept {
    assert(_impl != nullptr);
    switch (_impl->state()) {
    case asiortc::data_channel::ready_state_t::connecting:
        return data_channel_ready_state_t::connecting;
    case asiortc::data_channel::ready_state_t::open:
        return data_channel_ready_state_t::open;
    case asiortc::data_channel::ready_state_t::closing:
        return data_channel_ready_state_t::closing;
    case asiortc::data_channel::ready_state_t::closed:
        return data_channel_ready_state_t::closed;
    }
    std::unreachable();
}

data_channel_priority data_channel_interface::priority() const noexcept {
    assert(_impl != nullptr);
    switch (_impl->priority()) {
    case asiortc::data_channel::data_channel_priority::very_low:
        return data_channel_priority::very_low;
    case asiortc::data_channel::data_channel_priority::low:
        return data_channel_priority::low;
    case asiortc::data_channel::data_channel_priority::medium:
        return data_channel_priority::medium;
    case asiortc::data_channel::data_channel_priority::high:
        return data_channel_priority::high;
    }
    std::unreachable();
}

asiortc::task<bool> data_channel_interface::open() {
    assert(_impl != nullptr);
    return _impl->open();
}

asiortc::task<bool> data_channel_interface::send(std::string_view text) {
    assert(_impl != nullptr);
    co_return co_await _impl->send_text(text);
}

asiortc::task<bool>
data_channel_interface::send(std::span<const uint8_t> data) {
    assert(_impl != nullptr);
    co_return co_await _impl->send_binary(data);
}

asiortc::task<data_channel_message> data_channel_interface::read() {
    assert(_impl != nullptr);
    auto msg = co_await _impl->read();
    co_return data_channel_message(std::move(msg.data), msg.binary);
}

void data_channel_interface::close() noexcept {
    if (_impl)
        _impl->close();
}

} // namespace asiortc

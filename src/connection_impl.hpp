#pragma once

#include <exec/any_sender_of.hpp>

#include "asioice/basic_agent.hpp"
#include "asioice/config.hpp"
#include "asioice/data_channel.hpp"
#include "asioice/detail/property.hpp"
#include "asioice/dtls_transport.hpp"
#include "asioice/ssl/dtls_config.hpp"
#include "asioice/task.hpp"
#include "asiortc/connection.hpp"
#include "sdp.hpp"
#include "srtp_transport.hpp"

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

#include <boost/compat/move_only_function.hpp>
#include <functional>
#include <optional>

namespace asiortc {

template <class T>
struct any_sender_trait {
  using type = exec::any_sender<exec::any_receiver<
      stdexec::completion_signatures<stdexec::set_value_t(T),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>>>;
};

template <>
struct any_sender_trait<void> {
  using type = exec::any_sender<exec::any_receiver<
      stdexec::completion_signatures<stdexec::set_value_t(),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>>>;
};

template <class T>
using any_sender = typename any_sender_trait<T>::type;

struct connection_impl : std::enable_shared_from_this<connection_impl> {
  using agent_type = asioice::basic_agent<net::ip::udp::socket>;
  using executor_type = typename agent_type::executor_type;
  using ice_transport_type = typename agent_type::ice_transport_type;
  using dtls_transport_type = asioice::ssl::dtls_transport<ice_transport_type>;
  using srtp_transport_type = asiortc::srtp_transport<ice_transport_type>;
  using sctp_transport_type = asioice::sctp::transport<dtls_transport_type>;
  using datachannel_manager_type =
      asioice::data_channel_manager<sctp_transport_type>;
  using data_channel_type = typename datachannel_manager_type::data_channel;

  using on_data_channel_cb =
      std::function<void(std::shared_ptr<data_channel_type>)>;
  using on_candidates_cb = boost::compat::move_only_function<void(
      std::span<const asioice::candidate>)>;

  connection_impl(executor_type ex, asiortc::configuration cfg = {});

  connection_impl(const connection_impl&) = delete;
  connection_impl(connection_impl&&) = delete;
  connection_impl& operator=(const connection_impl&) = delete;
  connection_impl& operator=(connection_impl&&) = delete;

  executor_type get_executor() const { return _executor; }

  ice_connection_state_t ice_connection_state() const noexcept {
    return to_ice_connection_state(_agent.state());
  }

  auto on_ice_connection_state_changed() noexcept {
    return _agent.on_state_change() |
           stdexec::then([this] { return ice_connection_state(); });
  }

  ice_gathering_state_t ice_gathering_state() const noexcept {
    return _gathering_state.get();
  }

  auto on_ice_gathering_state_changed() noexcept {
    return _gathering_state.on_change() |
           stdexec::continues_on(asioice::utils::scheduler{_executor});
  }

  const auto& sctp() const noexcept { return _sctp_transport; }

  auto& sctp() noexcept { return _sctp_transport; }

  signaling_state_t signaling_state() const noexcept {
    return _signaling_state.get();
  }

  asioice::task<session_description> create_offer();
  asioice::task<session_description> create_answer();

  asioice::task<void> set_local_description(session_description desc);
  asioice::task<void> set_remote_description(session_description desc);

  auto add_ice_candidate(asioice::candidate c) {
    return _agent.add_remote_candidate(std::move(c));
  }
  auto add_ice_candidate() { return _agent.add_remote_candidate(); }
  void on_candidates(on_candidates_cb cb);

  asioice::task<void> close();
  void on_remote_channel(on_data_channel_cb cb);

 private:
  asioice::task<void> apply_descriptions();
  void start_gathering();
  void start_connecting();
  asioice::task<void> do_connect();
  static ice_connection_state_t to_ice_connection_state(
      asioice::agent_state_t s) noexcept;

  executor_type _executor;
  stdexec::counting_scope _scope{};

  agent_type _agent;
  asioice::ssl::dtls_certificate _cert{};
  bool _roles_set = false;
  std::shared_ptr<ice_transport_type> _ice_transport{};
  std::shared_ptr<dtls_transport_type> _dtls_transport{};
  std::shared_ptr<srtp_transport_type> _srtp_transport{};
  std::shared_ptr<sctp_transport_type> _sctp_transport{};
  std::optional<datachannel_manager_type> _data_channel_manager{};

  std::optional<session_description> _local_desc{};
  std::optional<session_description> _remote_desc{};
  std::optional<session_description> _pending_local_desc{};
  std::optional<session_description> _pending_remote_desc{};
  asioice::utils::property<ice_gathering_state_t> _gathering_state{
      ice_gathering_state_t::init};
  std::optional<any_sender<void>> _gathering_task{};
  asioice::utils::property<signaling_state_t> _signaling_state{
      signaling_state_t::stable};
  std::optional<any_sender<void>> _connecting_task{};

  on_data_channel_cb _on_remote_channel_cb;

  // state monitoring
  std::optional<any_sender<void>> _ice_connection_state_watcher{};
};

}  // namespace asiortc

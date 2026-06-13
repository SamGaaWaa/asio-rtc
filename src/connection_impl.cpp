#include "connection_impl.hpp"

#include <chrono>
#include <exec/async_scope.hpp>
#include <exec/finally.hpp>
#include <stdexcept>
#include <string>

#include "asioice/agent_config.hpp"
#include "asioice/candidate.hpp"
#include "asioice/detail/detached_with_data.hpp"
#include "asioice/detail/ignore.hpp"
#include "asioice/detail/on_scope_empty.hpp"
#include "asioice/detail/stop_when.hpp"
#include "asioice/detail/string_utils.hpp"
#include "asioice/sctp_transport.hpp"

namespace asiortc {

static std::string to_string(signaling_state_t s) {
  switch (s) {
    case signaling_state_t::stable:
      return "stable";
    case signaling_state_t::have_local_offer:
      return "have_local_offer";
    case signaling_state_t::have_remote_offer:
      return "have_remote_offer";
    case signaling_state_t::have_local_pranswer:
      return "have_local_pranswer";
    case signaling_state_t::have_remote_pranswer:
      return "have_remote_pranswer";
    case signaling_state_t::closed:
      return "closed";
  }
}

static std::string ice_ufrag_from(const session_description& sdp) {
  if (!sdp.ice_ufrag.empty())
    return sdp.ice_ufrag;
  if (!sdp.medias.empty())
    return sdp.medias[0].ice_ufrag;
  return {};
}

static std::string ice_pwd_from(const session_description& sdp) {
  if (!sdp.ice_pwd.empty())
    return sdp.ice_pwd;
  if (!sdp.medias.empty())
    return sdp.medias[0].ice_pwd;
  return {};
}

static std::string fingerprint_from(const session_description& sdp) {
  if (!sdp.fingerprint.empty())
    return sdp.fingerprint;
  if (!sdp.medias.empty())
    return sdp.medias[0].fingerprint;
  return {};
}

static std::string setup_from(const session_description& sdp) {
  if (!sdp.setup.empty())
    return sdp.setup;
  if (!sdp.medias.empty())
    return sdp.medias[0].setup;
  return {};
}

static std::vector<std::string> candidates_from(
    const session_description& sdp) {
  std::vector<std::string> result = sdp.candidates;
  if (!sdp.medias.empty())
    result.insert(result.end(), sdp.medias[0].candidates.begin(),
                  sdp.medias[0].candidates.end());
  return result;
}

static asioice::agent_config get_agent_config(asiortc::configuration&& cfg) {
  asioice::agent_config ret{};
  ret.username = asioice::utils::random_string(8);
  ret.password = asioice::utils::random_string(28);
  ret.ice_controlling = false;
  ret.use_loopback = true;
  ret.transport_policy = cfg.ice_transport_policy == ice_transport_policy_t::all
                             ? asioice::transport_policy::ALL
                             : asioice::transport_policy::RELAY;
  ret.enable_mdns = true;
  // TODO: parse STUN server and TURN server
  return ret;
}

connection_impl::connection_impl(connection_impl::executor_type ex,
                                 asiortc::configuration cfg)
    : _executor{std::move(ex)},
      _agent{_executor, get_agent_config(std::move(cfg))} {}

asioice::task<void> connection_impl::apply_descriptions() {
  if (_roles_set || !_local_desc || !_remote_desc)
    co_return;
  _roles_set = true;

  auto ufrag = ice_ufrag_from(*_remote_desc);
  auto pwd = ice_pwd_from(*_remote_desc);
  if (!ufrag.empty())
    _agent.set_remote_username(std::move(ufrag));
  if (!pwd.empty())
    _agent.set_remote_password(std::move(pwd));

  _ice_transport = _agent.create_ice_transport(1);
  _dtls_transport =
      std::make_shared<dtls_transport_type>(_ice_transport, std::move(_cert));

  const auto fp_str = fingerprint_from(*_remote_desc);
  auto remote_fp = asioice::ssl::fingerprint::from_sdp(fp_str);
  if (remote_fp)
    _dtls_transport->set_expected_remote_fingerprint(std::move(*remote_fp));

  auto candidate_lines = candidates_from(*_remote_desc);
  if (candidate_lines.empty())
    co_return;
  exec::async_scope scope;
  for (const auto& line : candidate_lines) {
    auto c = asioice::candidate::from_sdp(line);
    if (c) {
      scope.spawn(_agent.add_remote_candidate(std::move(*c)) |
                  asioice::utils::ignore());
    }
  }
  co_await (asioice::utils::on_scope_empty(scope) |
            stdexec::continues_on(asioice::utils::scheduler{_executor}));
}

void connection_impl::start_gathering() {
  if (_gathering_task)
    return;
  _gathering_state = ice_gathering_state_t::gathering;
  _gathering_task = stdexec::spawn_future(
      stdexec::starts_on(stdexec::inline_scheduler{},
                         exec::finally(this->_agent.gather_candidates(),
                                       stdexec::just() | stdexec::then([this] {
                                         this->_gathering_task.reset();
                                         if (_gathering_state ==
                                             ice_gathering_state_t::gathering)
                                           _gathering_state =
                                               ice_gathering_state_t::complete;
                                       }))),
      _scope.get_token());
}

void connection_impl::start_connecting() {
  if (_local_desc && _remote_desc && !_connecting_task) {
    _connecting_task = stdexec::spawn_future(
        stdexec::starts_on(
            stdexec::inline_scheduler{},
            exec::finally(this->do_connect(),
                          stdexec::just() | stdexec::then([this] {
                            this->_connecting_task.reset();
                          }))),
        _scope.get_token());
  }
}

asioice::task<void> connection_impl::do_connect() {
  if (!this->_agent.config().trickle_ice) {
    while (_gathering_state != ice_gathering_state_t::complete)
      co_await (_gathering_state.on_change() |
                stdexec::continues_on(asioice::utils::scheduler{_executor}));
  }
  if (!co_await _agent.connect()) {
    ICE_IN_DEBUG {
      std::cerr << "ICE connect failed\n";
    }
    co_return;
  }

  auto setup_str = setup_from(*_remote_desc);
  bool we_are_active = (setup_str != "active");
  auto dtls_role = we_are_active ? dtls_transport_type::handshake_type::client
                                 : dtls_transport_type::handshake_type::server;
  auto hs_ec = co_await _dtls_transport->async_handshake(dtls_role);
  if (hs_ec) {
    ICE_IN_DEBUG {
      std::cerr << "DTLS handshake failed: " << hs_ec.message() << '\n';
    }
    co_return;
  }

  _sctp_transport = std::make_shared<sctp_transport_type>(_dtls_transport);
  _sctp_transport->start();

  bool sctp_ok = false;
  if (we_are_active) {
    sctp_ok = co_await _sctp_transport->accept();
  } else {
    sctp_ok = co_await _sctp_transport->connect();
  }
  if (!sctp_ok) {
    ICE_IN_DEBUG {
      std::cerr << "SCTP setup failed\n";
    }
    co_return;
  }

  _data_channel_manager.emplace(_sctp_transport, we_are_active);
  if (_on_remote_channel_cb)
    _data_channel_manager->on_remote_channel(_on_remote_channel_cb);
  _data_channel_manager->start();
}

asioice::task<void> connection_impl::set_local_description(
    session_description desc) {
  switch (_signaling_state.get()) {
    case signaling_state_t::have_local_offer:
    case signaling_state_t::have_local_pranswer:
    case signaling_state_t::have_remote_pranswer:
      throw std::logic_error{"set_local_description: invalid state: " +
                             to_string(_signaling_state.get())};
    case signaling_state_t::stable:
    case signaling_state_t::closed:
      if (desc.type != "offer")
        throw std::logic_error{"set_local_description: desc.type != \"offer\""};
      break;
    case signaling_state_t::have_remote_offer:
      if (desc.type != "answer")
        throw std::logic_error{
            "set_local_description: desc.type != \"answer\""};
      break;
  }
  bool is_offer = (desc.type == "offer");
  _local_desc = std::move(desc);
  if (is_offer)
    _signaling_state = signaling_state_t::have_local_offer;
  else
    _signaling_state = signaling_state_t::have_local_pranswer;

  co_await apply_descriptions();
  start_gathering();
  start_connecting();
}

asioice::task<void> connection_impl::set_remote_description(
    session_description desc) {
  switch (_signaling_state.get()) {
    case signaling_state_t::have_remote_offer:
    case signaling_state_t::have_local_pranswer:
    case signaling_state_t::have_remote_pranswer:
      throw std::logic_error{"set_remote_description: invalid state: " +
                             to_string(_signaling_state.get())};
    case signaling_state_t::stable:
    case signaling_state_t::closed:
      if (desc.type != "offer")
        throw std::logic_error{
            "set_remote_description: desc.type != \"offer\""};
      break;
    case signaling_state_t::have_local_offer:
      if (desc.type != "answer")
        throw std::logic_error{
            "set_remote_description: desc.type != \"answer\""};
      break;
  }
  bool is_offer = (desc.type == "offer");
  _remote_desc = std::move(desc);
  if (is_offer)
    _signaling_state = signaling_state_t::have_remote_offer;
  else
    _signaling_state = signaling_state_t::have_remote_pranswer;

  co_await apply_descriptions();
  start_connecting();
}

void connection_impl::on_candidates(connection_impl::on_candidates_cb cb) {
  _agent.on_local_candidates(std::move(cb));
}

asioice::task<session_description> connection_impl::create_offer() {
  auto fp = _cert.get_fingerprint(asioice::ssl::hash_algorithm::sha256);

  session_description offer;
  offer.type = "offer";
  offer.version = 0;
  offer.origin.username = "-";
  offer.origin.session_id = 0;
  offer.origin.session_version = 0;
  offer.origin.nettype = "IN";
  offer.origin.addrtype = "IP4";
  offer.origin.addr = "0.0.0.0";
  offer.session_name = "-";
  offer.timing.start = 0;
  offer.timing.stop = 0;
  offer.bundle_groups = {"0"};
  offer.ice_ufrag = _agent.local_username();
  offer.ice_pwd = _agent.local_password();
  offer.fingerprint = fp.to_sdp();
  offer.setup = "actpass";

  sdp_media app;
  app.mid = "0";
  app.media_type = "application";
  app.port = 9;
  app.proto = "UDP/DTLS/SCTP";
  app.conn_nettype = "IN";
  app.conn_addrtype = "IP4";
  app.conn_addr = "0.0.0.0";
  app.direction = sdp_direction::sendrecv;
  app.sctpmap = "webrtc-datachannel";
  app.sctp_port = 5000;
  offer.medias.push_back(std::move(app));

  co_return offer;
}

asioice::task<session_description> connection_impl::create_answer() {
  if (!_remote_desc ||
      _signaling_state != signaling_state_t::have_remote_offer) {
    throw std::runtime_error("set_remote_description must be called first");
  }

  auto fp = _cert.get_fingerprint(asioice::ssl::hash_algorithm::sha256);
  const auto remote_setup = setup_from(_remote_desc.value());
  const bool we_are_active = (remote_setup != "active");

  session_description answer;
  answer.type = "answer";
  answer.version = 0;
  answer.origin.username = "-";
  answer.origin.session_id = 0;
  answer.origin.session_version = 0;
  answer.origin.nettype = "IN";
  answer.origin.addrtype = "IP4";
  answer.origin.addr = "0.0.0.0";
  answer.session_name = "-";
  answer.timing.start = 0;
  answer.timing.stop = 0;
  answer.bundle_groups = _remote_desc->bundle_groups.empty()
                             ? std::vector<std::string>{"0"}
                             : _remote_desc->bundle_groups;
  answer.ice_ufrag = _agent.local_username();
  answer.ice_pwd = _agent.local_password();
  answer.fingerprint = fp.to_sdp();
  answer.setup = we_are_active ? "active" : "passive";

  sdp_media app;
  app.mid = "0";
  app.media_type = "application";
  app.port = 9;
  app.proto = "UDP/DTLS/SCTP";
  app.conn_nettype = "IN";
  app.conn_addrtype = "IP4";
  app.conn_addr = "0.0.0.0";
  app.direction = sdp_direction::sendrecv;

  if (!_remote_desc->medias.empty()) {
    const auto& rm = _remote_desc->medias[0];
    app.sctpmap = rm.sctpmap;
    app.sctp_port = rm.sctp_port;
  }
  answer.medias.push_back(std::move(app));

  co_return answer;
}

asioice::task<void> connection_impl::close() {
  // _scope.request_stop();
  _agent.close();
  if (_data_channel_manager)
    _data_channel_manager->stop();
  _ice_transport.reset();
  _dtls_transport.reset();
  _srtp_transport.reset();
  _sctp_transport.reset();

  _local_desc.reset();
  _remote_desc.reset();
  _pending_local_desc.reset();
  _pending_remote_desc.reset();
  _gathering_task.reset();
  _connecting_task.reset();

  _on_remote_channel_cb = nullptr;

  // asioice::utils::detached_with_data(
  //     stdexec::continues_on(
  //         stdexec::starts_on(
  //             stdexec::inline_scheduler{},
  //             _scope.join() |
  //             stdexec::continues_on(asioice::utils::scheduler{_executor})
  //         ),
  //         asioice::utils::scheduler{_executor}
  //     ),
  //     this->shared_from_this()
  // );

  co_await (stdexec::starts_on(stdexec::inline_scheduler{}, _scope.join()) |
            stdexec::continues_on(asioice::utils::scheduler{_executor}));
}

void connection_impl::on_remote_channel(on_data_channel_cb cb) {
  _on_remote_channel_cb = std::move(cb);
  if (_data_channel_manager) {
    _data_channel_manager->on_remote_channel(_on_remote_channel_cb);
  }
}

ice_connection_state_t connection_impl::to_ice_connection_state(
    asioice::agent_state_t s) noexcept {
  switch (s) {
    case asioice::agent_state_t::INIT:
    case asioice::agent_state_t::GATHERING:
      return ice_connection_state_t::init;
    case asioice::agent_state_t::CONNECTING:
      return ice_connection_state_t::checking;
    case asioice::agent_state_t::CONNECTED:
      return ice_connection_state_t::connected;
    case asioice::agent_state_t::CLOSED:
      return ice_connection_state_t::failed;
  }
}

}  // namespace asiortc

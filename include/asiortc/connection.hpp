#pragma once

#include <memory>

#include "asiortc/configuration.hpp"

namespace asiortc {

enum struct ice_connection_state_t : char {
    init,
    checking,
    connected,
    failed,
    closed
};

enum struct ice_gathering_state_t : char { init, gathering, complete };

enum struct signaling_state_t : char {
    stable,
    have_local_offer,
    have_remote_offer,
    have_local_pranswer,
    have_remote_pranswer,
    closed
};

enum struct connection_state_t : char {
    init,
    connecting,
    connected,
    disconnected,
    failed,
    closed
};

struct connection {

  private:
    struct connection_impl;

    std::shared_ptr<connection_impl> _impl;
};

} // namespace asiortc
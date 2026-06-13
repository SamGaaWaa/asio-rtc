#pragma once

#include <string>
#include <vector>
#include <optional>

namespace asiortc {
    
enum bundle_policy_t: char {
    balanced,
    max_compat,
    max_bundle
};

struct credential {
    std::string username;
    std::string password;
};

struct ice_server {
    std::string url;
    std::optional<asiortc::credential> credential{}; 
};

enum ice_transport_policy_t: char {
    all, relay
};

struct configuration {
    bundle_policy_t bundle_policy{bundle_policy_t::max_bundle};
    std::vector<ice_server> ice_servers;
    ice_transport_policy_t ice_transport_policy{ice_transport_policy_t::all};
};

} // namespace asiortc

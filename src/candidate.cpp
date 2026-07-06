#include "candidate_convert.hpp"
#include "asiortc/config.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#include <asio/ip/address.hpp>
namespace asiortc {
namespace net = asio;
}
#else
#include <boost/asio/ip/address.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#endif

namespace asiortc {

constexpr bool is_mdns(std::string_view host) {
    return host.ends_with(".local");
}

asioice::candidate to_ice(const asiortc::candidate &c) {
    asioice::candidate ice;
    ice.foundation = c.foundation;
    ice.component = c.component;
    ice.transport_type = c.transport_type;
    ice.priority = c.priority;
    ice.tcptype = c.tcptype;
    ice.generation = c.generation;

    switch (c.type) {
    case asiortc::candidate_type::host:
        ice.type = asioice::candidate_type::host;
        break;
    case asiortc::candidate_type::srflx:
        ice.type = asioice::candidate_type::srflx;
        break;
    case asiortc::candidate_type::prflx:
        ice.type = asioice::candidate_type::prflx;
        break;
    case asiortc::candidate_type::relay:
        ice.type = asioice::candidate_type::relay;
        break;
    }

    boost::system::error_code ec;
    if (!c.address.empty()) {
        if (is_mdns(c.address)) {
            ice.mdns_host = c.address;
            ice.endpoint = asioice::endpoint{net::ip::address{}, c.port};
        } else {
            ice.endpoint =
                asioice::endpoint{net::ip::make_address(c.address, ec), c.port};
            if (ec)
                throw std::runtime_error("invalid address: " + c.address +
                                         " (" + ec.message() + ")");
        }
    }

    if (c.related_address && !c.related_address->empty()) {
        if (is_mdns(*c.related_address)) {
            ice.mdns_related = *c.related_address;
            ice.related = asioice::endpoint{net::ip::address{},
                                            c.related_port.value_or(0)};
        } else {
            ice.related =
                asioice::endpoint{net::ip::make_address(*c.related_address, ec),
                                  c.related_port.value_or(0)};
            if (ec)
                throw std::runtime_error("invalid related address: " +
                                         c.related_address.value_or("") + " (" +
                                         ec.message() + ")");
        }
    }

    return ice;
}

asiortc::candidate from_ice(const asioice::candidate &ice) {
    asiortc::candidate out;
    out.foundation = ice.foundation;
    out.component = ice.component;
    out.transport_type = ice.transport_type;
    out.priority = ice.priority;
    out.tcptype = ice.tcptype;
    out.generation = ice.generation;

    switch (ice.type) {
    case asioice::candidate_type::host:
        out.type = asiortc::candidate_type::host;
        break;
    case asioice::candidate_type::srflx:
        out.type = asiortc::candidate_type::srflx;
        break;
    case asioice::candidate_type::prflx:
        out.type = asiortc::candidate_type::prflx;
        break;
    case asioice::candidate_type::relay:
        out.type = asiortc::candidate_type::relay;
        break;
    }

    if (!ice.mdns_host.empty()) {
        out.address = ice.mdns_host;
        out.port = ice.endpoint.port();
    } else {
        out.address = ice.endpoint.address().to_string();
        out.port = ice.endpoint.port();
    }

    if (!ice.mdns_related.empty()) {
        out.related_address = ice.mdns_related;
        out.related_port = ice.endpoint.port();
    } else if (ice.related) {
        out.related_address = ice.related->address().to_string();
        out.related_port = ice.related->port();
    }

    return out;
}

std::string candidate::to_sdp() const { return to_ice(*this).to_sdp(); }

std::optional<candidate> candidate::from_sdp(std::string_view sdp) noexcept {
    auto ice = asioice::candidate::from_sdp(sdp);
    if (!ice)
        return std::nullopt;
    return from_ice(*ice);
}

} // namespace asiortc

#pragma once

#include "asiortc/config.hpp"

#if ASIORTC_USE_STANDALONE_ASIO
#define ASIOICE_USE_BOOST_ASIO 0
#include <asio/any_io_executor.hpp>
namespace asiortc {
namespace net = asio;
}
#else
#define ASIOICE_USE_BOOST_ASIO 1
#include <boost/asio/any_io_executor.hpp>
namespace asiortc {
namespace net = boost::asio;
}
#endif

#include "asioice/detail/asio2exec.hpp"

namespace asiortc::utils {

using asioice::utils::scheduler;
using asioice::utils::use_sender;

} // namespace asiortc::utils
#include "asiortc/detail/uuid.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace asiortc::utils {

std::string uuid() {
    static thread_local boost::uuids::random_generator generator;
    boost::uuids::uuid uuid = generator();
    return boost::uuids::to_string(uuid);
}

} // namespace asiortc::utils
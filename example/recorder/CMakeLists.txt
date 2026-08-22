find_package(Boost COMPONENTS process filesystem CONFIG REQUIRED)

add_executable(recorder main.cpp)
target_include_directories(recorder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party)
target_link_libraries(recorder PRIVATE
    asiortc_test_flags
    OpenSSL::SSL OpenSSL::Crypto srtp3
    Boost::process Boost::filesystem)

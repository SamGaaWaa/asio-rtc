add_executable(datachannel_benchmark main.cpp)
target_include_directories(datachannel_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/src)
target_link_libraries(datachannel_benchmark PRIVATE
    asiortc_test_flags
    OpenSSL::SSL OpenSSL::Crypto srtp3)

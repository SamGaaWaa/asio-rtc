add_executable(transceiver_test main.cpp)
target_link_libraries(transceiver_test PRIVATE asiortc_test_flags
                       OpenSSL::SSL OpenSSL::Crypto srtp3)
target_include_directories(transceiver_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party)

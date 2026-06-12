add_executable(srtp_example main.cpp)
target_link_libraries(srtp_example PRIVATE asiortc_test_flags
                       OpenSSL::SSL OpenSSL::Crypto srtp3)
target_include_directories(srtp_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party)

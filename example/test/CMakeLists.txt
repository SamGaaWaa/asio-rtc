add_executable(negotiation_test main.cpp)
target_include_directories(negotiation_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party)
target_link_libraries(negotiation_test PRIVATE
    asiortc_test_flags
    OpenSSL::SSL OpenSSL::Crypto srtp3)

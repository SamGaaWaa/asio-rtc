add_executable(sfu_example main.cpp)
target_include_directories(sfu_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party)
target_link_libraries(sfu_example PRIVATE
    asiortc_test_flags
    OpenSSL::SSL OpenSSL::Crypto srtp3)

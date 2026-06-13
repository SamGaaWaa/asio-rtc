cmake_minimum_required(VERSION 3.22)

add_executable(chat_example main.cpp)
target_link_libraries(chat_example PRIVATE asiortc_test_flags
                       asioice exsctp OpenSSL::SSL OpenSSL::Crypto srtp3)
target_include_directories(chat_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party)

find_package(Boost 1.89 CONFIG REQUIRED COMPONENTS filesystem process)
add_executable(ffmpeg_track_example main.cpp)
target_link_libraries(ffmpeg_track_example PRIVATE asiortc_test_flags
                       OpenSSL::SSL OpenSSL::Crypto srtp3 Boost::filesystem Boost::process)
target_include_directories(ffmpeg_track_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party)

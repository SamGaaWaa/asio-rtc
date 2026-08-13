find_package(Boost COMPONENTS process filesystem CONFIG REQUIRED)

add_executable(ffmpeg_exe main.cpp)
target_include_directories(ffmpeg_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party)
target_link_libraries(ffmpeg_exe PRIVATE
    asiortc_test_flags
    OpenSSL::SSL OpenSSL::Crypto srtp3
    Boost::process Boost::filesystem)

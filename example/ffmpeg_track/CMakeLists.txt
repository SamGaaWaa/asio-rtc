set(FFMPEG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg)

add_executable(ffmpeg_track_example
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg/codecs/vpx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg/codecs/h264.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg/codecs/vp9.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg/codecs/opus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/codecs/g711.cpp
)
target_include_directories(ffmpeg_track_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg
    ${FFMPEG_ROOT}/include)
target_link_libraries(ffmpeg_track_example PRIVATE asiortc_test_flags
                       OpenSSL::SSL OpenSSL::Crypto srtp3
                       /usr/lib/x86_64-linux-gnu/libavcodec.so.60
                       /usr/lib/x86_64-linux-gnu/libavformat.so.60
                       /usr/lib/x86_64-linux-gnu/libavutil.so.58
                        /usr/lib/x86_64-linux-gnu/libswscale.so.7
                        z m pthread dl)

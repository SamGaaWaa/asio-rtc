set(FFMPEG_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg)
set(FFMPEG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg)

add_executable(ffmpeg_track_example
    main.cpp
    ${FFMPEG_SRC_DIR}/codecs/vpx.cpp
    ${FFMPEG_SRC_DIR}/codecs/h264.cpp
    ${FFMPEG_SRC_DIR}/codecs/vp9.cpp
    ${FFMPEG_SRC_DIR}/codecs/opus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/codecs/g711.cpp
)
target_include_directories(ffmpeg_track_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/asio-ice/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${FFMPEG_SRC_DIR}
    ${AVCODEC_INCLUDE_DIR})
target_link_libraries(ffmpeg_track_example PRIVATE asiortc_test_flags
    OpenSSL::SSL OpenSSL::Crypto srtp3
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
    z m pthread dl)

cmake_minimum_required(VERSION 3.22)

project(asio-rtc)

option(ASIORTC_USE_STANDALONE_ASIO "Uses standalone asio" OFF)
option(ASIORTC_TEST "Build test" ON)
# set(STDEXEC_DIR)

set(CMAKE_CXX_STANDARD 23)

if(ASIORTC_USE_STANDALONE_ASIO)
    set(__ASIORTC_USE_STANDALONE_ASIO 1)
else()
    set(__ASIORTC_USE_STANDALONE_ASIO 0)
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/include/asiortc/config.hpp.in" "${CMAKE_CURRENT_SOURCE_DIR}/include/asiortc/config.hpp")

find_package(OpenSSL 3 REQUIRED)

set(Boost_DEBUG ON)
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.89 CONFIG REQUIRED)
if(Boost_FOUND)
    message(STATUS "Boost found")
endif()
if(ASIORTC_USE_STANDALONE_ASIO)
    set(ASIO_STANDALONE ON)
    find_package(asio CONFIG REQUIRED)
endif()

set(ASIOICE_ENABLE_SCTP_OVER_DTLS ON)
if(ASIORTC_USE_STANDALONE_ASIO)
    set(ASIOICE_USE_BOOST_ASIO OFF)
endif()
add_subdirectory(third_party/asio-ice)

set(CRYPTO_LIBRARY openssl)
set(LIBSRTP_TEST_APPS OFF)
add_subdirectory(third_party/libsrtp)

 file(GLOB RTC_SRC_FILES 
    src/candidate.cpp
    src/peer_connection.cpp
    src/connection_impl.cpp
    src/rtp_interfaces.cpp
    src/srtp_transport.cpp
    src/rtp.cpp
    src/rtcp.cpp
    src/sdp.cpp
    src/rtp_transceiver.cpp
    src/media_track_impl.cpp
    src/jitter_buffer.cpp
    src/rtp_capabilities.cpp
    src/twcc.cpp
    src/nack_generator.cpp
    src/codecs/default_vpx.cpp
    src/codecs/default_h264.cpp
    src/codecs/default_opus.cpp
    src/codecs/vpx_descriptor.cpp
    src/nack_generator.cpp
    src/uuid.cpp
    src/any_stream_track.cpp
)
add_library(asiortc STATIC ${RTC_SRC_FILES})
target_link_libraries(asiortc PRIVATE asioice OpenSSL::SSL OpenSSL::Crypto srtp3)
target_link_libraries(asiortc PUBLIC Boost::boost)
if(ASIORTC_USE_STANDALONE_ASIO)
    target_link_libraries(asiortc PUBLIC asio::asio)
    target_compile_definitions(asiortc PUBLIC ASIO_STANDALONE)
endif()
target_include_directories(asiortc PUBLIC ${STDEXEC_DIR})
target_include_directories(asiortc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(asiortc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(asiortc PRIVATE asioice OpenSSL::SSL OpenSSL::Crypto srtp3)

add_library(asiortc_compile_flags INTERFACE)
target_compile_options(asiortc_compile_flags INTERFACE
                       $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fconcepts-diagnostics-depth=10 -Wno-non-template-friend -Wall -fcoroutines>
                       )
target_compile_options(asiortc_compile_flags INTERFACE
                       $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:__cplusplus /Zc:preprocessor /utf-8 /D_WIN32_WINNT=0x0601 /Zm200 /bigobj /W4 /wd4100 /wd4101 /wd4127 /wd4324 /wd4456 /wd4459>
                       )
target_link_libraries(asiortc_compile_flags INTERFACE
                      $<$<PLATFORM_ID:Windows>:bcrypt;ws2_32;iphlpapi;mswsock>
                      )
target_compile_options(asiortc_compile_flags INTERFACE
                       $<$<AND:$<COMPILE_LANG_AND_ID:CXX,GNU>,$<PLATFORM_ID:Windows>>:-Wa,-mbig-obj>
                       )
target_link_libraries(asiortc PRIVATE asiortc_compile_flags)

add_library(asiortc_test_flags INTERFACE)
target_link_libraries(asiortc_test_flags INTERFACE Boost::boost)
if(ASIORTC_USE_STANDALONE_ASIO)
    target_link_libraries(asiortc_test_flags INTERFACE asio::asio)
    target_compile_definitions(asiortc_test_flags INTERFACE ASIO_STANDALONE)
endif()
target_link_libraries(asiortc_test_flags INTERFACE asiortc asiortc_compile_flags)
target_compile_definitions(asiortc_test_flags INTERFACE DCSCTP_USE_BOOST_MOVE_ONLY_FUNCTION)
target_include_directories(asiortc_test_flags INTERFACE ${STDEXEC_DIR})
target_include_directories(asiortc_test_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(asiortc_test_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(asiortc_test_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-ice/include)
target_include_directories(asiortc_test_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-ice/exsctp)
target_include_directories(asiortc_test_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-ice/exsctp/dcsctp/dcsctp)

if(ASIORTC_TEST)
    find_package(Boost COMPONENTS process CONFIG REQUIRED)
    file(GLOB ASIORTC_TEST_SOURCES
        src/srtp_test.cpp
        src/rtp_rtcp_test.cpp
        src/sdp_test.cpp
        src/any_stream_track_test.cpp
    )
    foreach(TEST_FILE ${ASIORTC_TEST_SOURCES})
        get_filename_component(EXECUTABLE_NAME ${TEST_FILE} NAME_WE)
        add_executable(${EXECUTABLE_NAME} ${TEST_FILE})
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE asiortc_test_flags OpenSSL::SSL OpenSSL::Crypto srtp3 Boost::process)
    endforeach()
endif()

add_subdirectory(example/srtp)
add_subdirectory(example/chat)
add_subdirectory(example/transceiver_test)
add_subdirectory(example/ffmpeg_track)
add_subdirectory(example/sfu)
add_subdirectory(example/test)
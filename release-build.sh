#!/bin/bash
mkdir -p clang-release-build
pushd clang-release-build

cmake .. \
        -DBoost_DIR=/home/sam/opensource/asio-rtc/boost_clang/lib/cmake/Boost-1.91.0 \
        -DSTDEXEC_DIR=/home/sam/opensource/stdexec/include \
        -DASIOICE_TEST=OFF \
        -DASIOICE_EXAMPLE=OFF \
        -DLIBSRTP_TEST_APPS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang-20 \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++ -O3 -DNDEBUG" \
        -DCMAKE_CXX_COMPILER=clang++-20 \
        -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=mold -L/usr/lib/llvm-20/lib -Wl,-rpath,/usr/lib/llvm-20/lib"
make
popd

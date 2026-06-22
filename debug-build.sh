#!/bin/bash
mkdir -p clang-build
pushd clang-build

cmake .. \
        -DBoost_DIR=/home/sam/opensource/asio-rtc/boost_clang/lib/cmake/Boost-1.91.0 \
        -DSTDEXEC_DIR=/home/sam/opensource/stdexec/include \
        -DASIOICE_TEST=OFF \
        -DASIOICE_EXAMPLE=OFF \
        -DLIBSRTP_TEST_APPS=OFF \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang-20 \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++ -O0 -g3 -fsanitize=address" \
        -DCMAKE_CXX_COMPILER=clang++-20 \
        -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=mold -L/usr/lib/llvm-20/lib -Wl,-rpath,/usr/lib/llvm-20/lib"
make -j2
popd

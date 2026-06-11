# AGENTS.md — asio-rtc

## Build

```bash
./debug-build.sh          # Clang 20 debug + ASan → clang-build/
```

Hardcodes personal paths (`Boost_DIR`, `STDEXEC_DIR`). Edit before first build. Also requires OpenSSL 3.

Manual CMake:
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DBoost_DIR=<path>/lib/cmake/Boost-1.89.0 \
         -DSTDEXEC_DIR=<path>/stdexec/include \
         -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
make -j$(nproc)
```

Key CMake options: `ASIORTC_USE_STANDALONE_ASIO` (default OFF, uses Boost.Asio), `ASIORTC_TEST` (default ON).

## Test

Single self-contained test executable (no framework):

```bash
cd clang-build && ./srtp_test
```

## Architecture

- `asiortc` is a **thin WebRTC layer** on top of `asio-ice` (ICE + DTLS + SCTP + DataChannel).
- Public headers in `include/`; internal implementation headers in `src/`.
- **Private headers in `src/` must NOT expose third-party includes** (libsrtp, OpenSSL internals). Keep includes like `srtp.h` in `.cpp` files only. The header `src/srtp_transport.hpp` is private to the library — used by `src/*.cpp` but not by external consumers.
- Namespace: `asiortc`

## Dependencies (git submodules)

- `third_party/asio-ice` — ICE/DTLS/SCTP transport stack. See `third_party/asio-ice/AGENTS.md` for its conventions.
- `third_party/libsrtp` — Cisco libsrtp 3.x (SRTP keying for WebRTC media).

Both linked statically. CMake sets `Boost_USE_STATIC_LIBS ON`.

## SRTP init guard pattern

`src/srtp_transport.cpp` uses a `shared_ptr<srtp_init_guard>` via `weak_ptr` + `mutex` to ensure `srtp_init()` is called exactly once and `srtp_shutdown()` runs when the last transport is destroyed. The guard is copied into every `srtp_transport_base` instance so the consumer doesn't need to manage init/shutdown.

When adding SRTP functionality:
- **Declare in `src/srtp_transport.hpp`** (private header, fine to add `srtp_t` session as opaque `void*`/PIMPL, but do NOT `#include "srtp.h"` in any header).
- **Implement in `src/srtp_transport.cpp`** — include `srtp.h` only here, cast between opaque handle and `srtp_t`.
- Every `srtp_transport_base` already holds a `shared_ptr<srtp_init_guard>`, so the library is guaranteed alive for the object's lifetime.

## SRTP transport (`src/srtp_transport.hpp`, `src/srtp_transport.cpp`)

`srtp_transport` wraps libsrtp send/receive sessions derived from DTLS-SRTP key material.

- **Constructor** takes `asioice::ssl::srtp_key_material` + `dtls_role`. Based on role (RFC 5764):
  - DTLS client sends with `client_write_key` + `client_write_salt`, receives with `server_write_*`
  - DTLS server sends with `server_write_key` + `server_write_salt`, receives with `client_write_*`
- **Separate `srtp_t` sessions**: one for send (`ssrc_any_outbound`), one for recv (`ssrc_any_inbound`). A single session handles both RTP and RTCP.
- **`max_protect_overhead = 20`**: 4 (SRTCP index) + 16 (GCM auth tag). Used internally to size buffers for in-place protect. Callers must pre-size buffers with this overhead.
- **Sequence numbers must be monotonically increasing** per session; libsrtp rejects duplicate SNs with `replay_fail`.
- **Profile mapping**: asio-ice's `srtp_protection_profile` enum → libsrtp `srtp_profile_t`. Only the 4 WebRTC profiles are supported (AES_CM_128_HMAC_SHA1_80/32, AEAD_AES_128/256_GCM).
- Header forward-declares asio-ice types (`srtp_key_material`, `dtls_role`, `srtp_protection_profile`); full definitions included only in `.cpp`. `srtp.h` never appears in any header.
- `remote_sdp` + `parse_remote_sdp()` mirrors the SDP parsing pattern from asio-ice examples for extracting ICE credentials, fingerprint, and setup role.

## Style

- C++23, coroutines (`-fcoroutines`), P2300 senders/receivers (stdexec)
- 4-space indent, 80-col suggested
- `.clang-format` in submodules; no root-level `.clang-format` — don't create one unless asked
- No comments unless asked

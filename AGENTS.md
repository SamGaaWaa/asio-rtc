# AGENTS.md — asio-rtc

## Build

```bash
./debug-build.sh          # Clang 20 debug + ASan → clang-build/
```

Hardcodes personal paths (`Boost_DIR`, `STDEXEC_DIR`). Edit before first build. Also requires OpenSSL 3.
Uses `libc++` (`-stdlib=libc++`) and `mold` linker (`-fuse-ld=mold`).
`ASIORTC_ENABLE_FFMPEG` is OFF by default; enabling it requires a full FFmpeg build with all transitive dependencies (opus, vpx, srt, etc.) — the pre-built static libs in `third_party/ffmpeg/lib/` may not link if system deps are missing.

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

## Style (from asio-ice conventions)

- C++23, coroutines (`-fcoroutines`), P2300 senders/receivers (stdexec)
- 4-space indent, 80-col suggested
- `.clang-format` at repo root (LLVM-based, 80-col, 4-space indent, PointerAlignment: Right)
- No comments unless asked
- **Member naming**: `_prefix` for private members (`_send_session`, `_profile`, `_init_guard`)
- **Type naming**: `snake_case` (e.g., `srtp_transport_base`, `remote_sdp`, `attr_type_t`)
- **Include order**: own header first, then project headers, then third-party, then std
- **`#pragma once`** everywhere; no include guards
- **File pairs**: private header in `src/` (`.hpp`) + implementation in `src/` (`.cpp`)
- **Test files**: `*_test.cpp` in `src/`, standalone executables (no framework), exit on failure

## RTP / RTCP packet parsing & serialization

Place new files in `src/` as private implementation headers (`src/rtp.hpp`, `src/rtp.cpp`). Follow the `asioice::stun` pattern: wire-format structs + message struct with `parse()` / `write_to()` / `serialized_size()`.

### Namespace
`asiortc::rtp` — mirrors `asioice::stun`.

### Wire-format pattern
```cpp
#pragma pack(push, 1)
struct rtp_header_t {
    uint8_t csrcc : 4;
    uint8_t extension : 1;
    uint8_t padding : 1;
    uint8_t version : 2;
    // ...
};
static_assert(sizeof(rtp_header_t) == 12, "RTP fixed header must be 12 bytes");
```

Use `static_assert` on every wire-format struct size.

### Byte order
RTP/RTCP uses big-endian (network byte order). Use these helpers from asio-ice's `third_party/binary.hpp` (include path: `"binary.hpp"`):

```cpp
#include "binary.hpp"
// reading
uint16_t seq = binary::ntoh<uint16_t>(header->sequence_number);
uint32_t ts  = binary::read_big<uint32_t>(data);
// writing
header->type = binary::hton<uint16_t>(200);
binary::write_big<uint32_t>(dest, ssrc);
```

### Message struct pattern (from `asioice::stun`)
```cpp
struct rtp_packet {
    // public fields — packet data
    uint8_t version = 2;
    uint8_t padding = 0;
    uint8_t extension = 0;
    uint8_t csrcc = 0;
    uint8_t marker = 0;
    uint8_t payload_type = 0;
    uint16_t sequence_number = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    // ...
    std::span<const uint8_t> payload;
    std::span<const uint8_t> extension_data;

    // parsing
    static std::optional<rtp_packet> parse(const void *data,
                                           std::size_t len) noexcept;
    // serialization
    int write_to(void *data, std::size_t len) const noexcept;
    std::size_t serialized_size() const noexcept;
};
```

### RTCP compound packets
RTCP packets are compound (multiple packets in one UDP datagram). Each RTCP sub-packet has its own type byte, length in 32-bit words minus 1, and payload. Use a variant/visitor or iterator pattern:

```cpp
struct rtcp_packet {
    uint8_t type;        // SR=200, RR=201, SDES=202, BYE=203, APP=204
    uint16_t length;     // in 32-bit words minus 1
    // ... per-type payload

    static std::optional<rtcp_packet> parse(const void *data,
                                            std::size_t len) noexcept;
    int write_to(void *data, std::size_t len) const noexcept;
    std::size_t serialized_size() const noexcept;
};
```

### Adding new source / test files
Update `CMakeLists.txt`:
- Source files: add to `file(GLOB RTC_SRC_FILES ...)`
- Test files: add to `file(GLOB ASIORTC_TEST_SOURCES ...)`

## Available utilities (from asio-ice)

| Header | What |
|--------|------|
| `"binary.hpp"` | `binary::ntoh<T>`, `binary::hton<T>`, `binary::read_big<T>`, `binary::write_big<T>` |
| `"asioice/detail/scope_guard.hpp"` | `asioice::utils::scope_guard<F>` — RAII cleanup, requires `nothrow_invocable<F>` |
| `"asioice/endian.hpp"` | `__ICE_LITTLE_ENDIAN__` define |
| `"json.hpp"` | nlohmann JSON (no path prefix, private include for asioice targets) |

`binary.hpp` is available because asioice's CMake does `target_include_directories(asioice PRIVATE .../third_party)`, and asio-rtc's tests link `asiortc_test_flags` which includes `asioice/include` — but `binary.hpp` lives in `third_party/asio-ice/third_party/`. If needed from asio-rtc source, add the include path in CMakeLists.txt.

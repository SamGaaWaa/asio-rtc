# AGENTS.md — asio-rtc

## Build

```bash
./debug-build.sh          # Clang 20 debug + ASan → clang-build/
./release-build.sh        # Clang 20 release → clang-release-build/
```

Both hardcode personal paths (`Boost_DIR`, `STDEXEC_DIR`). Edit before first build. Requires OpenSSL 3.
Uses `libc++` (`-stdlib=libc++`) and `mold` linker (`-fuse-ld=mold`). Scripts also pass `-DASIOICE_TEST=OFF -DASIOICE_EXAMPLE=OFF`.
`ASIORTC_ENABLE_FFMPEG` is OFF by default; enabling it requires a full FFmpeg build with all transitive dependencies (opus, vpx, srt, etc.) — the pre-built static libs in `third_party/ffmpeg/lib/` may not link if system deps are missing.

Manual CMake:
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DBoost_DIR=<path>/lib/cmake/Boost-1.91.0 \
         -DSTDEXEC_DIR=<path>/stdexec/include \
         -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20
make -j$(nproc)
```

`find_package(Boost 1.89)` in CMakeLists.txt is a minimum; the build scripts use Boost 1.91.0.
Examples `sfu`, `test`, `ffmpeg_track`, `file_frame`, `recorder` embed their page via C++23 `#embed "index.html"` — needs Clang 19+/GCC 15+.
Examples `ffmpeg_exe`, `file_frame`, `recorder` shell out to the `ffmpeg`/`ffprobe` executable via `boost::process::v2::popen` (built unconditionally; need `ffmpeg` in `PATH` at runtime). Only `ffmpeg_track` links FFmpeg libs and is gated by `ASIORTC_ENABLE_FFMPEG`.

Key CMake options: `ASIORTC_USE_STANDALONE_ASIO` (default OFF, uses Boost.Asio), `ASIORTC_TEST` (default ON).

## Test

Three self-contained test executables (no framework), one per `src/*_test.cpp`:

```bash
cd clang-build
./srtp_test
./rtp_rtcp_test
./sdp_test
./any_stream_track_test ../test.webm   # only built with ASIORTC_ENABLE_FFMPEG=ON
```

## Architecture

- `asiortc` is a **thin WebRTC layer** on top of `asio-ice` (ICE + DTLS + SCTP + DataChannel).
- Public headers in `include/`; internal implementation headers in `src/`.
- **Private headers in `src/` must NOT expose third-party includes** (libsrtp, OpenSSL internals). Keep includes like `srtp.h` in `.cpp` files only. The header `src/srtp_transport.hpp` is private to the library — used by `src/*.cpp` but not by external consumers.
- Namespace: `asiortc`
- **No pluggable encoder/decoder API.** `media_track::description()` returns a `media_description{format, clock_rate, encoding_params, channels}`; its `format` is the single source of truth for codec selection. SDP m=lines contain exactly one codec derived from the track format. Built-in RTP packetizers handle framing/fragmentation.
- **`media_format` only contains encoded types**: `opus`, `vp8`, `h264`, `vp9`. No raw formats (YUV, PCM). Enum values are the RTP payload-type numbers (`opus=200, vp8=201, h264=202, vp9=203`; `unknown=-1`).
- **Logging via `samlog`** (from asio-ice's `third_party/asio-ice/samlog/`), not iostream. `include/asiortc/peer_connection.hpp` does `using namespace samlog;`; use `SAMLOG_INFO`/`SAMLOG_WARN`/`SAMLOG_ERROR` macros.
- **SDP lives in `src/sdp.hpp`/`src/sdp.cpp`** (`session_description`, `sdp_media`, `sdp_rtpmap`, etc.). The public interface is `include/asiortc/session_description.hpp` (`session_description_interface` + `parse_sdp()`); the concrete structs are private to the library.

## Track API & simulcast

`media_track::recv()` signature:
```cpp
struct encode_target {
    std::optional<int> max_bitrate;
    std::string rid;
};

virtual task<std::vector<media_frame>>
recv(std::span<const encode_target> layers) = 0;
```

For non-simulcast, callers pass a single-element span. For simulcast, one
`encode_target` per layer; the track may return multiple frames per call
(e.g., hardware encoder producing multiple resolutions in one pass).

## RTP packetizers (`src/rtp_packetizer/`)

Built-in per-codec RTP framing. Selected automatically based on `track->description().format`.
No external registration needed. Each `rtp_sender` holds a `unique_ptr<rtp_packetizer_base>`.

| File | Class | Handles |
|------|-------|---------|
| `h264_packetizer.hpp/.cpp` | `H264Packetizer` | Annex B → NAL unit extraction + FU-A fragmentation |
| `vpx_packetizer.hpp/.cpp` | `Vp8Packetizer` / `Vp9Packetizer` | VP8/VP9 descriptor + MTU fragmentation |
| `opus_packetizer.hpp/.cpp` | `OpusPacketizer` | Passthrough (Opus frames always ≤ MTU) |
| `vpx_descriptor.hpp/.cpp` | `vpx_payload_descriptor` | VP8/VP9 RTP descriptor parse/serialize |
| `base.hpp` | `rtp_packetizer_base` | `pack(data, timestamp) → {payloads, timestamp}` |

## RTP receive path

`SRTP → do_on_rtp_rtcp_packet → dispatch_rtp → media_track_impl` (all in `src/connection_impl.cpp`):

1. `dispatch_rtp` calls `rewrite_rtx_packet()` — rewrites RTX to the original PT/SSRC/seq using `_rtx_pt_to_pt`/`_rtx_ssrc_to_ssrc`, built by `build_ssrc_map()` from the offer's `a=ssrc-group:FID` + `a=fmtp:<rtx> apt=<pt>`.
2. Routes to a receiver by the sdes:mid extension (`_mid_ext_id`) or by SSRC (`_ssrc_set`).
3. `depayload()` strips the per-packet VP8/VP9 payload descriptor (H264 is a no-op at packet level).
4. `media_track_impl::push_frame()` → `jitter_buffer` assembles consecutive packets sharing a timestamp; `recv()` returns one `media_frame` per frame with `data` = raw codec bitstream.

- `media_track_impl` is constructed from an `sdp_rtpmap` (not `media_kind`); `rtpmap()`/`description()` derive the format from the codec name.
- **Codec names are lowercase** in `sdp_rtpmap::from_media_description()` (`"vp8"`, `"h264"`, `"opus"`) but mixed-case in parsed SDP (`"VP8"`, `"H264"`). Compare with `asioice::utils::nceq()`, never `==` — `depayload()` and `media_track_impl::description()` once used `==` and silently skipped VP8 descriptor stripping / reported `unknown` format.
- **`media_track_impl` must init its jitter buffer with `is_video = !nceq(codec.name, "opus")`.** `jitter_buffer` defaults `is_video=false`, which makes `pop_frame()` emit a partial multi-packet frame the moment the buffer is exhausted instead of waiting — this split fragmented VP8 keyframes into corrupt frames (ffmpeg: "Invalid profile / Invalid sync code").
- **`nack_generator::get_nacks()` must scan only the valid window `[lowest_seq, highest_seq)`** (tracked by `_lowest_seq`), never the whole ring buffer. The buffer is zero-initialized, so scanning all `_history_size` slots NACKs every never-received seq (including those before the stream's first packet) — producing ~1000 NACKs ("sent rtcp nack feedback: ... 952 items") on a loss-free local connection.

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
- **Buffer sizing**: `max_protect_rtp_overhead()` returns `SRTP_MAX_TRAILER_LEN`, `max_protect_rtcp_overhead()` returns `SRTP_MAX_SRTCP_TRAILER_LEN` (libsrtp constants). The `send_rtp(Vec&)`/`send_rtcp(Vec&)` vector overloads auto-resize the buffer in-place; the span overloads require the caller to pre-size. **RTCP feedback can be larger than RTP** — `sync_send_rtcp()` allocates a heap buffer sized `data.size() + max_protect_rtcp_overhead()`; a fixed-size stack buffer makes large NACK feedback fail with "srtp_protect_rtcp failed: out buffer is too small".
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

RTP packet structs are public (`include/asiortc/rtp.hpp` + `src/rtp.cpp`); RTCP is private (`src/rtcp.hpp`/`src/rtcp.cpp`). Follow the `asioice::stun` pattern: wire-format structs + message struct with `parse()` / `write_to()` / `serialized_size()`.

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

Note: `RTC_SRC_FILES` still lists `src/codecs/*.cpp` entries (e.g. `default_vpx.cpp`) that no longer exist — that directory was removed but the glob wasn't cleaned up. GLOB silently skips missing files, so the build still works; don't be misled into recreating `src/codecs/`.

### Adding new RTP packetizers
Place files in `src/rtp_packetizer/`. Inherit from `rtp_packetizer_base` in `base.hpp`.
Update `CMakeLists.txt` glob and add include to `src/connection_impl.cpp`.

## Available utilities (from asio-ice)

| Header | What |
|--------|------|
| `"binary.hpp"` | `binary::ntoh<T>`, `binary::hton<T>`, `binary::read_big<T>`, `binary::write_big<T>` |
| `"asioice/detail/scope_guard.hpp"` | `asioice::utils::scope_guard<F>` — RAII cleanup, requires `nothrow_invocable<F>` |
| `"asioice/endian.hpp"` | `__ICE_LITTLE_ENDIAN__` define |
| `"json.hpp"` | nlohmann JSON (no path prefix, private include for asioice targets) |

`binary.hpp` is available because asioice's CMake does `target_include_directories(asioice PRIVATE .../third_party)`, and asio-rtc's tests link `asiortc_test_flags` which includes `asioice/include` — but `binary.hpp` lives in `third_party/asio-ice/third_party/`. If needed from asio-rtc source, add the include path in CMakeLists.txt.

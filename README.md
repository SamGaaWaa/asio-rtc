# asio-rtc

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

English | [简体中文](./README.zh-CN.md)

A fresh WebRTC implementation built on Boost.Asio and stdexec. Includes ICE (RFC 8445), STUN, TURN, DataChannel, and SRTP media.

## When to use asio-rtc

**Use asio-rtc if:**
- Your project uses Boost.Asio and you need WebRTC connectivity with browsers or SFU servers.
- You want C++26 sender/receiver interfaces.
- You prefer a small dependency footprint.

asio-rtc is a great fit because:
  - It is a complete reimplementation on top of asio, with no internal threads — all logic runs single-threaded on the io_context.
  - Every API returns a stdexec sender, composable with `then`, `when_all`, `async_scope`, and other algorithms. Coroutines work seamlessly alongside senders. Custom schedulers can offload heavy work like encoding to a thread pool.
  - All operations support cancellation via `stop_token`.
  - The codebase is small and readable, easy to modify.

**Not a good fit if:**
- You need an older C++ standard.
  - asio-rtc targets C++23/26.
- You want synchronous APIs.
  - asio-rtc provides only async interfaces and is not thread-safe, so `stdexec::sync_wait(async_op)` won't help either.
- You don't want C++26 senders.
  - asio-rtc uses Boost.Asio internally but exposes only sender-based APIs. Traditional completion token compatibility is in progress; use `use_sender` for bridging.
- You need a battle-tested, production-ready WebRTC library.
  - asio-rtc is under heavy development and not yet rigorously tested.

## Features

- [x] **PeerConnection** — offer/answer, ICE, DTLS-SRTP, SCTP DataChannel
- [x] **Transceiver** — add_transceiver, get_transceivers, replace_track
- [x] **DataChannel** — RFC 8832 DCEP over SCTP-over-DTLS
- [x] **SDP** — parse/generate, BUNDLE, codec negotiation, extmaps
- [x] **Codecs** — VP8, VP9, H264, Opus default encoders/decoders
- [x] **Jitter buffer** — RTP reorder and frame assembly
- [x] **NACK** — NACK retransmission
- [x] **TWCC** — TWCC receive-side feedback
- [x] **mDNS**
- [x] **Trickle-ICE**
- [x] **ICE-Lite Client Side**
- [ ] **ICE-Lite Server Side**
- [ ] **GCC** — transport-wide congestion control (TWCC send-side)
- [ ] **Bandwidth estimation** — REMB send/receive
- [ ] **Simulcast**
  - [x] send
  - [ ] recv
- [ ] **FEC** — RED, ULPFEC
- [ ] **Renegotiation**
- [ ] **DTMF sender**
- [ ] **Audio PLC**
- [ ] **TCP candidate support**
- [ ] **TURN**
  - [x] UDP
  - [ ] TCP
  - [ ] TLS
- [ ] asio completion token compatibility
- [ ] **Browser interop test suite**

## Dependencies

| Dependency | Version | Notes |
|-----------|---------|-------|
| [Boost (headers only)](https://www.boost.org/) | 1.89+ | boost.container, boost.intrusive, etc.; required even with standalone Asio |
| [stdexec](https://github.com/NVIDIA/stdexec) | — | std::execution reference implementation |
| [OpenSSL](https://www.openssl.org/) | 3.0+ | DTLS/SCTP |
| [asio-ice](https://github.com/SamGaaWaa/asio-ice) | git submodule | ICE/DTLS/SCTP transport |
| [libsrtp](https://github.com/cisco/libsrtp) | git submodule | SRTP encryption |
| Clang / GCC / MSVC | 20+ / 13+ / 19+ | C++23 |

## Build

```bash
git clone --recurse-submodules https://github.com/SamGaaWaa/asio-rtc.git
./debug-build.sh          # Clang 20 debug + ASan → clang-build/
```

Or manually:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DBoost_DIR=/path/to/boost/lib/cmake/Boost-1.91.0 \
         -DSTDEXEC_DIR=/path/to/stdexec/include \
         -DCMAKE_CXX_COMPILER=clang++-20
make -j$(nproc)
```

| CMake option | Default | Description |
|-------------|---------|-------------|
| `ASIORTC_USE_STANDALONE_ASIO` | `OFF` | Standalone Asio instead of Boost.Asio |
| `ASIORTC_TEST` | `ON` | Build tests |
| `ASIORTC_ENABLE_FFMPEG` | `OFF` | ffmpeg codec features |

## Usage

```cpp
#include "asiortc.hpp"

namespace rtc = asiortc;

boost::asio::io_context ctx;

rtc::peer_connection conn(ctx.get_executor(), rtc::configuration{
    .ice_servers = {.urls = {"stun:stun.l.google.com:19302"}}
});

auto tr = conn.add_transceiver(rtc::media_kind::video,
    {.direction = rtc::sdp_direction::sendrecv, .streams = {"my-video"}});

co_await conn.set_remote_description(parse_sdp(browser_offer, "offer"));
auto answer = co_await conn.create_answer();
co_await conn.set_local_description(std::move(answer));
```

## Examples

| Example | Description |
|---------|-------------|
| `chat` | SDP exchanged via copy-paste, DataChannel chat |
| `sfu` | SFU loopback with browser |
| `test` | Negotiation test suite with browser UI |
| `ffmpeg_track` | Video file → WebRTC (`ASIORTC_ENABLE_FFMPEG`) |

## Tests

```bash
cd clang-build
./srtp_test
./rtp_rtcp_test
./sdp_test
./any_stream_track_test ../test.webm   # needs ASIORTC_ENABLE_FFMPEG=ON
```

## References

This project draws inspiration from several open-source WebRTC implementations:

- [aiortc](https://github.com/aiortc/aiortc)
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- [pion/webrtc](https://github.com/pion/webrtc)

## License

MIT

---

> **🚧 This project is under heavy development. Bug reports, feature suggestions, and pull requests are welcome!**

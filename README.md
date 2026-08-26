# asio-rtc

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

English | [简体中文](./README.zh-CN.md)

A fresh WebRTC implementation built on C++23, [Boost.Asio](https://www.boost.org/), and [stdexec](https://github.com/NVIDIA/stdexec).

## When to use asio-rtc

If your project uses Boost.Asio and needs to establish WebRTC connections with browsers or SFU servers, asio-rtc is a great fit because:

  - asio-rtc has no internal threads; all logic runs on an `executor`.
  - asio-rtc supports both the traditional asio async model and the C++26 Senders/Receivers model.
  - asio-rtc provides JavaScript-like interfaces; if you are familiar with browser WebRTC, asio-rtc will feel familiar.
  - All async operations support cancellation.
  - The code is readable and easy to modify.

## Current Progress

- [x] **PeerConnection** — offer/answer, ICE, DTLS-SRTP, SCTP [DataChannel](https://tools.ietf.org/html/rfc8832)
- [x] **Transceiver** — [add_transceiver](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-addtransceiver), [get_transceivers](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-gettransceivers), [replace_track](https://www.w3.org/TR/webrtc/#dom-rtcrtpsender-replacetrack)
- [x] **DataChannel** — RFC 8832 DCEP over SCTP-over-DTLS
- [x] **SDP** — parse/generate, BUNDLE, codec negotiation, extmaps
- [x] **Jitter buffer** — RTP reorder and frame assembly
- [x] **NACK** — packet loss retransmission
- [x] **TWCC** — TWCC receive-side feedback
- [x] **mDNS**
- [x] **[Trickle-ICE](https://tools.ietf.org/html/rfc8838)**
- [x] **ICE-Lite Client Side**
- [ ] **ICE-Lite Server Side**
- [ ] **GCC** — TWCC send-side congestion control
- [ ] **Bandwidth estimation** — [REMB](https://tools.ietf.org/html/draft-alvestrand-rmcat-remb-03) send/receive
- [ ] **Simulcast**
  - [ ] send
  - [ ] recv
- [ ] **FEC** — [RED](https://tools.ietf.org/html/rfc2198), [ULPFEC](https://tools.ietf.org/html/rfc5109)
- [ ] **Renegotiation**
- [ ] **[DTMF](https://tools.ietf.org/html/rfc4733) sender**
- [ ] **Audio PLC**
- [ ] **TCP candidate**
- [ ] **[TURN](https://tools.ietf.org/html/rfc8656)**
  - [x] UDP
  - [ ] TCP
  - [ ] TLS
- [x] asio [completion token](https://www.boost.org/doc/libs/release/doc/html/boost_asio/overview/model/completion_tokens.html) compatible interfaces
- [ ] **Browser interop test suite**

## Dependencies

| Dependency | Version | Notes |
|-----------|---------|-------|
| [Boost (headers only)](https://www.boost.org/) | 1.89+ | boost.container, boost.intrusive, etc.; required even with standalone Asio |
| [stdexec](https://github.com/NVIDIA/stdexec) | — | std::execution reference implementation |
| [OpenSSL](https://www.openssl.org/) | 3.0+ | DTLS/SCTP |
| [asio-ice](https://github.com/SamGaaWaa/asio-ice) | git submodule | ICE/DTLS/SCTP transport layer |
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
         -DSTDEXEC_DIR=/path/to/stdexec/include
make -j$(nproc)
```

| CMake option | Default | Description |
|-------------|---------|-------------|
| `ASIORTC_USE_STANDALONE_ASIO` | `OFF` | Use standalone Asio instead of Boost.Asio (still requires other Boost headers) |
| `ASIORTC_TEST` | `ON` | Build tests |

## Usage

All async interfaces are available in two forms:

- **Sender interface** (default): returns a `stdexec` sender, which can be `co_await`ed in coroutines, or composed with algorithms like `stdexec::then` / `stdexec::when_all` / `exec::async_scope`.
- **Completion token interface**: methods additionally accept an asio completion token (`use_awaitable`, `use_future`, `deferred`, `use_sender`, plain callbacks, etc.), allowing `co_await` in asio coroutines or composition with other completion tokens.

```cpp
#include "asiortc.hpp"

namespace rtc = asiortc;

boost::asio::io_context ctx;

rtc::peer_connection conn(ctx.get_executor(), rtc::configuration{
    .ice_servers = {.urls = {"stun:stun.l.google.com:19302"}}
});

auto tr = conn.add_transceiver(
    rtc::media_description::make_default(rtc::media_format::vp8),
    {.direction = rtc::sdp_direction::sendrecv, .streams = {"my-video"}});

co_await conn.set_remote_description(parse_sdp(browser_offer, "offer"));
auto answer = co_await conn.create_answer();
co_await conn.set_local_description(std::move(answer));
```

Both interface styles:

```cpp
// sender interface: co_await directly in a stdexec coroutine
rtc::task<void> f(rtc::peer_connection &conn) {
    auto offer = co_await conn.create_offer();
}

// completion token interface: co_await in an asio coroutine
boost::asio::awaitable<void> g(rtc::peer_connection &conn) {
    auto offer = co_await conn.create_offer(boost::asio::use_awaitable);
}
```

## Examples

| Example | Description |
|---------|-------------|
| `chat` | SDP exchanged via copy-paste, DataChannel chat |
| `sfu` | Browser video SFU loopback |
| `test` | Browser UI-driven negotiation test |

## Tests

```bash
cd clang-build
./srtp_test
./rtp_rtcp_test
./sdp_test
./any_stream_track_test ../test.webm   # needs ASIORTC_ENABLE_FFMPEG=ON
```

## asio-rtc draws on the following implementations

- [aiortc](https://github.com/aiortc/aiortc)
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- [pion/webrtc](https://github.com/pion/webrtc)

## License

MIT

---

> **🚧 This project is under heavy development. Bug reports, feature suggestions, and pull requests are welcome!**

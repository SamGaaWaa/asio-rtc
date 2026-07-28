# asio-rtc

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

简体中文 | [English](./README.md)

一个全新的 WebRTC 实现，基于 [Boost.Asio](https://www.boost.org/) 和 [stdexec](https://github.com/NVIDIA/stdexec)。包含 [ICE (RFC 8445)](https://tools.ietf.org/html/rfc8445)、[STUN (RFC 8489)](https://tools.ietf.org/html/rfc8489)、[TURN (RFC 8656)](https://tools.ietf.org/html/rfc8656)、[DataChannel (RFC 8832)](https://tools.ietf.org/html/rfc8832) 和 SRTP 多媒体。

## 什么时候用 asio-rtc
如果
* 你的项目使用 Boost.Asio，需要跟浏览器或 SFU 服务器建立 WebRTC 连接。
* 你需要 C++26 的 sender 接口。
* 你不想引入几十个依赖库。

那 asio-rtc 就非常合适，因为
  - asio-rtc 完全使用 asio 重新实现，没有内部线程，所有逻辑在 `io_context` 单线程执行。
  - asio-rtc 所有接口返回 `stdexec` 的 `sender`，能用 `then`、`when_all`、`async_scope` 等算法组合出复杂的异步逻辑，也能跟协程无缝衔接。还可以通过自定义调度器，把编解码等耗时工作移交到线程池。
  - 所有操作都支持通过 `stop_token` 取消。
 - asio-rtc 代码可读性强，方便修改。

但如果有以下情况，则可能不适合：
* 你的 C++ 标准不够新。
  - asio-rtc 使用 C++23，面向 C++26.
* 你想用同步接口。
    - asio-rtc 只提供异步接口，且不是线程安全的，故 `stdexec::sync_wait` 也帮不了你。
* 你不想用 C++26 `sender`。
    - asio-rtc 虽然使用 Boost.Asio 实现，但对外接口都是 `sender`，传统的 `completion token` 兼容接口正在开发中，现在可用 [use_sender](https://github.com/SamGaaWaa/asio2exec) 进行桥接
* 你需要功能完善的、可靠的 WebRTC 实现。
    - asio-rtc正在火热开发中，未经过严谨测试。


## 当前进度

- [x] **PeerConnection** — offer/answer、ICE、DTLS-SRTP、SCTP [DataChannel](https://tools.ietf.org/html/rfc8832)
- [x] **Transceiver** — [add_transceiver](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-addtransceiver)、[get_transceivers](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-gettransceivers)、[replace_track](https://www.w3.org/TR/webrtc/#dom-rtcrtpsender-replacetrack)
- [x] **DataChannel** — RFC 8832 DCEP over SCTP-over-DTLS
- [x] **SDP** — 解析/生成，BUNDLE，编解码协商，extmaps
- [x] **编解码** — VP8、VP9、H264、Opus 默认编解码器
- [x] **Jitter buffer** — RTP 重排序与帧组装
- [x] **NACK** — 丢包重传
- [x] **TWCC** — TWCC 接收端反馈
- [x] **mDNS**
- [x] **[Trickle-ICE](https://tools.ietf.org/html/rfc8838)**
- [x] **ICE-Lite 客户端**
- [ ] **ICE-Lite 服务端**
- [ ] **GCC** — TWCC 发送端拥塞控制
- [ ] **带宽估计** — [REMB](https://tools.ietf.org/html/draft-alvestrand-rmcat-remb-03) 收发
- [ ] **Simulcast**
  - [x] 发送端
  - [ ] 接收端
- [ ] **FEC** — [RED](https://tools.ietf.org/html/rfc2198)、[ULPFEC](https://tools.ietf.org/html/rfc5109)
- [ ] **重协商**
- [ ] **[DTMF](https://tools.ietf.org/html/rfc4733) 发送**
- [ ] **音频 PLC**
- [ ] **TCP candidate**
- [ ] **[TURN](https://tools.ietf.org/html/rfc8656)**
  - [x] UDP
  - [ ] TCP
  - [ ] TLS
- [ ] asio [completion token](https://www.boost.org/doc/libs/release/doc/html/boost_asio/overview/model/completion_tokens.html) 兼容接口
- [ ] **浏览器互操作测试套件**

## 依赖

| 项目 | 版本 | 说明 |
|------|------|------|
| [Boost (headers only)](https://www.boost.org/) | 1.89+ | boost.container, boost.intrusive等，即使用了独立 Asio 也需要 |
| [stdexec](https://github.com/NVIDIA/stdexec) | — | std::execution 参考实现 |
| [OpenSSL](https://www.openssl.org/) | 3.0+ | DTLS/SCTP |
| [asio-ice](https://github.com/SamGaaWaa/asio-ice) | git submodule | ICE/DTLS/SCTP 传输层 |
| [libsrtp](https://github.com/cisco/libsrtp) | git submodule | SRTP 加密 |
| Clang / GCC / MSVC | 20+ / 13+ / 19+| C++23 |

## 构建

```bash
git clone --recurse-submodules https://github.com/SamGaaWaa/asio-rtc.git
./debug-build.sh          # Clang 20 debug + ASan → clang-build/
```

或者手动：

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DBoost_DIR=/path/to/boost/lib/cmake/Boost-1.91.0 \
         -DSTDEXEC_DIR=/path/to/stdexec/include \
         -DCMAKE_CXX_COMPILER=clang++-20
make -j$(nproc)
```

| CMake 选项 | 默认 | 说明 |
|-----------|------|------|
| `ASIORTC_USE_STANDALONE_ASIO` | `OFF` | 使用独立 Asio 替代 Boost.Asio |
| `ASIORTC_TEST` | `ON` | 构建测试 |
| `ASIORTC_ENABLE_FFMPEG` | `OFF` | ffmpeg 编解码功能 |

## 用法

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

## 示例

| 示例 | 说明 |
|------|------|
| `chat` | SDP用复制粘贴交换，DataChannel 聊天 |
| `sfu` | 浏览器视频 SFU 回环转发 |
| `test` | 浏览器 UI 驱动的协商测试套件 |
| `ffmpeg_track` | 视频文件 → WebRTC（需 `ASIORTC_ENABLE_FFMPEG`） |

## 测试

```bash
cd clang-build
./srtp_test
./rtp_rtcp_test
./sdp_test
./any_stream_track_test ../test.webm   # 需要 ASIORTC_ENABLE_FFMPEG=ON
```

## 参考

本项目借鉴了以下开源实现：

- [aiortc](https://github.com/aiortc/aiortc)
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- [pion/webrtc](https://github.com/pion/webrtc)

## 许可证

MIT

---

> **🚧 本项目正在火热开发中。欢迎提交 Bug 报告、功能建议和 Pull Request！**

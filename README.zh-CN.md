# asio-rtc

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

简体中文 | [English](./README.md)

一个全新的 WebRTC 实现，基于 C++23、[Boost.Asio](https://www.boost.org/) 和 [stdexec](https://github.com/NVIDIA/stdexec)。

## 什么时候使用 asio-rtc ？
如果你的项目使用了 Boost.Asio，需要跟浏览器或 SFU 服务器建立 WebRTC 连接。那 asio-rtc 就非常合适，因为：
  - asio-rtc 没有内部线程，所有逻辑在 `executor` 执行。
  - asio-rtc 支持传统的 asio 异步模型，也支持 C++26 的 Senders/Receivers 模型。
  - asio-rtc 提供了与 JavaScript 相似的接口，如果你熟悉网页 WebRTC，使用 asio-rtc 会感到很熟悉。
  - 所有异步操作都支持取消。
  - 代码可读性强，方便修改。

## 当前进度

- [x] **PeerConnection** — offer/answer、ICE、DTLS-SRTP、SCTP [DataChannel](https://tools.ietf.org/html/rfc8832)
- [x] **Transceiver** — [add_transceiver](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-addtransceiver)、[get_transceivers](https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-gettransceivers)、[replace_track](https://www.w3.org/TR/webrtc/#dom-rtcrtpsender-replacetrack)
- [x] **DataChannel** — RFC 8832 DCEP over SCTP-over-DTLS
- [x] **SDP** — 解析/生成，BUNDLE，编解码协商，extmaps
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
  - [ ] 发送端
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
- [x] asio [completion token](https://www.boost.org/doc/libs/release/doc/html/boost_asio/overview/model/completion_tokens.html) 兼容接口
- [ ] **浏览器互操作测试**

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
         -DSTDEXEC_DIR=/path/to/stdexec/include
make -j$(nproc)
```

| CMake 选项 | 默认 | 说明 |
|-----------|------|------|
| `ASIORTC_USE_STANDALONE_ASIO` | `OFF` | 使用独立 Asio 替代 Boost.Asio (仍然依赖其他 Boost 头文件) |
| `ASIORTC_TEST` | `ON` | 构建测试 |

## 用法

所有异步接口都同时提供两种形式：

- **Sender 接口**（默认）：返回 `stdexec sender`，可在协程里 `co_await`，或用 `stdexec::then` / `stdexec::when_all` /
  `exec::async_scope` 等算法组合。
- **Completion token 接口**：方法额外接受一个 asio completion token
  （`use_awaitable`、`use_future`、`deferred`、`use_sender`、普通回调等），
  可在 asio 协程里 `co_await`，或配合其它 completion token 使用。

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

两种接口的写法：

```cpp
// sender 接口：在 stdexec 协程里直接 co_await
rtc::task<void> f(rtc::peer_connection &conn) {
    auto offer = co_await conn.create_offer();
}

// completion token 接口：在 asio 协程里 co_await
boost::asio::awaitable<void> g(rtc::peer_connection &conn) {
    auto offer = co_await conn.create_offer(boost::asio::use_awaitable);
}
```

## 示例

| 示例 | 说明 |
|------|------|
| `chat` | SDP用复制粘贴交换，DataChannel 聊天 |
| `sfu` | 浏览器视频 SFU 回环转发 |
| `test` | 浏览器 UI 驱动的协商测试 |

## 测试

```bash
cd clang-build
./srtp_test
./rtp_rtcp_test
./sdp_test
./any_stream_track_test ../test.webm   # 需要 ASIORTC_ENABLE_FFMPEG=ON
```

## asio-rtc 借鉴了以下实现

- [aiortc](https://github.com/aiortc/aiortc)
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- [pion/webrtc](https://github.com/pion/webrtc)

## 许可证

MIT

---

> **🚧 本项目正在火热开发中。欢迎提交 Bug 报告、功能建议和 Pull Request！**

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>

namespace asiortc {

struct nack_info {
    bool received = false;      // 是否已收到该包
    bool nacked = false;        // 是否已经发送过至少一次 NACK
    uint8_t retry_count = 0;    // 已重试次数
    int64_t last_nack_time = 0; // 上次发送 NACK 的时间戳
};

class nack_generator {
  public:
    // 构造函数
    // @param history_size: 历史窗口大小，通常 1024~2048 即可
    // @param max_retries: 单个包最大重试次数，防止无限重传
    // @param rtt_ms: 初始 RTT 估算值，用于退避
    // @param nack_delay_ms: 检测到丢包后等待多久才发 NACK（防乱序误判），通常
    // 5~20ms
    nack_generator(uint16_t history_size = 1024, uint8_t max_retries = 10,
                   int64_t rtt_ms = 100, int64_t nack_delay_ms = 10);

    // 接收到的 RTP 包序号
    // @param seq: RTP 包的 16 位序列号
    void receive_packet(uint16_t seq);

    // 获取当前需要发送 NACK 的序列号列表
    // @param now_ms: 当前时间戳（毫秒）
    // @param nacks_out: 输出参数，复用外部 vector 避免频繁分配内存
    void get_nacks(int64_t now_ms, std::vector<uint16_t> &nacks_out);

    // 动态更新 RTT（由网络模块如 GCC 算法反馈）
    void update_rtt(int64_t rtt_ms);

  private:
    // 计算 16 位序列号的有符号距离
    static inline int32_t seq_distance(uint16_t a, uint16_t b) {
        return static_cast<int32_t>(static_cast<int16_t>(b - a));
    }

    // 将序列号映射到环形缓冲区的索引
    inline size_t get_index(uint16_t seq) const { return seq % _history_size; }

  private:
    const uint16_t _history_size;
    const uint8_t _max_retries;
    int64_t _rtt_ms;
    const int64_t _nack_delay_ms;

    bool _initialized = false;
    uint16_t _highest_seq = 0; // 目前收到的最大序列号

    // 环形缓冲区，记录每个包的状态
    std::vector<nack_info> _buffer;
};

} // namespace asiortc

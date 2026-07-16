#include "nack_generator.hpp"
#include <algorithm>
#include <cassert>

namespace asiortc {

nack_generator::nack_generator(uint16_t history_size, uint8_t max_retries,
                               int64_t rtt_ms, int64_t nack_delay_ms)
    : _history_size(history_size), _max_retries(max_retries), _rtt_ms(rtt_ms),
      _nack_delay_ms(nack_delay_ms), _buffer(history_size) {}

void nack_generator::receive_packet(uint16_t seq) {
    if (!_initialized) {
        _highest_seq = seq;
        _initialized = true;
        _buffer[get_index(seq)].received = true;
        return;
    }

    int32_t diff = seq_distance(_highest_seq, seq);

    if (diff <= 0) {
        // 1. 旧包或重复包
        if (diff > -_history_size) {
            _buffer[get_index(seq)].received = true;
        }
        // 如果 diff <= -_history_size，太旧了，直接丢弃
        return;
    }

    // 2. 新包
    // 如果跨越了多个包，将中间未收到的包状态保持默认（received=false）
    // 如果 diff >= _history_size，说明跳跃太大，旧窗口完全失效，需要清理
    if (diff >= _history_size) {
        // 清空整个缓冲区状态
        std::fill(_buffer.begin(), _buffer.end(), nack_info{});
    } else {
        // 清理刚刚滑出窗口的包状态（可选：为了严格，清理 [highest_seq + 1, seq
        // - 1] 之外的旧数据） 实际上环形缓冲区覆盖写入时自然清理，但 nack_info
        // 中的其他字段需要重置
        for (uint16_t i = 1; i < diff; ++i) {
            uint16_t missing_seq = _highest_seq + i;
            _buffer[get_index(missing_seq)] = nack_info{}; // 重置状态
        }
    }

    // 标记当前包已收到
    _buffer[get_index(seq)].received = true;

    // 更新最高序列号
    _highest_seq = seq;
}

void nack_generator::get_nacks(int64_t now_ms,
                               std::vector<uint16_t> &nacks_out) {
    nacks_out.clear();

    if (!_initialized)
        return;

    // 退避时间计算：通常为 1 * RTT，为了防止网络抖动可加个常数偏移
    int64_t backoff_ms = _rtt_ms + 20;

    // 从最新的包开始遍历到最旧的包
    // 最旧的包是 _highest_seq - _history_size + 1
    for (uint16_t i = 1; i < _history_size; ++i) {
        uint16_t seq = _highest_seq - i; // 自动处理回绕
        size_t idx = get_index(seq);
        nack_info &info = _buffer[idx];

        if (info.received) {
            continue;
        }

        // 包已收到，跳过
        if (info.retry_count >= _max_retries) {
            // 超过最大重试次数，放弃治疗，防止 NACK 风暴
            // 标记为已收到避免下次再检查
            info.received = true;
            continue;
        }

        if (!info.nacked) {
            // 从未发过 NACK
            // 防过早请求：收到下一个包后等待 _nack_delay_ms 再发，防乱序误判
            // 这里的逻辑是：只有当包缺失时间超过 _nack_delay_ms 才发
            // 由于没有记录包丢失的确切时间，我们用距离 highest_seq 的远近近似
            // 简单处理：如果距离最高序列号 >
            // 1，直接发（因为已经证明后面包到了）
            nacks_out.push_back(seq);
            info.nacked = true;
            info.retry_count++;
            info.last_nack_time = now_ms;
        } else {
            // 已经发过 NACK，检查是否需要重试
            if (now_ms - info.last_nack_time >= backoff_ms) {
                nacks_out.push_back(seq);
                info.retry_count++;
                info.last_nack_time = now_ms;
            }
        }
    }
}

void nack_generator::update_rtt(int64_t rtt_ms) {
    // 平滑 RTT 更新，防止剧烈波动
    _rtt_ms = (_rtt_ms * 7 + rtt_ms) / 8;
}

} // namespace asiortc

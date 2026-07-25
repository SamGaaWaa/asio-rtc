#pragma once

#include <chrono>
#include <cstdint>

#include <boost/intrusive/set.hpp>

#include "nack_generator.hpp"

namespace asiortc {

struct rtp_receiver;

struct ssrc_context
    : boost::intrusive::set_base_hook<
          boost::intrusive::link_mode<boost::intrusive::auto_unlink>> {
    ssrc_context(rtp_receiver &r, uint32_t ssrc) noexcept
        : _receiver{r}, _ssrc{ssrc}, _nack_gen{1024, 10, 100, 10} {}

    ssrc_context(const ssrc_context &) = delete;
    ssrc_context(ssrc_context &&) = delete;
    ssrc_context &operator=(const ssrc_context &) = delete;
    ssrc_context &operator=(ssrc_context &&) = delete;

    uint32_t ssrc() const noexcept { return _ssrc; }
    auto &receiver() noexcept { return _receiver; }
    const auto &receiver() const noexcept { return _receiver; }

    // --- Stream tracking ---

    void track_packet(uint16_t seq, uint32_t rtp_ts) {
        if (!_base_seq_set) {
            _base_seq = seq;
            _base_seq_set = true;
        }
        if (seq < _max_seq) {
            if (_max_seq - seq > 0x8000) {
                _cycles += 0x10000;
                _max_seq = seq;
            }
        } else {
            _max_seq = seq;
        }
        _packets_expected = _cycles + _max_seq - _base_seq + 1;
        _packets_received++;

        auto now = std::chrono::steady_clock::now();
        auto arr_ts = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count() /
            1000 * 90);
        if (_last_arrival_ts != 0) {
            int transit = static_cast<int>(arr_ts - _last_arrival_ts) -
                          static_cast<int>(rtp_ts - _last_rtp_ts);
            if (transit < 0)
                transit = -transit;
            _jitter_q4 += transit - ((_jitter_q4 + 8) >> 4);
        }
        _last_arrival_ts = arr_ts;
        _last_rtp_ts = rtp_ts;
    }

    bool check_gap(uint64_t prev_expected) const noexcept {
        if (!_base_seq_set)
            return false;
        return _packets_expected > prev_expected + 1;
    }

    int consecutive_lost() const noexcept { return _consecutive_lost; }
    void inc_consecutive_lost() noexcept { _consecutive_lost++; }
    void reset_consecutive_lost() noexcept { _consecutive_lost = 0; }

    // --- RTCP SR ---

    void record_sr(uint64_t ntp_ts) {
        _lsr = ntp_ts;
        _lsr_time = std::chrono::steady_clock::now();
    }

    // --- RR building ---

    uint32_t extended_max() const noexcept {
        return static_cast<uint32_t>(_cycles + _max_seq);
    }

    uint64_t packets_expected_count() const noexcept {
        return _packets_expected;
    }
    uint64_t packets_received_count() const noexcept {
        return _packets_received;
    }
    uint64_t expected_prior() const noexcept { return _expected_prior; }
    uint64_t received_prior() const noexcept { return _received_prior; }

    void advance_prior() noexcept {
        _expected_prior = _packets_expected;
        _received_prior = _packets_received;
    }

    uint8_t fraction_lost() const noexcept {
        uint64_t ei = _packets_expected - _expected_prior;
        uint64_t ri = _packets_received - _received_prior;
        int64_t li = static_cast<int64_t>(ei) - static_cast<int64_t>(ri);
        if (ei == 0 || li <= 0)
            return 0;
        return static_cast<uint8_t>((li << 8) / ei);
    }

    int jitter_q4() const noexcept { return _jitter_q4; }
    uint64_t lsr() const noexcept { return _lsr; }
    std::chrono::steady_clock::time_point lsr_time() const noexcept {
        return _lsr_time;
    }

    // --- NACK ---
    nack_generator _nack_gen;

    // --- Reset on reuse (create_ssrc_context) ---
    void reset_stats() noexcept {
        _max_seq = 0;
        _cycles = 0;
        _base_seq = 0;
        _base_seq_set = false;
        _packets_expected = 0;
        _packets_received = 0;
        _expected_prior = 0;
        _received_prior = 0;
        _jitter_q4 = 0;
        _last_arrival_ts = 0;
        _last_rtp_ts = 0;
        _lsr = 0;
        _consecutive_lost = 0;
        _lsr_time = {};
        new (&_nack_gen) nack_generator{1024, 10, 100, 10};
    }

    struct ssrc_key {
        using type = uint32_t;
        type operator()(const ssrc_context &ctx) const noexcept {
            return ctx.ssrc();
        }
    };

  private:
    rtp_receiver &_receiver;
    uint32_t _ssrc;
    uint32_t _max_seq = 0;
    uint64_t _cycles = 0;
    uint32_t _base_seq = 0;
    bool _base_seq_set = false;
    uint64_t _packets_expected = 0;
    uint64_t _packets_received = 0;
    uint64_t _expected_prior = 0;
    uint64_t _received_prior = 0;
    int _jitter_q4 = 0;
    uint32_t _last_arrival_ts = 0;
    uint32_t _last_rtp_ts = 0;
    uint64_t _lsr = 0;
    int _consecutive_lost = 0;
    std::chrono::steady_clock::time_point _lsr_time{};
};

using ssrc_context_set = boost::intrusive::set<
    ssrc_context,
    boost::intrusive::key_of_value<typename ssrc_context::ssrc_key>,
    boost::intrusive::constant_time_size<false>>;

} // namespace asiortc

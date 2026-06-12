#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace asiortc::rtp {

bool is_rtp_packet(std::span<const uint8_t> data) noexcept;

struct rtp_packet {
  uint8_t version = 2;
  uint8_t padding = 0;
  uint8_t extension = 0;
  uint8_t csrc_count = 0;
  uint8_t marker = 0;
  uint8_t payload_type = 0;
  uint16_t sequence_number = 0;
  uint32_t timestamp = 0;
  uint32_t ssrc = 0;
  std::vector<uint32_t> csrcs;
  uint16_t extension_profile = 0;
  std::span<const uint8_t> extension_data;
  std::span<const uint8_t> payload;

  static std::optional<rtp_packet> parse(const void* data,
                                         std::size_t len) noexcept;
  int write_to(void* data, std::size_t len) const noexcept;
  std::size_t serialized_size() const noexcept;

  static uint32_t get_ssrc(std::span<const uint8_t> data) noexcept;
  static uint8_t get_payload_type(std::span<const uint8_t> data) noexcept;
  static uint16_t get_sequence_number(std::span<const uint8_t> data) noexcept;
};

}  // namespace asiortc::rtp

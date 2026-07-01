#include <cstring>
#include <iostream>
#include <vector>

#include "rtcp.hpp"
#include "rtp.hpp"

using namespace asiortc;

#define ASSERT(cond)                                                        \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                << "\n";                                                    \
      std::exit(1);                                                         \
    }                                                                       \
  } while (0)

static std::vector<uint8_t> make_basic_rtp(uint16_t seq = 1,
                                           uint32_t ts = 1,
                                           uint32_t ssrc = 0x12345678) {
  std::vector<uint8_t> pkt(12);
  pkt[0] = 0x80;  // V=2, P=0, X=0, CC=0
  pkt[1] = 0x60;  // M=0, PT=96
  pkt[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  pkt[3] = static_cast<uint8_t>(seq & 0xFF);
  pkt[4] = static_cast<uint8_t>((ts >> 24) & 0xFF);
  pkt[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
  pkt[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
  pkt[7] = static_cast<uint8_t>(ts & 0xFF);
  pkt[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
  pkt[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
  pkt[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
  pkt[11] = static_cast<uint8_t>(ssrc & 0xFF);
  for (size_t i = 0; i < 20; ++i)
    pkt.push_back(static_cast<uint8_t>(i));
  return pkt;
}

static void test_rtp_parse_basic() {
  auto raw = make_basic_rtp();
  auto pkt = rtp::rtp_packet::parse(raw.data(), raw.size());
  ASSERT(pkt.has_value());
  ASSERT(pkt->version == 2);
  ASSERT(pkt->padding == 0);
  ASSERT(pkt->extension == 0);
  ASSERT(pkt->csrc_count == 0);
  ASSERT(pkt->marker == 0);
  ASSERT(pkt->payload_type == 96);
  ASSERT(pkt->sequence_number == 1);
  ASSERT(pkt->timestamp == 1);
  ASSERT(pkt->ssrc == 0x12345678);
  ASSERT(pkt->csrcs.empty());
  ASSERT(pkt->extension_data.empty());
  ASSERT(pkt->payload.size() == 20);
  ASSERT(pkt->payload[0] == 0);
  ASSERT(pkt->payload[19] == 19);

  std::cout << "  parse basic OK\n";
}

static void test_rtp_parse_max_fields() {
  std::vector<uint8_t> raw(12 + 4 * 4 + 20);
  // V=2, P=0, X=0, CC=4
  raw[0] = 0x84;
  // M=1, PT=127
  raw[1] = 0xFF;
  // seq = 65535
  raw[2] = 0xFF;
  raw[3] = 0xFF;
  // ts = max
  raw[4] = 0xFF;
  raw[5] = 0xFF;
  raw[6] = 0xFF;
  raw[7] = 0xFF;
  // ssrc = max
  raw[8] = 0xFF;
  raw[9] = 0xFF;
  raw[10] = 0xFF;
  raw[11] = 0xFF;
  // 4 CSRCs
  for (int i = 0; i < 4; ++i) {
    raw[12 + i * 4 + 0] = 0xAA;
    raw[12 + i * 4 + 1] = 0xBB;
    raw[12 + i * 4 + 2] = static_cast<uint8_t>(i);
    raw[12 + i * 4 + 3] = static_cast<uint8_t>(i + 1);
  }
  // payload
  for (size_t i = 0; i < 20; ++i)
    raw[12 + 16 + i] = static_cast<uint8_t>(i);

  auto pkt = rtp::rtp_packet::parse(raw.data(), raw.size());
  ASSERT(pkt.has_value());
  ASSERT(pkt->csrc_count == 4);
  ASSERT(pkt->csrcs.size() == 4);
  ASSERT(pkt->marker == 1);
  ASSERT(pkt->payload_type == 127);
  ASSERT(pkt->sequence_number == 65535);
  ASSERT(pkt->timestamp == 0xFFFFFFFF);
  ASSERT(pkt->ssrc == 0xFFFFFFFF);
  ASSERT(pkt->csrcs[0] == 0xAABB0001);

  std::cout << "  parse max fields OK\n";
}

static void test_rtp_parse_with_extension() {
  std::vector<uint8_t> raw(12 + 4 + 8 + 10);
  raw[0] = 0x90;  // V=2, P=0, X=1, CC=0
  raw[1] = 0x60;
  raw[2] = 0x00;
  raw[3] = 0x01;
  raw[4] = 0x00;
  raw[5] = 0x00;
  raw[6] = 0x00;
  raw[7] = 0x01;
  raw[8] = 0x12;
  raw[9] = 0x34;
  raw[10] = 0x56;
  raw[11] = 0x78;
  // extension header: profile=0xBEEF, length=2 (in 32-bit words)
  raw[12] = 0xBE;
  raw[13] = 0xEF;
  raw[14] = 0x00;
  raw[15] = 0x02;
  // 8 bytes of extension data (2 * 4)
  for (int i = 0; i < 8; ++i)
    raw[16 + i] = static_cast<uint8_t>(0xAA + i);
  // 10 bytes payload
  for (size_t i = 0; i < 10; ++i)
    raw[24 + i] = static_cast<uint8_t>(i);

  auto pkt = rtp::rtp_packet::parse(raw.data(), raw.size());
  ASSERT(pkt.has_value());
  ASSERT(pkt->extension == 1);
  ASSERT(pkt->extension_profile == 0xBEEF);
  ASSERT(pkt->extension_data.size() == 8);
  ASSERT(pkt->extension_data[0] == 0xAA);
  ASSERT(pkt->extension_data[7] == 0xB1);
  ASSERT(pkt->payload.size() == 10);

  std::cout << "  parse with extension OK\n";
}

static void test_rtp_parse_with_padding() {
  std::vector<uint8_t> raw(12 + 20 + 3);
  raw[0] = 0xA0;  // V=2, P=1, X=0, CC=0
  raw[1] = 0x60;
  raw[2] = 0x00;
  raw[3] = 0x01;
  raw[4] = 0x00;
  raw[5] = 0x00;
  raw[6] = 0x00;
  raw[7] = 0x01;
  raw[8] = 0x12;
  raw[9] = 0x34;
  raw[10] = 0x56;
  raw[11] = 0x78;
  for (size_t i = 0; i < 20; ++i)
    raw[12 + i] = static_cast<uint8_t>(i);
  // 3 bytes padding: 0, 0, 3
  raw[32] = 0x00;
  raw[33] = 0x00;
  raw[34] = 0x03;

  auto pkt = rtp::rtp_packet::parse(raw.data(), raw.size());
  ASSERT(pkt.has_value());
  ASSERT(pkt->padding == 1);
  ASSERT(pkt->payload.size() == 20);

  std::cout << "  parse with padding OK\n";
}

static void test_rtp_parse_bad_input() {
  // Empty
  {
    auto pkt = rtp::rtp_packet::parse(nullptr, 0);
    ASSERT(!pkt.has_value());
  }
  // Too short (< 12)
  {
    uint8_t buf[11] = {};
    auto pkt = rtp::rtp_packet::parse(buf, 11);
    ASSERT(!pkt.has_value());
  }
  // CC=4 but missing CSRCs
  {
    uint8_t buf[13] = {};
    buf[0] = 0x84;  // CC=4
    auto pkt = rtp::rtp_packet::parse(buf, 13);
    ASSERT(!pkt.has_value());
  }
  // Extension set but missing extension header
  {
    uint8_t buf[13] = {};
    buf[0] = 0x90;  // X=1
    buf[1] = 0x60;
    auto pkt = rtp::rtp_packet::parse(buf, 13);
    ASSERT(!pkt.has_value());
  }
  // Padding set but pad_len=0 (invalid)
  {
    std::vector<uint8_t> raw(16);
    raw[0] = 0xA0;  // P=1
    raw[1] = 0x60;
    raw[15] = 0x00;  // pad_len=0
    auto pkt = rtp::rtp_packet::parse(raw.data(), raw.size());
    ASSERT(!pkt.has_value());
  }
  // Padding set but pad_len > payload
  {
    std::vector<uint8_t> raw(16);
    raw[0] = 0xA0;
    raw[1] = 0x60;
    raw[15] = 0xFF;  // pad_len=255
    auto pkt = rtp::rtp_packet::parse(raw.data(), raw.size());
    ASSERT(!pkt.has_value());
  }

  std::cout << "  parse bad input OK\n";
}

static void test_rtp_write_roundtrip() {
  rtp::rtp_packet original;
  original.version = 2;
  original.marker = 1;
  original.payload_type = 96;
  original.sequence_number = 42;
  original.timestamp = 1234567890;
  original.ssrc = 0xDEADBEEF;
  std::vector<uint8_t> payload(30);
  for (size_t i = 0; i < 30; ++i)
    payload[i] = static_cast<uint8_t>(i * 3);
  original.payload = payload;

  std::vector<uint8_t> buf(original.serialized_size());
  int written = original.write_to(buf.data(), buf.size());
  ASSERT(written == static_cast<int>(buf.size()));

  auto parsed = rtp::rtp_packet::parse(buf.data(), buf.size());
  ASSERT(parsed.has_value());
  ASSERT(parsed->version == 2);
  ASSERT(parsed->marker == 1);
  ASSERT(parsed->payload_type == 96);
  ASSERT(parsed->sequence_number == 42);
  ASSERT(parsed->timestamp == 1234567890);
  ASSERT(parsed->ssrc == 0xDEADBEEF);
  ASSERT(parsed->payload.size() == 30);
  ASSERT(0 == std::memcmp(parsed->payload.data(), payload.data(), 30));

  // buffer too small
  int n = original.write_to(buf.data(), 1);
  ASSERT(n == -1);

  std::cout << "  write roundtrip OK\n";
}

static void test_rtp_write_with_csrcs() {
  rtp::rtp_packet original;
  original.csrc_count = 2;
  original.csrcs = {0x11111111, 0x22222222};
  original.payload_type = 100;
  original.sequence_number = 500;
  original.timestamp = 999;
  original.ssrc = 0x33333333;
  std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
  original.payload = payload;

  std::vector<uint8_t> buf(original.serialized_size());
  int written = original.write_to(buf.data(), buf.size());
  ASSERT(written == static_cast<int>(buf.size()));

  auto parsed = rtp::rtp_packet::parse(buf.data(), buf.size());
  ASSERT(parsed.has_value());
  ASSERT(parsed->csrc_count == 2);
  ASSERT(parsed->csrcs.size() == 2);
  ASSERT(parsed->csrcs[0] == 0x11111111);
  ASSERT(parsed->csrcs[1] == 0x22222222);
  ASSERT(parsed->payload.size() == 5);

  std::cout << "  write with CSRCs OK\n";
}

static void test_rtp_write_with_extension() {
  rtp::rtp_packet original;
  original.extension = 1;
  original.extension_profile = 0xABCD;
  uint8_t ext_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  original.extension_data.assign(ext_data, ext_data + 5);
  original.payload_type = 96;
  original.ssrc = 1;
  std::vector<uint8_t> payload = {10, 20, 30};
  original.payload = payload;

  std::vector<uint8_t> buf(original.serialized_size());
  int written = original.write_to(buf.data(), buf.size());
  ASSERT(written == static_cast<int>(buf.size()));

  auto parsed = rtp::rtp_packet::parse(buf.data(), buf.size());
  ASSERT(parsed.has_value());
  ASSERT(parsed->extension == 1);
  ASSERT(parsed->extension_profile == 0xABCD);
  // extension_data is padded to 4-byte boundary on the wire
  ASSERT(parsed->extension_data.size() == 8);
  ASSERT(parsed->extension_data[0] == 0x01);
  ASSERT(parsed->extension_data[4] == 0x05);
  ASSERT(parsed->extension_data[5] == 0x00);
  ASSERT(parsed->payload.size() == 3);

  std::cout << "  write with extension OK\n";
}

static void test_rtp_write_with_padding() {
  rtp::rtp_packet original;
  original.padding = 1;
  original.payload_type = 96;
  original.ssrc = 1;
  std::vector<uint8_t> payload = {1, 2, 3};  // 3 bytes → pad to 4
  original.payload = payload;

  auto size = original.serialized_size();
  ASSERT(size % 4 == 0);
  ASSERT(size == 12 + 3 + 1);  // header + payload + 1 pad byte

  std::vector<uint8_t> buf(size);
  int written = original.write_to(buf.data(), buf.size());
  ASSERT(written == static_cast<int>(buf.size()));
  ASSERT(buf.back() == 1);  // pad_len = 1

  auto parsed = rtp::rtp_packet::parse(buf.data(), buf.size());
  ASSERT(parsed.has_value());
  ASSERT(parsed->padding == 1);
  ASSERT(parsed->payload.size() == 3);

  std::cout << "  write with padding OK\n";
}

static void test_rtp_static_helpers() {
  auto raw = make_basic_rtp(42, 789, 0xCAFEBABE);

  ASSERT(rtp::is_rtp_packet(raw.data(), raw.size()));
  ASSERT(!rtp::is_rtp_packet(nullptr, 0));
  ASSERT(!rtp::is_rtp_packet(raw.data(), 5));
  // wrong version
  std::vector<uint8_t> bad(12);
  bad[0] = 0x00;  // V=0
  bad[1] = 0x60;
  ASSERT(!rtp::is_rtp_packet(bad.data(), bad.size()));

  ASSERT(rtp::rtp_packet::get_payload_type(raw.data()) == 96);
  ASSERT(rtp::rtp_packet::get_sequence_number(raw.data()) == 42);
  ASSERT(rtp::rtp_packet::get_ssrc(raw.data()) == 0xCAFEBABE);

  std::cout << "  static helpers OK\n";
}

static std::vector<uint8_t> make_sr_rtcp(uint8_t rc = 0,
                                         uint32_t ssrc = 0x12345678) {
  // header (4) + SSRC (4) + sender_info (20) + report_blocks (rc * 24)
  std::size_t total = 4 + 4 + 20 + rc * 24;
  std::vector<uint8_t> pkt(total);
  pkt[0] = static_cast<uint8_t>(0x80 | (rc & 0x1F));  // V=2, P=0, RC
  pkt[1] = 200;                                       // PT=SR
  uint16_t length = static_cast<uint16_t>(total / 4 - 1);
  pkt[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
  pkt[3] = static_cast<uint8_t>(length & 0xFF);
  pkt[4] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
  pkt[5] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
  pkt[6] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
  pkt[7] = static_cast<uint8_t>(ssrc & 0xFF);
  // NTP timestamp MSW = 0x00000001
  pkt[8] = 0x00;
  pkt[9] = 0x00;
  pkt[10] = 0x00;
  pkt[11] = 0x01;
  // NTP timestamp LSW = 0x00000002
  pkt[12] = 0x00;
  pkt[13] = 0x00;
  pkt[14] = 0x00;
  pkt[15] = 0x02;
  // RTP timestamp = 100
  pkt[16] = 0x00;
  pkt[17] = 0x00;
  pkt[18] = 0x00;
  pkt[19] = 0x64;
  // packet count = 50
  pkt[20] = 0x00;
  pkt[21] = 0x00;
  pkt[22] = 0x00;
  pkt[23] = 0x32;
  // octet count = 5000
  pkt[24] = 0x00;
  pkt[25] = 0x00;
  pkt[26] = 0x13;
  pkt[27] = 0x88;

  for (uint8_t i = 0; i < rc; ++i) {
    size_t off = 28 + i * 24;
    // SSRC = 0xAABBCCDD + i
    uint32_t rb_ssrc = 0xAABBCCDD + i;
    pkt[off + 0] = static_cast<uint8_t>((rb_ssrc >> 24) & 0xFF);
    pkt[off + 1] = static_cast<uint8_t>((rb_ssrc >> 16) & 0xFF);
    pkt[off + 2] = static_cast<uint8_t>((rb_ssrc >> 8) & 0xFF);
    pkt[off + 3] = static_cast<uint8_t>(rb_ssrc & 0xFF);
    // fraction_lost=2, cumulative_lost=10
    pkt[off + 4] = 0x02;
    pkt[off + 5] = 0x00;
    pkt[off + 6] = 0x00;
    pkt[off + 7] = 0x0A;
    // ext_highest_seq = 1000
    pkt[off + 8] = 0x00;
    pkt[off + 9] = 0x00;
    pkt[off + 10] = 0x03;
    pkt[off + 11] = 0xE8;
    // jitter = 200
    pkt[off + 12] = 0x00;
    pkt[off + 13] = 0x00;
    pkt[off + 14] = 0x00;
    pkt[off + 15] = 0xC8;
    // lsr = 300
    pkt[off + 16] = 0x00;
    pkt[off + 17] = 0x00;
    pkt[off + 18] = 0x01;
    pkt[off + 19] = 0x2C;
    // dlsr = 400
    pkt[off + 20] = 0x00;
    pkt[off + 21] = 0x00;
    pkt[off + 22] = 0x01;
    pkt[off + 23] = 0x90;
  }
  return pkt;
}

static void test_rtcp_parse_sr() {
  {
    auto raw = make_sr_rtcp(0);
    auto pkt = rtcp::rtcp_packet::parse(raw.data(), raw.size());
    ASSERT(pkt.has_value());
    ASSERT(pkt->version == 2);
    ASSERT(pkt->padding == 0);
    ASSERT(pkt->report_count == 0);
    ASSERT(pkt->type == rtcp::packet_type::SR);
    ASSERT(pkt->ssrc == 0x12345678);
    ASSERT(pkt->ntp_timestamp == 0x0000000100000002ULL);
    ASSERT(pkt->rtp_timestamp == 100);
    ASSERT(pkt->sender_packet_count == 50);
    ASSERT(pkt->sender_octet_count == 5000);
    ASSERT(pkt->blocks.empty());
  }

  {
    auto raw = make_sr_rtcp(2);
    auto pkt = rtcp::rtcp_packet::parse(raw.data(), raw.size());
    ASSERT(pkt.has_value());
    ASSERT(pkt->report_count == 2);
    ASSERT(pkt->blocks.size() == 2);
    auto& b0 = pkt->blocks[0];
    ASSERT(b0.ssrc == 0xAABBCCDD);
    ASSERT(b0.fraction_lost == 2);
    ASSERT(b0.cumulative_lost == 10);
    ASSERT(b0.ext_highest_seq == 1000);
    ASSERT(b0.jitter == 200);
    ASSERT(b0.lsr == 300);
    ASSERT(b0.dlsr == 400);
    auto& b1 = pkt->blocks[1];
    ASSERT(b1.ssrc == 0xAABBCCDE);
  }

  std::cout << "  parse SR OK\n";
}

static std::vector<uint8_t> make_rr_rtcp(uint8_t rc = 1,
                                         uint32_t ssrc = 0x11111111) {
  std::size_t total = 4 + 4 + rc * 24;
  std::vector<uint8_t> pkt(total);
  pkt[0] = static_cast<uint8_t>(0x80 | (rc & 0x1F));
  pkt[1] = 201;  // PT=RR
  uint16_t length = static_cast<uint16_t>(total / 4 - 1);
  pkt[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
  pkt[3] = static_cast<uint8_t>(length & 0xFF);
  pkt[4] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
  pkt[5] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
  pkt[6] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
  pkt[7] = static_cast<uint8_t>(ssrc & 0xFF);

  for (uint8_t i = 0; i < rc; ++i) {
    size_t off = 8 + i * 24;
    uint32_t rb_ssrc = 0xCCCC0000 + i;
    pkt[off + 0] = static_cast<uint8_t>((rb_ssrc >> 24) & 0xFF);
    pkt[off + 1] = static_cast<uint8_t>((rb_ssrc >> 16) & 0xFF);
    pkt[off + 2] = static_cast<uint8_t>((rb_ssrc >> 8) & 0xFF);
    pkt[off + 3] = static_cast<uint8_t>(rb_ssrc & 0xFF);
    pkt[off + 4] = 0x03;
    pkt[off + 5] = 0x00;
    pkt[off + 6] = 0x00;
    pkt[off + 7] = 0x14;
  }
  return pkt;
}

static void test_rtcp_parse_rr() {
  auto raw = make_rr_rtcp(1);
  auto pkt = rtcp::rtcp_packet::parse(raw.data(), raw.size());
  ASSERT(pkt.has_value());
  ASSERT(pkt->type == rtcp::packet_type::RR);
  ASSERT(pkt->ssrc == 0x11111111);
  ASSERT(pkt->report_count == 1);
  ASSERT(pkt->blocks.size() == 1);
  ASSERT(pkt->blocks[0].ssrc == 0xCCCC0000);
  ASSERT(pkt->blocks[0].fraction_lost == 3);
  ASSERT(pkt->blocks[0].cumulative_lost == 20);

  std::cout << "  parse RR OK\n";
}

static void test_rtcp_parse_bye() {
  // BYE: header (4) + 2 SSRCs (8) + reason (4) = 16
  // length = 16/4 - 1 = 3
  std::vector<uint8_t> raw(16);
  raw[0] = 0x82;  // V=2, P=0, SC=2
  raw[1] = 203;   // PT=BYE
  raw[2] = 0x00;
  raw[3] = 0x03;  // length=3
  // SSRC 1
  raw[4] = 0xDE;
  raw[5] = 0xAD;
  raw[6] = 0xBE;
  raw[7] = 0xEF;
  // SSRC 2
  raw[8] = 0xCA;
  raw[9] = 0xFE;
  raw[10] = 0xBA;
  raw[11] = 0xBE;
  // reason: 3 bytes "bye"
  raw[12] = 3;
  raw[13] = 'b';
  raw[14] = 'y';
  raw[15] = 'e';

  auto pkt = rtcp::rtcp_packet::parse(raw.data(), raw.size());
  ASSERT(pkt.has_value());
  ASSERT(pkt->type == rtcp::packet_type::BYE);
  ASSERT(pkt->report_count == 2);
  ASSERT(pkt->payload.size() == 12);  // 8 SSRCs + 4 reason bytes

  std::cout << "  parse BYE OK\n";
}

static void test_rtcp_parse_bad_input() {
  {
    auto pkt = rtcp::rtcp_packet::parse(nullptr, 0);
    ASSERT(!pkt.has_value());
  }
  {
    uint8_t buf[3] = {};
    auto pkt = rtcp::rtcp_packet::parse(buf, 3);
    ASSERT(!pkt.has_value());
  }
  {
    // length says 100 but we only have 4 bytes
    uint8_t buf[4] = {};
    buf[0] = 0x80;
    buf[1] = 200;
    buf[2] = 0x00;
    buf[3] = 0x64;  // length=100
    auto pkt = rtcp::rtcp_packet::parse(buf, 4);
    ASSERT(!pkt.has_value());
  }
  {
    // SR with insufficient data for sender info
    uint8_t buf[12] = {};
    buf[0] = 0x80;
    buf[1] = 200;
    buf[2] = 0x00;
    buf[3] = 0x02;  // length=2 → 12 bytes total
    auto pkt = rtcp::rtcp_packet::parse(buf, 12);
    ASSERT(!pkt.has_value());
  }

  std::cout << "  parse bad input OK\n";
}

static void test_rtcp_write_roundtrip_sr() {
  rtcp::rtcp_packet original;
  original.type = rtcp::packet_type::SR;
  original.ssrc = 0xABCDEF01;
  original.ntp_timestamp = 0x0000000200000003ULL;
  original.rtp_timestamp = 999999;
  original.sender_packet_count = 1000;
  original.sender_octet_count = 50000;

  std::vector<uint8_t> buf(original.serialized_size());
  int written = original.write_to(buf.data(), buf.size());
  ASSERT(written == static_cast<int>(buf.size()));

  auto parsed = rtcp::rtcp_packet::parse(buf.data(), buf.size());
  ASSERT(parsed.has_value());
  ASSERT(parsed->type == rtcp::packet_type::SR);
  ASSERT(parsed->ssrc == 0xABCDEF01);
  ASSERT(parsed->ntp_timestamp == 0x0000000200000003ULL);
  ASSERT(parsed->rtp_timestamp == 999999);
  ASSERT(parsed->sender_packet_count == 1000);
  ASSERT(parsed->sender_octet_count == 50000);
  ASSERT(parsed->blocks.empty());

  // buffer too small
  int n = original.write_to(buf.data(), 1);
  ASSERT(n == -1);

  std::cout << "  write roundtrip SR OK\n";
}

static void test_rtcp_write_roundtrip_sr_with_blocks() {
  rtcp::rtcp_packet original;
  original.type = rtcp::packet_type::SR;
  original.report_count = 2;
  original.ssrc = 0x11112222;
  original.ntp_timestamp = 100;
  original.rtp_timestamp = 200;
  original.sender_packet_count = 300;
  original.sender_octet_count = 400;
  original.blocks.push_back({0xAAAAAAAA, 5, 100, 5000, 128, 600, 700});
  original.blocks.push_back({0xBBBBBBBB, 0, 0, 10000, 256, 800, 900});

  std::vector<uint8_t> buf(original.serialized_size());
  int written = original.write_to(buf.data(), buf.size());
  ASSERT(written == static_cast<int>(buf.size()));

  auto parsed = rtcp::rtcp_packet::parse(buf.data(), buf.size());
  ASSERT(parsed.has_value());
  ASSERT(parsed->type == rtcp::packet_type::SR);
  ASSERT(parsed->report_count == 2);
  ASSERT(parsed->ssrc == 0x11112222);
  ASSERT(parsed->blocks.size() == 2);
  ASSERT(parsed->blocks[0].ssrc == 0xAAAAAAAA);
  ASSERT(parsed->blocks[0].fraction_lost == 5);
  ASSERT(parsed->blocks[0].cumulative_lost == 100);
  ASSERT(parsed->blocks[0].ext_highest_seq == 5000);
  ASSERT(parsed->blocks[0].jitter == 128);
  ASSERT(parsed->blocks[0].lsr == 600);
  ASSERT(parsed->blocks[0].dlsr == 700);
  ASSERT(parsed->blocks[1].ssrc == 0xBBBBBBBB);
  ASSERT(parsed->blocks[1].cumulative_lost == 0);

  std::cout << "  write roundtrip SR with blocks OK\n";
}

static void test_rtcp_write_roundtrip_rr() {
  rtcp::rtcp_packet original;
  original.type = rtcp::packet_type::RR;
  original.ssrc = 0x99999999;
  original.report_count = 1;
  original.blocks.push_back({0x55555555, 2, 50, 20000, 64, 100, 150});

  std::vector<uint8_t> buf(original.serialized_size());
  int written = original.write_to(buf.data(), buf.size());
  ASSERT(written == static_cast<int>(buf.size()));

  auto parsed = rtcp::rtcp_packet::parse(buf.data(), buf.size());
  ASSERT(parsed.has_value());
  ASSERT(parsed->type == rtcp::packet_type::RR);
  ASSERT(parsed->ssrc == 0x99999999);
  ASSERT(parsed->blocks.size() == 1);
  ASSERT(parsed->blocks[0].ssrc == 0x55555555);

  std::cout << "  write roundtrip RR OK\n";
}

static void test_rtcp_compound_parse() {
  auto sr = make_sr_rtcp(1);
  auto rr = make_rr_rtcp(0, 0x22222222);
  std::vector<uint8_t> compound;
  compound.insert(compound.end(), sr.begin(), sr.end());
  compound.insert(compound.end(), rr.begin(), rr.end());

  auto packets = rtcp::parse_compound(compound.data(), compound.size());
  ASSERT(packets.size() == 2);
  ASSERT(packets[0].type == rtcp::packet_type::SR);
  ASSERT(packets[0].ssrc == 0x12345678);
  ASSERT(packets[0].blocks.size() == 1);
  ASSERT(packets[1].type == rtcp::packet_type::RR);
  ASSERT(packets[1].ssrc == 0x22222222);

  std::cout << "  compound parse OK\n";
}

static void test_rtcp_write_with_padding() {
  rtcp::rtcp_packet original;
  original.type = rtcp::packet_type::SR;
  original.padding = 1;
  original.ssrc = 0x12345678;
  original.ntp_timestamp = 1;
  original.rtp_timestamp = 2;
  original.sender_packet_count = 3;
  original.sender_octet_count = 4;
  std::vector<uint8_t> payload = {0xAA, 0xBB};  // 2 bytes → pad to 4
  original.payload = payload;

  auto size = original.serialized_size();
  ASSERT(size % 4 == 0);

  std::vector<uint8_t> buf(size);
  int written = original.write_to(buf.data(), buf.size());
  ASSERT(written == static_cast<int>(buf.size()));
  ASSERT(buf.back() >= 1);

  auto parsed = rtcp::rtcp_packet::parse(buf.data(), buf.size());
  ASSERT(parsed.has_value());
  ASSERT(parsed->padding == 1);

  std::cout << "  write with padding OK\n";
}

static void test_rtcp_static_helpers() {
  auto sr = make_sr_rtcp(0);
  ASSERT(rtcp::rtcp_packet::is_rtcp_packet(sr.data(), sr.size()));
  ASSERT(!rtcp::rtcp_packet::is_rtcp_packet(nullptr, 0));
  ASSERT(!rtcp::rtcp_packet::is_rtcp_packet(sr.data(), 2));
  // RTP should NOT match (PT=96 < 192)
  auto rtp = make_basic_rtp();
  ASSERT(!rtcp::rtcp_packet::is_rtcp_packet(rtp.data(), rtp.size()));

  ASSERT(rtcp::rtcp_packet::get_packet_type(sr.data(), sr.size()) ==
         rtcp::packet_type::SR);
  ASSERT(rtcp::rtcp_packet::get_packet_type(nullptr, 0) == 0);

  std::cout << "  static helpers OK\n";
}

int main() {
  std::cout << "RTP:\n";
  test_rtp_parse_basic();
  test_rtp_parse_max_fields();
  test_rtp_parse_with_extension();
  test_rtp_parse_with_padding();
  test_rtp_parse_bad_input();
  test_rtp_write_roundtrip();
  test_rtp_write_with_csrcs();
  test_rtp_write_with_extension();
  test_rtp_write_with_padding();
  test_rtp_static_helpers();

  std::cout << "RTCP:\n";
  test_rtcp_parse_sr();
  test_rtcp_parse_rr();
  test_rtcp_parse_bye();
  test_rtcp_parse_bad_input();
  test_rtcp_write_roundtrip_sr();
  test_rtcp_write_roundtrip_sr_with_blocks();
  test_rtcp_write_roundtrip_rr();
  test_rtcp_compound_parse();
  test_rtcp_write_with_padding();
  test_rtcp_static_helpers();

  std::cout << "All tests passed\n";
  return 0;
}

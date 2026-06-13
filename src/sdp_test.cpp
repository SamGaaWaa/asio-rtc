#include "sdp.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace asiortc;

#define ASSERT(cond)                                                        \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                << "\n";                                                    \
      std::exit(1);                                                         \
    }                                                                       \
  } while (0)

static void test_parse_trivial() {
  auto s = parse_sdp("");
  ASSERT(s.version == 0);
  ASSERT(s.medias.empty());
  ASSERT(s.ice_ufrag.empty());
  ASSERT(s.candidates.empty());

  std::cout << "  parse trivial OK\n";
}

static void test_parse_session_only() {
  std::string sdp =
      "v=0\r\n"
      "o=- 1234567890 987654321 IN IP4 192.168.0.1\r\n"
      "s=TestSession\r\n"
      "t=100 200\r\n"
      "a=ice-ufrag:abcdef1234\r\n"
      "a=ice-pwd:secretpass\r\n";

  auto s = parse_sdp(sdp);
  ASSERT(s.version == 0);
  ASSERT(s.origin.username == "-");
  ASSERT(s.origin.session_id == 1234567890);
  ASSERT(s.origin.session_version == 987654321);
  ASSERT(s.origin.nettype == "IN");
  ASSERT(s.origin.addrtype == "IP4");
  ASSERT(s.origin.addr == "192.168.0.1");
  ASSERT(s.session_name == "TestSession");
  ASSERT(s.timing.start == 100);
  ASSERT(s.timing.stop == 200);
  ASSERT(s.ice_ufrag == "abcdef1234");
  ASSERT(s.ice_pwd == "secretpass");
  ASSERT(s.medias.empty());

  std::cout << "  parse session only OK\n";
}

static void test_parse_video_offer() {
  std::string sdp =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0\r\n"
      "a=msid-semantic: WMS stream1\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=sendrecv\r\n"
      "a=rtcp-mux\r\n"
      "a=ice-ufrag:offerUfrag123\r\n"
      "a=ice-pwd:offerPwd456\r\n"
      "a=fingerprint:sha-256 AB:CD:EF:01:02:03:04:05:06\r\n"
      "a=setup:actpass\r\n"
      "a=candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host\r\n"
      "a=rtpmap:96 VP8/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=fmtp:97 apt=96\r\n"
      "a=rtcp-fb:96 nack\r\n"
      "a=ssrc:1234567890 cname:test\r\n"
      "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n";

  auto s = parse_sdp(sdp);
  ASSERT(s.version == 0);
  ASSERT(s.bundle_groups.size() == 1);
  ASSERT(s.bundle_groups[0] == "0");
  ASSERT(s.medias.size() == 1);

  auto& m = s.medias[0];
  ASSERT(m.media_type == "video");
  ASSERT(m.port == 9);
  ASSERT(m.proto == "UDP/TLS/RTP/SAVPF");
  ASSERT(m.payload_types.size() == 2);
  ASSERT(m.payload_types[0] == 96);
  ASSERT(m.payload_types[1] == 97);

  ASSERT(m.mid == "0");
  ASSERT(m.direction == sdp_direction::sendrecv);
  ASSERT(m.rtcp_mux == true);
  ASSERT(m.ice_ufrag == "offerUfrag123");
  ASSERT(m.ice_pwd == "offerPwd456");
  ASSERT(m.fingerprint == "sha-256 AB:CD:EF:01:02:03:04:05:06");
  ASSERT(m.setup == "actpass");
  ASSERT(m.candidates.size() == 1);
  ASSERT(m.candidates[0] ==
         "candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host");

  ASSERT(m.rtpmaps.size() == 2);
  ASSERT(m.rtpmaps[0].payload_type == 96);
  ASSERT(m.rtpmaps[0].name == "VP8");
  ASSERT(m.rtpmaps[0].clock_rate == 90000);
  ASSERT(m.rtpmaps[0].encoding_params.empty());

  ASSERT(m.rtpmaps[1].payload_type == 97);
  ASSERT(m.rtpmaps[1].name == "rtx");
  ASSERT(m.rtpmaps[1].clock_rate == 90000);

  ASSERT(m.fmtps.size() == 1);
  ASSERT(m.fmtps[0] == "97 apt=96");

  ASSERT(m.ssrcs.size() == 1);
  ASSERT(m.ssrcs[0] == "1234567890 cname:test");

  ASSERT(m.extmaps.size() == 1);

  // Check that rtcp-fb is in generic attributes
  bool found_rtcp_fb = false;
  for (auto& [name, value] : m.attributes) {
    if (name == "rtcp-fb" && value == "96 nack") {
      found_rtcp_fb = true;
      break;
    }
  }
  ASSERT(found_rtcp_fb);

  std::cout << "  parse video offer OK\n";
}

static void test_parse_audio_opus() {
  std::string sdp =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:1\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
      "a=sendrecv\r\n"
      "a=ice-ufrag:audioUfrag\r\n"
      "a=ice-pwd:audioPwd\r\n"
      "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
      "a=setup:active\r\n";

  auto s = parse_sdp(sdp);
  ASSERT(s.medias.size() == 1);
  auto& m = s.medias[0];
  ASSERT(m.media_type == "audio");
  ASSERT(m.payload_types.size() == 1);
  ASSERT(m.payload_types[0] == 111);

  ASSERT(m.rtpmaps.size() == 1);
  ASSERT(m.rtpmaps[0].payload_type == 111);
  ASSERT(m.rtpmaps[0].name == "opus");
  ASSERT(m.rtpmaps[0].clock_rate == 48000);
  ASSERT(m.rtpmaps[0].encoding_params == "2");

  ASSERT(m.fmtps.size() == 1);
  ASSERT(m.fmtps[0] == "111 minptime=10;useinbandfec=1");

  std::cout << "  parse audio opus OK\n";
}

static void test_parse_multiple_media() {
  std::string sdp =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0 1 2\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:96 VP8/90000\r\n"
      "a=sendonly\r\n"
      "a=ice-ufrag:ufrag1\r\n"
      "a=ice-pwd:pwd1\r\n"
      "a=fingerprint:sha-256 FINGER1\r\n"
      "a=setup:active\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:1\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=recvonly\r\n"
      "a=ice-ufrag:ufrag2\r\n"
      "a=ice-pwd:pwd2\r\n"
      "a=fingerprint:sha-256 FINGER2\r\n"
      "a=setup:passive\r\n"
      "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:2\r\n"
      "a=sctp-port:5000\r\n"
      "a=ice-ufrag:ufrag3\r\n"
      "a=ice-pwd:pwd3\r\n"
      "a=fingerprint:sha-256 FINGER3\r\n"
      "a=setup:actpass\r\n";

  auto s = parse_sdp(sdp);
  ASSERT(s.bundle_groups.size() == 3);
  ASSERT(s.bundle_groups[0] == "0");
  ASSERT(s.bundle_groups[1] == "1");
  ASSERT(s.bundle_groups[2] == "2");

  ASSERT(s.medias.size() == 3);

  auto& video = s.medias[0];
  ASSERT(video.media_type == "video");
  ASSERT(video.mid == "0");
  ASSERT(video.direction == sdp_direction::sendonly);
  ASSERT(video.ice_ufrag == "ufrag1");

  auto& audio = s.medias[1];
  ASSERT(audio.media_type == "audio");
  ASSERT(audio.mid == "1");
  ASSERT(audio.direction == sdp_direction::recvonly);
  ASSERT(audio.ice_ufrag == "ufrag2");

  auto& data = s.medias[2];
  ASSERT(data.media_type == "application");
  ASSERT(data.mid == "2");
  ASSERT(data.proto == "UDP/DTLS/SCTP");
  ASSERT(data.sctp_port == 5000);
  ASSERT(data.ice_ufrag == "ufrag3");

  std::cout << "  parse multiple media OK\n";
}

static void test_parse_session_level_attrs() {
  std::string sdp =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=ice-ufrag:sessUfrag\r\n"
      "a=ice-pwd:sessPwd\r\n"
      "a=fingerprint:sha-256 SESSION_FP\r\n"
      "a=setup:passive\r\n"
      "a=ice-lite\r\n"
      "a=candidate:1 1 UDP 2130706431 10.0.0.1 9999 typ host\r\n"
      "a=mid:data\r\n"
      "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=sctp-port:5000\r\n"
      "a=mid:0\r\n";

  auto s = parse_sdp(sdp);
  ASSERT(s.ice_ufrag == "sessUfrag");
  ASSERT(s.ice_pwd == "sessPwd");
  ASSERT(s.fingerprint == "sha-256 SESSION_FP");
  ASSERT(s.setup == "passive");
  ASSERT(s.mid == "data");
  ASSERT(s.candidates.size() == 1);
  ASSERT(s.candidates[0] ==
         "candidate:1 1 UDP 2130706431 10.0.0.1 9999 typ host");

  // ice-lite is stored as generic attribute
  bool found_ice_lite = false;
  for (auto& [name, value] : s.attributes) {
    if (name == "ice-lite" && value.empty()) {
      found_ice_lite = true;
      break;
    }
  }
  ASSERT(found_ice_lite);

  ASSERT(s.medias.size() == 1);

  std::cout << "  parse session-level attrs OK\n";
}

static void test_parse_lf_only() {
  std::string sdp =
      "v=0\n"
      "o=- 0 0 IN IP4 0.0.0.0\n"
      "s=-\n"
      "t=0 0\n"
      "a=group:BUNDLE 0\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\n"
      "c=IN IP4 0.0.0.0\n"
      "a=mid:0\n"
      "a=rtpmap:96 VP8/90000\n"
      "a=sendrecv\n"
      "a=ice-ufrag:lfUfrag\n"
      "a=ice-pwd:lfPwd\n"
      "a=fingerprint:sha-256 LF:FINGER:PRINT\n"
      "a=setup:active\n"
      "a=candidate:1 1 UDP 2130706431 10.0.0.1 4444 typ host\n";

  auto s = parse_sdp(sdp);
  ASSERT(s.version == 0);
  ASSERT(s.medias.size() == 1);
  ASSERT(s.medias[0].ice_ufrag == "lfUfrag");
  ASSERT(s.medias[0].candidates.size() == 1);
  ASSERT(s.medias[0].rtpmaps.size() == 1);

  std::cout << "  parse LF only OK\n";
}

static void test_parse_datachannel() {
  std::string sdp =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=sendrecv\r\n"
      "a=sctp-port:5000\r\n"
      "a=ice-ufrag:dcUfrag\r\n"
      "a=ice-pwd:dcPwd\r\n"
      "a=fingerprint:sha-256 DC:FINGER:PRINT\r\n"
      "a=setup:actpass\r\n"
      "a=candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host\r\n"
      "a=max-message-size:262144\r\n";

  auto s = parse_sdp(sdp);
  ASSERT(s.medias.size() == 1);
  auto& m = s.medias[0];
  ASSERT(m.media_type == "application");
  ASSERT(m.proto == "UDP/DTLS/SCTP");
  ASSERT(m.sctp_port == 5000);
  ASSERT(m.ice_ufrag == "dcUfrag");
  ASSERT(m.ice_pwd == "dcPwd");
  ASSERT(m.fingerprint == "sha-256 DC:FINGER:PRINT");
  ASSERT(m.candidates.size() == 1);

  bool found_max_msg = false;
  for (auto& [name, value] : m.attributes) {
    if (name == "max-message-size" && value == "262144") {
      found_max_msg = true;
      break;
    }
  }
  ASSERT(found_max_msg);

  std::cout << "  parse datachannel OK\n";
}

static void test_write_basic() {
  session_description s;
  s.version = 0;
  s.origin.username = "-";
  s.origin.session_id = 123;
  s.origin.session_version = 456;
  s.origin.nettype = "IN";
  s.origin.addrtype = "IP4";
  s.origin.addr = "0.0.0.0";
  s.session_name = "-";
  s.timing.start = 0;
  s.timing.stop = 0;

  auto str = s.to_string();
  ASSERT(str.starts_with("v=0\r\n"));
  ASSERT(str.find("o=- 123 456 IN IP4 0.0.0.0\r\n") != std::string::npos);
  ASSERT(str.find("s=-\r\n") != std::string::npos);
  ASSERT(str.find("t=0 0\r\n") != std::string::npos);

  std::cout << "  write basic OK\n";
}

static void test_write_with_media() {
  session_description s;
  s.version = 0;
  s.origin.username = "-";
  s.origin.session_id = 0;
  s.origin.session_version = 0;
  s.origin.nettype = "IN";
  s.origin.addrtype = "IP4";
  s.origin.addr = "0.0.0.0";
  s.session_name = "-";
  s.timing.start = 0;
  s.timing.stop = 0;
  s.bundle_groups = {"0"};

  sdp_media video;
  video.media_type = "video";
  video.port = 9;
  video.proto = "UDP/TLS/RTP/SAVPF";
  video.payload_types = {96, 97};
  video.conn_nettype = "IN";
  video.conn_addrtype = "IP4";
  video.conn_addr = "0.0.0.0";
  video.mid = "0";
  video.direction = sdp_direction::sendrecv;
  video.rtcp_mux = true;
  video.rtpmaps = {
      {96, "VP8", 90000, ""},
      {97, "rtx", 90000, ""},
  };
  video.ice_ufrag = "testUfrag";
  video.ice_pwd = "testPwd";
  video.fingerprint = "sha-256 AA:BB:CC";
  video.setup = "active";
  video.candidates = {
      "candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host"};
  s.medias.push_back(std::move(video));

  auto str = s.to_string();
  ASSERT(str.find("a=group:BUNDLE 0\r\n") != std::string::npos);
  ASSERT(str.find("m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n") !=
         std::string::npos);
  ASSERT(str.find("a=mid:0\r\n") != std::string::npos);
  ASSERT(str.find("a=sendrecv\r\n") != std::string::npos);
  ASSERT(str.find("a=rtcp-mux\r\n") != std::string::npos);
  ASSERT(str.find("a=rtpmap:96 VP8/90000\r\n") != std::string::npos);
  ASSERT(str.find("a=rtpmap:97 rtx/90000\r\n") != std::string::npos);
  ASSERT(str.find("a=ice-ufrag:testUfrag\r\n") != std::string::npos);
  ASSERT(str.find("a=ice-pwd:testPwd\r\n") != std::string::npos);
  ASSERT(str.find("a=fingerprint:sha-256 AA:BB:CC\r\n") != std::string::npos);
  ASSERT(str.find("a=setup:active\r\n") != std::string::npos);
  ASSERT(str.find(
             "a=candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host\r\n") !=
         std::string::npos);

  std::cout << "  write with media OK\n";
}

static void test_write_with_datachannel() {
  session_description s;
  s.version = 0;
  s.origin.username = "-";
  s.origin.session_id = 0;
  s.origin.session_version = 0;
  s.origin.nettype = "IN";
  s.origin.addrtype = "IP4";
  s.origin.addr = "0.0.0.0";
  s.session_name = "-";
  s.timing.start = 0;
  s.timing.stop = 0;

  sdp_media dc;
  dc.media_type = "application";
  dc.port = 9;
  dc.proto = "UDP/DTLS/SCTP";
  dc.payload_types = {};
  dc.conn_nettype = "IN";
  dc.conn_addrtype = "IP4";
  dc.conn_addr = "0.0.0.0";
  dc.mid = "0";
  dc.sctp_port = 5000;
  dc.ice_ufrag = "dcUfrag";
  dc.ice_pwd = "dcPwd";
  dc.fingerprint = "sha-256 DC:FP";
  dc.setup = "actpass";
  s.medias.push_back(std::move(dc));

  auto str = s.to_string();
  ASSERT(str.find("m=application 9 UDP/DTLS/SCTP\r\n") != std::string::npos);
  ASSERT(str.find("a=sctp-port:5000\r\n") != std::string::npos);

  std::cout << "  write with datachannel OK\n";
}

static void test_roundtrip_video() {
  std::string original =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=sendrecv\r\n"
      "a=rtcp-mux\r\n"
      "a=ice-ufrag:testUfrag\r\n"
      "a=ice-pwd:testPwd\r\n"
      "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
      "a=setup:active\r\n"
      "a=candidate:1 1 UDP 2130706431 192.168.1.1 12345 typ host\r\n"
      "a=rtpmap:96 VP8/90000\r\n"
      "a=rtpmap:97 rtx/90000\r\n"
      "a=ssrc:1234567890 cname:test\r\n";

  auto parsed = parse_sdp(original);
  ASSERT(parsed.medias.size() == 1);

  auto serialized = parsed.to_string();
  auto reparsed = parse_sdp(serialized);

  ASSERT(reparsed.version == parsed.version);
  ASSERT(reparsed.medias.size() == 1);
  ASSERT(reparsed.medias[0].media_type == "video");
  ASSERT(reparsed.medias[0].port == 9);
  ASSERT(reparsed.medias[0].proto == "UDP/TLS/RTP/SAVPF");
  ASSERT(reparsed.medias[0].mid == "0");
  ASSERT(reparsed.medias[0].direction == sdp_direction::sendrecv);
  ASSERT(reparsed.medias[0].rtcp_mux);
  ASSERT(reparsed.medias[0].ice_ufrag == "testUfrag");
  ASSERT(reparsed.medias[0].ice_pwd == "testPwd");
  ASSERT(reparsed.medias[0].fingerprint == "sha-256 AA:BB:CC:DD");
  ASSERT(reparsed.medias[0].setup == "active");
  ASSERT(reparsed.medias[0].candidates.size() == 1);
  ASSERT(reparsed.medias[0].rtpmaps.size() == 2);
  ASSERT(reparsed.medias[0].ssrcs.size() == 1);
  ASSERT(reparsed.bundle_groups.size() == 1);

  std::cout << "  roundtrip video OK\n";
}

static void test_roundtrip_datachannel() {
  std::string original =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=sendrecv\r\n"
      "a=sctp-port:5000\r\n"
      "a=ice-ufrag:dcUfrag\r\n"
      "a=ice-pwd:dcPwd\r\n"
      "a=fingerprint:sha-256 DC:FP\r\n"
      "a=setup:actpass\r\n"
      "a=candidate:1 1 UDP 2130706431 10.0.0.1 9999 typ host\r\n";

  auto parsed = parse_sdp(original);
  ASSERT(parsed.medias.size() == 1);
  ASSERT(parsed.medias[0].media_type == "application");
  ASSERT(parsed.medias[0].sctp_port == 5000);

  auto serialized = parsed.to_string();
  auto reparsed = parse_sdp(serialized);

  ASSERT(reparsed.medias.size() == 1);
  ASSERT(reparsed.medias[0].media_type == "application");
  ASSERT(reparsed.medias[0].sctp_port == 5000);
  ASSERT(reparsed.medias[0].ice_ufrag == "dcUfrag");
  ASSERT(reparsed.medias[0].ice_pwd == "dcPwd");
  ASSERT(reparsed.medias[0].fingerprint == "sha-256 DC:FP");
  ASSERT(reparsed.medias[0].setup == "actpass");

  std::cout << "  roundtrip datachannel OK\n";
}

static void test_roundtrip_session_level_attrs() {
  std::string original =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=ice-ufrag:sessLevelUfrag\r\n"
      "a=ice-pwd:sessLevelPwd\r\n"
      "a=fingerprint:sha-256 SESSION_FP\r\n"
      "a=setup:passive\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=rtpmap:96 VP8/90000\r\n"
      "a=rtcp-mux\r\n";

  auto parsed = parse_sdp(original);
  ASSERT(parsed.ice_ufrag == "sessLevelUfrag");
  ASSERT(parsed.ice_pwd == "sessLevelPwd");
  ASSERT(parsed.fingerprint == "sha-256 SESSION_FP");
  ASSERT(parsed.setup == "passive");
  ASSERT(parsed.bundle_groups.size() == 2);

  auto serialized = parsed.to_string();
  auto reparsed = parse_sdp(serialized);

  ASSERT(reparsed.ice_ufrag == "sessLevelUfrag");
  ASSERT(reparsed.ice_pwd == "sessLevelPwd");
  ASSERT(reparsed.fingerprint == "sha-256 SESSION_FP");
  ASSERT(reparsed.setup == "passive");
  ASSERT(reparsed.bundle_groups.size() == 2);

  std::cout << "  roundtrip session-level attrs OK\n";
}

static void test_direction_mapping() {
  // sendrecv
  {
    std::string sdp =
        "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "c=IN IP4 0.0.0.0\r\na=sendrecv\r\n";
    auto s = parse_sdp(sdp);
    ASSERT(s.medias[0].direction == sdp_direction::sendrecv);
  }
  // sendonly
  {
    std::string sdp =
        "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 0.0.0.0\r\na=sendonly\r\n";
    auto s = parse_sdp(sdp);
    ASSERT(s.medias[0].direction == sdp_direction::sendonly);
  }
  // recvonly
  {
    std::string sdp =
        "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "c=IN IP4 0.0.0.0\r\na=recvonly\r\n";
    auto s = parse_sdp(sdp);
    ASSERT(s.medias[0].direction == sdp_direction::recvonly);
  }
  // inactive
  {
    std::string sdp =
        "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "c=IN IP4 0.0.0.0\r\na=inactive\r\n";
    auto s = parse_sdp(sdp);
    ASSERT(s.medias[0].direction == sdp_direction::inactive);
  }

  std::cout << "  direction mapping OK\n";
}

static void test_write_directions() {
  auto check_written = [](sdp_direction dir, const std::string& expected) {
    session_description s;
    s.version = 0;
    s.origin.username = "-";
    s.origin.session_id = 0;
    s.origin.session_version = 0;
    s.origin.nettype = "IN";
    s.origin.addrtype = "IP4";
    s.origin.addr = "0.0.0.0";
    s.session_name = "-";
    s.timing.start = 0;
    s.timing.stop = 0;
    sdp_media m;
    m.media_type = "video";
    m.port = 9;
    m.proto = "UDP/TLS/RTP/SAVPF";
    m.payload_types = {96};
    m.conn_nettype = "IN";
    m.conn_addrtype = "IP4";
    m.conn_addr = "0.0.0.0";
    m.direction = dir;
    s.medias.push_back(std::move(m));
    auto str = s.to_string();
    ASSERT(str.find("a=" + expected + "\r\n") != std::string::npos);
  };

  check_written(sdp_direction::sendrecv, "sendrecv");
  check_written(sdp_direction::sendonly, "sendonly");
  check_written(sdp_direction::recvonly, "recvonly");
  check_written(sdp_direction::inactive, "inactive");

  std::cout << "  write directions OK\n";
}

static void test_rtpmap_encoding_params() {
  std::string sdp =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtpmap:111 opus/48000/2\r\n";

  auto s = parse_sdp(sdp);
  ASSERT(s.medias[0].rtpmaps[0].encoding_params == "2");

  auto str = s.to_string();
  ASSERT(str.find("opus/48000/2") != std::string::npos);

  std::cout << "  rtpmap encoding params OK\n";
}

static void test_invalid_rtpmap_parse() {
  // No space between PT and codec
  std::string sdp_no_space =
      "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\na=rtpmap:96VP8/90000\r\n";
  auto s = parse_sdp(sdp_no_space);
  ASSERT(s.medias[0].rtpmaps.empty());

  // No clock rate
  std::string sdp_no_clock =
      "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\na=rtpmap:96 VP8\r\n";
  auto s2 = parse_sdp(sdp_no_clock);
  ASSERT(s2.medias[0].rtpmaps.empty());

  // Empty value
  std::string sdp_empty_val =
      "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\na=rtpmap:\r\n";
  auto s3 = parse_sdp(sdp_empty_val);
  ASSERT(s3.medias[0].rtpmaps.empty());

  std::cout << "  invalid rtpmap parse OK\n";
}

static void test_unknown_attributes_preserved() {
  std::string sdp =
      "v=0\r\n"
      "o=- 0 0 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=x-custom:value123\r\n"
      "a=another-custom\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=x-media-custom:foo\r\n";

  auto s = parse_sdp(sdp);

  bool found_sess = false;
  bool found_sess_no_val = false;
  for (auto& [name, value] : s.attributes) {
    if (name == "x-custom" && value == "value123")
      found_sess = true;
    if (name == "another-custom" && value.empty())
      found_sess_no_val = true;
  }
  ASSERT(found_sess);
  ASSERT(found_sess_no_val);

  bool found_media = false;
  for (auto& [name, value] : s.medias[0].attributes) {
    if (name == "x-media-custom" && value == "foo")
      found_media = true;
  }
  ASSERT(found_media);

  // Roundtrip preserves them
  auto str = s.to_string();
  auto reparsed = parse_sdp(str);

  bool found_sess2 = false;
  bool found_media2 = false;
  for (auto& [name, value] : reparsed.attributes) {
    if (name == "x-custom" && value == "value123")
      found_sess2 = true;
  }
  for (auto& [name, value] : reparsed.medias[0].attributes) {
    if (name == "x-media-custom" && value == "foo")
      found_media2 = true;
  }
  ASSERT(found_sess2);
  ASSERT(found_media2);

  std::cout << "  unknown attributes preserved OK\n";
}

int main() {
  std::cout << "SDP:\n";
  test_parse_trivial();
  test_parse_session_only();
  test_parse_video_offer();
  test_parse_audio_opus();
  test_parse_multiple_media();
  test_parse_session_level_attrs();
  test_parse_lf_only();
  test_parse_datachannel();

  std::cout << "Write:\n";
  test_write_basic();
  test_write_with_media();
  test_write_with_datachannel();

  std::cout << "Roundtrip:\n";
  test_roundtrip_video();
  test_roundtrip_datachannel();
  test_roundtrip_session_level_attrs();

  std::cout << "Edge cases:\n";
  test_direction_mapping();
  test_write_directions();
  test_rtpmap_encoding_params();
  test_invalid_rtpmap_parse();
  test_unknown_attributes_preserved();

  std::cout << "All tests passed\n";
  return 0;
}

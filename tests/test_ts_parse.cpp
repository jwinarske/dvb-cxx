// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// Unit test for the pure TS/PSI parsing (no DVB hardware required). Hand-builds
// a PAT, a PMT, and a video TS packet and checks the parsers recover them.
#include "ts/ts_parse.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace tv::ts;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// Wrap a PSI section body (the bytes from byte 3 onward, excluding CRC) in the
// table_id + section_length header and a dummy 4-byte CRC.
std::vector<uint8_t> make_section(uint8_t tid, std::vector<uint8_t> body) {
  const size_t section_length = body.size() + 4;  // body + CRC
  std::vector<uint8_t> s = {
      tid, static_cast<uint8_t>(0xb0 | ((section_length >> 8) & 0x0f)),
      static_cast<uint8_t>(section_length & 0xff)};
  s.insert(s.end(), body.begin(), body.end());
  s.insert(s.end(), {0xde, 0xad, 0xbe, 0xef});  // dummy CRC (parser drops it)
  return s;
}

void test_pat() {
  // One program: program_number 1 -> PMT pid 0x1000 (0xF0,0x00 with reserved
  // bits set). A network-PID entry (program 0) must be skipped.
  const std::vector<uint8_t> body = {
      0x00, 0x01,              // transport_stream_id
      0xc1,                    // version 0, current
      0x00, 0x00,              // section_number, last_section_number
      0x00, 0x00, 0xe0, 0x10,  // program 0 (network PID) -> skipped
      0x00, 0x01, 0xf0, 0x00,  // program 1 -> PMT pid 0x1000
  };
  const auto pat = make_section(0x00, body);

  std::vector<ProgramInfo> progs;
  parse_pat(pat.data(), pat.size(), progs);
  CHECK(progs.size() == 1);
  if (progs.size() == 1) {
    CHECK(progs[0].program_number == 1);
    CHECK(progs[0].pmt_pid == 0x1000);
  }
}

void test_pmt() {
  // PCR pid 0x0100; video stream_type 0x02 (MPEG-2) pid 0x0100; audio
  // stream_type 0x81 (AC-3) pid 0x0101.
  const std::vector<uint8_t> body = {
      0x00, 0x01,                    // program_number 1
      0xc1, 0x00, 0x00,              // version/current, section numbers
      0xe1, 0x00,                    // PCR pid 0x0100
      0xf0, 0x00,                    // program_info_length 0
      0x02, 0xe1, 0x00, 0xf0, 0x00,  // video: MPEG-2, pid 0x0100
      0x81, 0xe1, 0x01, 0xf0, 0x00,  // audio: AC-3,  pid 0x0101
  };
  const auto pmt = make_section(0x02, body);

  ProgramInfo pi;
  parse_pmt(pmt.data(), pmt.size(), pi);
  CHECK(pi.video_pid == 0x0100);
  CHECK(pi.video_codec == VideoCodec::Mpeg2);
  CHECK(pi.audio_pid == 0x0101);
}

void test_pes_depacketizer() {
  // A 188-byte TS packet for pid 0x0100, PUSI set, carrying a 9-byte PES header
  // (no optional fields) followed by a known ES payload.
  std::array<uint8_t, kTsPacket> pkt{};
  pkt[0] = kTsSync;
  pkt[1] = 0x40 | 0x01;  // PUSI + pid hi (0x0100)
  pkt[2] = 0x00;         // pid lo
  pkt[3] = 0x10;         // afc = payload only, cc 0
  const uint8_t pes_hdr[9] = {0x00, 0x00, 0x01, 0xe0, 0x00,
                              0x00, 0x80, 0x00, 0x00};
  for (int i = 0; i < 9; ++i)
    pkt[4 + i] = pes_hdr[i];
  const size_t es_off = 4 + 9;
  const size_t es_len = kTsPacket - es_off;  // 175 bytes of ES
  for (size_t i = 0; i < es_len; ++i)
    pkt[es_off + i] = static_cast<uint8_t>(i & 0xff);

  std::vector<uint8_t> got;
  const PesDepacketizer dep(0x0100);
  dep.feed(pkt.data(), [&](const uint8_t* d, size_t n, uint64_t) {
    got.insert(got.end(), d, d + n);
  });
  CHECK(got.size() == es_len);
  bool match = got.size() == es_len;
  for (size_t i = 0; match && i < es_len; ++i)
    match = got[i] == static_cast<uint8_t>(i & 0xff);
  CHECK(match);

  // A packet on a different PID must be ignored.
  std::vector<uint8_t> got2;
  const PesDepacketizer dep_other(0x0200);
  dep_other.feed(pkt.data(), [&](const uint8_t* d, size_t n, uint64_t) {
    got2.insert(got2.end(), d, d + n);
  });
  CHECK(got2.empty());
}

}  // namespace

int main() {
  test_pat();
  test_pmt();
  test_pes_depacketizer();
  if (g_failures == 0)
    std::printf("ts_parse: ALL TESTS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}

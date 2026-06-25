// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "ts/ts_parse.h"

namespace tv::ts {

VideoCodec codec_of_stream_type(uint8_t st) {
  switch (st) {
    case 0x01:  // MPEG-1 video
    case 0x02:  // MPEG-2 video (ATSC 1.0)
      return VideoCodec::Mpeg2;
    case 0x1b:  // H.264
      return VideoCodec::H264;
    case 0x24:  // HEVC
      return VideoCodec::Hevc;
    default:
      return VideoCodec::Unknown;
  }
}

namespace {

bool is_audio_stream_type(uint8_t st) {
  // MPEG audio, AAC (ADTS/LATM), AC-3 (ATSC private), E-AC-3.
  return st == 0x03 || st == 0x04 || st == 0x0f || st == 0x11 || st == 0x81 ||
         st == 0x87;
}

}  // namespace

void parse_pat(const uint8_t* s, size_t len, std::vector<ProgramInfo>& out) {
  if (len < 12 || s[0] != 0x00)
    return;
  const size_t section_length = ((s[1] & 0x0f) << 8) | s[2];
  // Untrusted broadcast data: a PAT body is >= 9 bytes (5-byte header after the
  // length field + 4-byte CRC). Reject shorter so `end` below can't underflow
  // and the entry loop can't read out of bounds.
  if (section_length < 9 || 3 + section_length > len)
    return;
  const size_t end = (3 + section_length) - 4;  // drop the 32-bit CRC
  for (size_t i = 8; i + 4 <= end; i += 4) {
    const uint16_t prog = (s[i] << 8) | s[i + 1];
    const uint16_t pid = ((s[i + 2] & 0x1f) << 8) | s[i + 3];
    if (prog != 0) {
      ProgramInfo pi;
      pi.program_number = prog;
      pi.pmt_pid = pid;
      out.push_back(pi);
    }
  }
}

void parse_pmt(const uint8_t* s, size_t len, ProgramInfo& pi) {
  if (len < 16 || s[0] != 0x02)
    return;
  const size_t section_length = ((s[1] & 0x0f) << 8) | s[2];
  // Untrusted broadcast data: a PMT body is >= 13 bytes (9-byte header after
  // the length field + 4-byte CRC). Reject shorter so `end` can't underflow.
  if (section_length < 13 || 3 + section_length > len)
    return;
  const size_t end = (3 + section_length) - 4;  // drop the 32-bit CRC
  const size_t program_info_length = ((s[10] & 0x0f) << 8) | s[11];
  size_t i = 12 + program_info_length;
  while (i + 5 <= end) {
    const uint8_t stream_type = s[i];
    const uint16_t pid = ((s[i + 1] & 0x1f) << 8) | s[i + 2];
    const size_t es_info_length = ((s[i + 3] & 0x0f) << 8) | s[i + 4];
    const VideoCodec vc = codec_of_stream_type(stream_type);
    if (vc != VideoCodec::Unknown && pi.video_pid == 0) {
      pi.video_pid = pid;
      pi.video_codec = vc;
    } else if (is_audio_stream_type(stream_type) && pi.audio_pid == 0) {
      pi.audio_pid = pid;
    }
    i += 5 + es_info_length;
  }
}

void PesDepacketizer::feed(const uint8_t* pkt, const EsSink& sink) const {
  if (pkt[0] != kTsSync)
    return;
  const uint16_t pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
  if (pid != pid_)
    return;
  const bool pusi = (pkt[1] & 0x40) != 0;
  const uint8_t afc = (pkt[3] >> 4) & 0x3;
  if ((afc & 0x1) == 0)
    return;  // no payload (adaptation field only)

  size_t poff = 4;
  if ((afc & 0x2) != 0)
    poff += 1 + pkt[4];  // skip the adaptation field
  if (poff >= kTsPacket)
    return;

  const uint8_t* payload = pkt + poff;
  size_t plen = static_cast<size_t>(kTsPacket) - poff;

  if (pusi) {
    // PES header: 00 00 01, stream_id, length(2), flags(2), hdr_len(1), then
    // hdr_len bytes of optional fields (PTS/DTS, …); the ES follows. Guard a
    // malformed/short header.
    if (plen >= 9 && payload[0] == 0x00 && payload[1] == 0x00 &&
        payload[2] == 0x01) {
      const size_t pes_hdr = 9 + payload[8];
      if (pes_hdr < plen) {
        payload += pes_hdr;
        plen -= pes_hdr;
      } else {
        plen = 0;
      }
    }
  }
  if (plen > 0)
    sink(payload, plen, 0);
}

}  // namespace tv::ts

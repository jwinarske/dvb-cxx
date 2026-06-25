// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Pure MPEG-2 transport-stream / PSI parsing, split out from ts_demux so it can
// be unit-tested without any DVB hardware. No syscalls, no device state.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace tv::ts {

enum class VideoCodec { Mpeg2, H264, Hevc, Unknown };

struct ProgramInfo {
  uint16_t program_number = 0;
  uint16_t pmt_pid = 0;
  uint16_t video_pid = 0;
  VideoCodec video_codec = VideoCodec::Unknown;
  uint16_t audio_pid = 0;  // 0 if none found
};

// Callback for elementary-stream bytes: (data, len, pts_ns). pts_ns is 0 when
// no PTS was extracted.
using EsSink = std::function<void(const uint8_t*, size_t, uint64_t)>;

// MPEG ES stream_type -> video codec (Unknown if not a supported video type).
VideoCodec codec_of_stream_type(uint8_t stream_type);

// Parse a PAT section (table_id 0x00): append a ProgramInfo (program_number +
// pmt_pid) per program, skipping the network-PID entry.
void parse_pat(const uint8_t* section,
               size_t len,
               std::vector<ProgramInfo>& out);

// Parse a PMT section (table_id 0x02) into `pi`: first video stream (PID +
// codec) and first audio PID.
void parse_pmt(const uint8_t* section, size_t len, ProgramInfo& pi);

constexpr int kTsPacket = 188;
constexpr uint8_t kTsSync = 0x47;

// Recovers a single PID's elementary stream from transport-stream packets.
// Stateless beyond the target PID: each packet strips TS framing and, on a
// payload-unit-start, the PES header, emitting the remaining ES via the sink.
class PesDepacketizer {
 public:
  explicit PesDepacketizer(uint16_t video_pid) noexcept : pid_(video_pid) {}

  // Process one 188-byte TS packet. Non-matching PIDs and packets without a
  // payload are ignored. `pkt` must point to kTsPacket bytes.
  void feed(const uint8_t* pkt, const EsSink& sink) const;

 private:
  uint16_t pid_;
};

}  // namespace tv::ts

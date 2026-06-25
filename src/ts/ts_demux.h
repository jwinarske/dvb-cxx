// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// tv::ts::Demux — pulls the video elementary stream out of a locked DVB
// adapter.
//
//   scan_program()  uses the kernel's PSI section filters on demux0 to read the
//                   PAT and the first program's PMT, yielding the video PID and
//                   its codec (MPEG-2 for ATSC 1.0, H.264/HEVC otherwise).
//   Demux::run()    sets a TS tap on the video PID (DMX_OUT_TS_TAP -> dvr0),
//                   reads the tapped transport stream, depacketizes PES, and
//                   hands the elementary-stream bytes to a callback (which
//                   feeds DecoderSource::submit_bitstream).
#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

#include "ts/ts_parse.h"  // VideoCodec, ProgramInfo, EsSink, parsers

namespace tv::ts {

// Scan the PAT and the first program's PMT on /dev/dvb/adapter<adapter>/demux0.
// Requires the frontend to be locked. Returns the first program carrying a
// video stream, or nullopt on timeout / no video.
std::optional<ProgramInfo> scan_program(int adapter, int timeout_ms = 5000);

class Demux {
 public:
  // Tap `video_pid` on /dev/dvb/adapter<adapter>/demux0 -> dvr0. Returns
  // nullopt if the devices can't be opened or the filter can't be set.
  static std::optional<Demux> open(int adapter, uint16_t video_pid);

  Demux(Demux&& o) noexcept
      : demux_fd_(std::exchange(o.demux_fd_, -1)),
        dvr_fd_(std::exchange(o.dvr_fd_, -1)),
        video_pid_(o.video_pid_) {}
  Demux& operator=(Demux&& o) noexcept;
  Demux(const Demux&) = delete;
  Demux& operator=(const Demux&) = delete;
  ~Demux();

  // Read the tapped TS and emit elementary-stream bytes via `sink` until `quit`
  // is set or a fatal read error occurs.
  void run(const std::atomic<bool>& quit, const EsSink& sink);

 private:
  Demux(int demux_fd, int dvr_fd, uint16_t video_pid) noexcept
      : demux_fd_(demux_fd), dvr_fd_(dvr_fd), video_pid_(video_pid) {}

  int demux_fd_ = -1;  // holds the PES/TS filter
  int dvr_fd_ = -1;    // reads the tapped TS
  uint16_t video_pid_ = 0;
};

}  // namespace tv::ts

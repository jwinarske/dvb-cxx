// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "ts/ts_demux.h"

#include <fcntl.h>
#include <linux/dvb/dmx.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace tv::ts {
namespace {

// Read one assembled PSI section (table_id `tid`) on adapter's demux0 for
// `pid`. Returns section length, or 0 on timeout/error. The kernel reassembles
// and CRC-checks the section; `buf` receives table_id .. CRC.
size_t read_section(int adapter,
                    uint16_t pid,
                    uint8_t tid,
                    uint8_t* buf,
                    size_t buflen,
                    int timeout_ms) {
  char path[64];
  std::snprintf(path, sizeof path, "/dev/dvb/adapter%d/demux0", adapter);
  const int fd = ::open(path, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    std::fprintf(stderr, "ts: open(%s): %s\n", path, std::strerror(errno));
    return 0;
  }

  dmx_sct_filter_params f{};
  f.pid = pid;
  f.filter.filter[0] = tid;
  f.filter.mask[0] = 0xff;
  f.timeout = 0;
  f.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;
  if (::ioctl(fd, DMX_SET_FILTER, &f) != 0) {
    std::fprintf(stderr, "ts: DMX_SET_FILTER(pid=%u tid=0x%02x): %s\n", pid,
                 tid, std::strerror(errno));
    ::close(fd);
    return 0;
  }

  pollfd p{fd, POLLIN, 0};
  size_t got = 0;
  if (::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN) != 0) {
    const ssize_t n = ::read(fd, buf, buflen);
    if (n > 0)
      got = static_cast<size_t>(n);
  }
  ::close(fd);
  return got;
}

}  // namespace

std::optional<ProgramInfo> scan_program(int adapter, int timeout_ms) {
  std::array<uint8_t, 4096> buf{};

  const size_t pat_len =
      read_section(adapter, 0x0000, 0x00, buf.data(), buf.size(), timeout_ms);
  if (pat_len == 0) {
    std::fprintf(stderr, "ts: no PAT (frontend not locked?)\n");
    return std::nullopt;
  }
  std::vector<ProgramInfo> programs;
  parse_pat(buf.data(), pat_len, programs);
  if (programs.empty()) {
    std::fprintf(stderr, "ts: PAT has no programs\n");
    return std::nullopt;
  }

  // Walk programs until one yields a PMT with a video stream.
  for (ProgramInfo pi : programs) {
    const size_t pmt_len = read_section(adapter, pi.pmt_pid, 0x02, buf.data(),
                                        buf.size(), timeout_ms);
    if (pmt_len == 0)
      continue;
    parse_pmt(buf.data(), pmt_len, pi);
    if (pi.video_pid != 0) {
      const char* cn = pi.video_codec == VideoCodec::Mpeg2  ? "MPEG-2"
                       : pi.video_codec == VideoCodec::H264 ? "H.264"
                       : pi.video_codec == VideoCodec::Hevc ? "HEVC"
                                                            : "?";
      std::fprintf(
          stderr,
          "ts: program %u (PMT pid %u): video pid %u (%s), audio pid %u\n",
          pi.program_number, pi.pmt_pid, pi.video_pid, cn, pi.audio_pid);
      return pi;
    }
  }
  std::fprintf(stderr, "ts: no program with a video stream\n");
  return std::nullopt;
}

std::optional<Demux> Demux::open(int adapter, uint16_t video_pid) {
  char dpath[64];
  char vpath[64];
  std::snprintf(dpath, sizeof dpath, "/dev/dvb/adapter%d/demux0", adapter);
  std::snprintf(vpath, sizeof vpath, "/dev/dvb/adapter%d/dvr0", adapter);

  const int demux_fd = ::open(dpath, O_RDWR);
  if (demux_fd < 0) {
    std::fprintf(stderr, "ts: open(%s): %s\n", dpath, std::strerror(errno));
    return std::nullopt;
  }
  // Route the video PID's packets to the DVR tap.
  dmx_pes_filter_params pf{};
  pf.pid = video_pid;
  pf.input = DMX_IN_FRONTEND;
  pf.output = DMX_OUT_TS_TAP;
  pf.pes_type = DMX_PES_OTHER;
  pf.flags = DMX_IMMEDIATE_START;
  if (::ioctl(demux_fd, DMX_SET_PES_FILTER, &pf) != 0) {
    std::fprintf(stderr, "ts: DMX_SET_PES_FILTER(pid=%u): %s\n", video_pid,
                 std::strerror(errno));
    ::close(demux_fd);
    return std::nullopt;
  }

  const int dvr_fd = ::open(vpath, O_RDONLY | O_NONBLOCK);
  if (dvr_fd < 0) {
    std::fprintf(stderr, "ts: open(%s): %s\n", vpath, std::strerror(errno));
    ::close(demux_fd);
    return std::nullopt;
  }
  return Demux(demux_fd, dvr_fd, video_pid);
}

Demux& Demux::operator=(Demux&& o) noexcept {
  if (this != &o) {
    if (dvr_fd_ >= 0)
      ::close(dvr_fd_);
    if (demux_fd_ >= 0)
      ::close(demux_fd_);
    demux_fd_ = std::exchange(o.demux_fd_, -1);
    dvr_fd_ = std::exchange(o.dvr_fd_, -1);
    video_pid_ = o.video_pid_;
  }
  return *this;
}

Demux::~Demux() {
  if (dvr_fd_ >= 0)
    ::close(dvr_fd_);
  if (demux_fd_ >= 0)
    ::close(demux_fd_);
}

void Demux::run(const std::atomic<bool>& quit, const EsSink& sink) {
  // Read the tapped TS in TS-packet-aligned chunks and strip PES framing to
  // recover the elementary stream. The tap delivers only `video_pid_`.
  const PesDepacketizer dep(video_pid_);
  std::vector<uint8_t> rb(kTsPacket * 348);  // ~64 KiB of whole packets
  size_t leftover = 0;  // bytes of a partial trailing packet carried over

  while (!quit) {
    pollfd p{dvr_fd_, POLLIN, 0};
    const int pr = ::poll(&p, 1, 200);
    if (pr <= 0)
      continue;
    // The tuner unplugging (or the dvr hanging up) raises POLLHUP/POLLERR with
    // POLLIN clear; poll then returns immediately, so a `continue` here would
    // busy-spin at 100% CPU. Treat hangup/error as end-of-stream.
    if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
      break;
    if ((p.revents & POLLIN) == 0)
      continue;

    const ssize_t n =
        ::read(dvr_fd_, rb.data() + leftover, rb.size() - leftover);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (n < 0 && errno == EOVERFLOW) {  // DVR ring overran; resync
        leftover = 0;
        continue;
      }
      break;  // EOF or fatal error
    }

    const size_t total = leftover + static_cast<size_t>(n);
    size_t off = 0;
    while (off + kTsPacket <= total) {
      const uint8_t* pkt = rb.data() + off;
      if (pkt[0] != kTsSync) {  // resync to the next sync byte
        ++off;
        continue;
      }
      dep.feed(pkt, sink);
      off += kTsPacket;
    }

    // Carry any partial trailing packet to the front for the next read.
    leftover = total - off;
    if (leftover > 0 && off > 0)
      std::memmove(rb.data(), rb.data() + off, leftover);
  }
}

}  // namespace tv::ts

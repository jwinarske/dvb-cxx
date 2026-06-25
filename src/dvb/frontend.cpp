// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "dvb/frontend.h"

#include <fcntl.h>
#include <linux/dvb/frontend.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace tv::dvb {
namespace {

// DRM/DVB ioctls retry on EINTR.
int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do {
    r = ::ioctl(fd, static_cast<int>(req), arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

}  // namespace

uint32_t atsc_channel_freq_hz(int ch) {
  // 6 MHz channels; the band gaps (FM between 6 and 7, etc.) make this
  // piecewise. Values are channel-center frequencies in MHz.
  int mhz = 0;
  if (ch >= 2 && ch <= 4)
    mhz = 57 + (ch - 2) * 6;  // 57, 63, 69
  else if (ch >= 5 && ch <= 6)
    mhz = 79 + (ch - 5) * 6;  // 79, 85
  else if (ch >= 7 && ch <= 13)
    mhz = 177 + (ch - 7) * 6;  // 177 .. 213
  else if (ch >= 14 && ch <= 69)
    mhz = 473 + (ch - 14) * 6;  // 473 .. 803
  else
    return 0;
  return static_cast<uint32_t>(mhz) * 1000000U;
}

std::optional<Frontend> Frontend::open(int adapter, int fe) {
  char path[64];
  std::snprintf(path, sizeof path, "/dev/dvb/adapter%d/frontend%d", adapter,
                fe);
  const int fd = ::open(path, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    std::fprintf(stderr, "dvb: open(%s): %s\n", path, std::strerror(errno));
    return std::nullopt;
  }
  return Frontend(fd, adapter);
}

Frontend& Frontend::operator=(Frontend&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = std::exchange(o.fd_, -1);
    adapter_ = o.adapter_;
  }
  return *this;
}

Frontend::~Frontend() {
  if (fd_ >= 0)
    ::close(fd_);
}

bool Frontend::tune(uint32_t freq_hz,
                    Modulation mod,
                    int timeout_ms,
                    bool verbose) const {
  // Clear any previous tune state first.
  dtv_property clr{};
  clr.cmd = DTV_CLEAR;
  dtv_properties clrseq{1, &clr};
  (void)xioctl(fd_, FE_SET_PROPERTY, &clrseq);

  fe_delivery_system delsys =
      mod == Modulation::Vsb8 ? SYS_ATSC : SYS_DVBC_ANNEX_B;
  fe_modulation modulation = mod == Modulation::Vsb8     ? VSB_8
                             : mod == Modulation::Qam256 ? QAM_256
                                                         : QAM_64;

  dtv_property p[4]{};
  p[0].cmd = DTV_DELIVERY_SYSTEM;
  p[0].u.data = delsys;
  p[1].cmd = DTV_FREQUENCY;
  p[1].u.data = freq_hz;
  p[2].cmd = DTV_MODULATION;
  p[2].u.data = modulation;
  p[3].cmd = DTV_TUNE;
  dtv_properties seq{4, p};
  if (xioctl(fd_, FE_SET_PROPERTY, &seq) != 0) {
    std::fprintf(stderr, "dvb: FE_SET_PROPERTY (tune): %s\n",
                 std::strerror(errno));
    return false;
  }

  if (verbose)
    std::fprintf(stderr, "dvb: tuning %.3f MHz %s ...\n", freq_hz / 1e6,
                 mod == Modulation::Vsb8 ? "8-VSB" : "QAM");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    fe_status_t st{};
    if (xioctl(fd_, FE_READ_STATUS, &st) == 0 && (st & FE_HAS_LOCK) != 0) {
      if (verbose) {
        const SignalStatus s = status();
        std::fprintf(stderr, "dvb: LOCK (signal=%u snr=%u)\n", s.signal, s.snr);
      }
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (verbose)
    std::fprintf(stderr, "dvb: no lock within %d ms (check antenna/cable)\n",
                 timeout_ms);
  return false;
}

bool Frontend::tune_atsc_channel(int channel,
                                 int timeout_ms,
                                 bool verbose) const {
  const uint32_t f = atsc_channel_freq_hz(channel);
  if (f == 0) {
    std::fprintf(stderr, "dvb: channel %d out of ATSC range (2..69)\n",
                 channel);
    return false;
  }
  return tune(f, Modulation::Vsb8, timeout_ms, verbose);
}

SignalStatus Frontend::status() const {
  SignalStatus s;
  fe_status_t st{};
  if (xioctl(fd_, FE_READ_STATUS, &st) == 0) {
    s.has_signal = (st & FE_HAS_SIGNAL) != 0;
    s.has_lock = (st & FE_HAS_LOCK) != 0;
  }
  uint16_t v = 0;
  if (xioctl(fd_, FE_READ_SIGNAL_STRENGTH, &v) == 0)
    s.signal = v;
  v = 0;
  if (xioctl(fd_, FE_READ_SNR, &v) == 0)
    s.snr = v;
  return s;
}

}  // namespace tv::dvb

// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// tv::dvb::Frontend — RAII wrapper over /dev/dvb/adapterN/frontendM that tunes
// a channel via the DVBv5 property API and waits for the demodulator to lock.
//
// For the WinTV-dualHD (ATSC) the delivery system is SYS_ATSC with 8-VSB
// modulation for over-the-air, or SYS_DVBC_ANNEX_B with QAM for North-American
// "ClearQAM" cable. Once locked, the transport stream flows out of the
// adapter's demux/dvr (see tv::ts::Demux).
#include <cstdint>
#include <optional>
#include <utility>

namespace tv::dvb {

// 8-VSB is ATSC over-the-air; QAM-256/64 is North-American ClearQAM cable
// (SYS_DVBC_ANNEX_B).
enum class Modulation { Vsb8, Qam256, Qam64 };

// Center frequency in Hz of a North-American ATSC physical RF channel (2..69),
// or 0 if out of range. 6 MHz channels: low-VHF 2-6, high-VHF 7-13, UHF 14-69.
uint32_t atsc_channel_freq_hz(int channel);

struct SignalStatus {
  bool has_signal = false;  // FE_HAS_SIGNAL
  bool has_lock = false;    // FE_HAS_LOCK
  uint16_t signal = 0;  // relative signal strength (FE_READ_SIGNAL_STRENGTH)
  uint16_t snr = 0;     // relative SNR (FE_READ_SNR)
};

class Frontend {
 public:
  // Open /dev/dvb/adapter<adapter>/frontend<fe>. Returns nullopt if it can't be
  // opened (no such adapter, or busy).
  static std::optional<Frontend> open(int adapter, int fe = 0);

  Frontend(Frontend&& o) noexcept
      : fd_(std::exchange(o.fd_, -1)), adapter_(o.adapter_) {}
  Frontend& operator=(Frontend&& o) noexcept;
  Frontend(const Frontend&) = delete;
  Frontend& operator=(const Frontend&) = delete;
  ~Frontend();

  // Tune to `freq_hz` with `mod` and block until FE_HAS_LOCK or `timeout_ms`
  // elapses. Returns true on lock. const: the fd is a handle; tuning changes
  // hardware state, not this wrapper (mirrors drm::Device's const ioctl
  // methods).
  [[nodiscard]] bool tune(uint32_t freq_hz,
                          Modulation mod,
                          int timeout_ms = 3000) const;

  // Convenience: tune a North-American ATSC OTA physical channel (8-VSB).
  [[nodiscard]] bool tune_atsc_channel(int channel,
                                       int timeout_ms = 3000) const;

  [[nodiscard]] SignalStatus status() const;
  [[nodiscard]] int adapter() const noexcept { return adapter_; }

 private:
  Frontend(int fd, int adapter) noexcept : fd_(fd), adapter_(adapter) {}

  int fd_ = -1;
  int adapter_ = 0;
};

}  // namespace tv::dvb

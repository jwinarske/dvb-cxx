// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// dvb-kms — DRM/KMS player for a DVB/ATSC TV tuner.
//
// Two input modes feed the same decode→present pipeline (decode auto-selects
// VAAPI ▸ V4L2 ▸ software; the frame is scanned out on a KMS hardware plane):
//
//   live tuner:  dvb-kms --adapter 0 --channel 7      (ATSC 8-VSB OTA)
//                dvb-kms --adapter 0 --freq 177000000  (explicit Hz)
//   file (test): dvb-kms stream.es                     (a demuxed video ES)
//
// The DRM backend needs DRM master (a bare VT / no compositor on the card).
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

#include <drm_mode.h>

#include "decoder_source.h"
#include "dvb/frontend.h"
#include "kms_presenter.h"
#include "ts/ts_demux.h"

namespace {

std::atomic<bool> g_quit{false};
void on_signal(int) {
  g_quit = true;
}

tv::Codec parse_codec(const char* s) {
  if (std::strcmp(s, "h264") == 0)
    return tv::Codec::H264;
  if (std::strcmp(s, "hevc") == 0 || std::strcmp(s, "h265") == 0)
    return tv::Codec::Hevc;
  return tv::Codec::Mpeg2;
}

tv::Codec codec_from_ts(tv::ts::VideoCodec vc) {
  switch (vc) {
    case tv::ts::VideoCodec::H264:
      return tv::Codec::H264;
    case tv::ts::VideoCodec::Hevc:
      return tv::Codec::Hevc;
    default:
      return tv::Codec::Mpeg2;
  }
}

const char* ts_codec_name(tv::ts::VideoCodec vc) {
  switch (vc) {
    case tv::ts::VideoCodec::Mpeg2:
      return "MPEG-2";
    case tv::ts::VideoCodec::H264:
      return "H.264";
    case tv::ts::VideoCodec::Hevc:
      return "HEVC";
    default:
      return "?";
  }
}

// Parse a comma-separated channel list (e.g. "7,9,33") into `out`. Invalid
// entries are skipped.
void parse_channel_list(const char* s, std::vector<int>& out) {
  while (s != nullptr && *s != '\0') {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s)
      break;
    if (v > 0)
      out.push_back(static_cast<int>(v));
    s = (*end == ',') ? end + 1 : end;
    while (*s == ',')
      ++s;
  }
}

// Which modulation families a scan sweeps. Vsb = ATSC 8-VSB over-the-air;
// Qam = ClearQAM cable (QAM-256/64); All = both (per frontend capability).
enum class ScanMod { Vsb, Qam, All };

ScanMod parse_scanmod(const char* s) {
  if (std::strcmp(s, "qam") == 0)
    return ScanMod::Qam;
  if (std::strcmp(s, "all") == 0)
    return ScanMod::All;
  return ScanMod::Vsb;
}

struct Args {
  const char* card = "/dev/dri/card0";
  int adapter = 0;
  int channel = 0;       // ATSC physical channel (>0 selects live mode)
  uint32_t freq_hz = 0;  // explicit frequency (selects live mode)
  tv::Codec codec =
      tv::Codec::Mpeg2;  // file mode only; live mode reads the PMT
  const char* file = nullptr;
  bool scan = false;                // sweep channels and report locked programs
  ScanMod scan_mod = ScanMod::Vsb;  // scan: which modulation family/families
  std::vector<int> channels;        // scan: narrow set; empty = full plan
  int scan_timeout_ms = 1500;       // scan: per-channel lock timeout
};

// Build the presenter, attach a decode source for `codec`, and run the present
// loop while `feed_fn` pushes the elementary stream on a background thread.
int play(const Args& a,
         tv::Codec codec,
         const std::function<void(tv::DecoderSource*)>& feed_fn) {
  auto presenter = tv::KmsPresenter::create(a.card);
  if (!presenter)
    return 1;
  auto src =
      tv::create_decoder_source(presenter->device(), codec, presenter->width(),
                                presenter->height(), DRM_MODE_ROTATE_0);
  if (!src) {
    std::fprintf(stderr, "no video decoder available\n");
    return 1;
  }
  tv::DecoderSource* feed = presenter->set_source(std::move(src), 0);
  if (feed == nullptr)
    return 1;

  // Guard the feed body: an exception escaping a std::thread calls
  // std::terminate, bypassing the orderly g_quit/join shutdown.
  std::thread feeder([&] {
    try {
      feed_fn(feed);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "feed: %s\n", e.what());
      g_quit = true;
    }
  });
  const int rc = presenter->run(g_quit);
  g_quit = true;
  if (feeder.joinable())
    feeder.join();
  return rc;
}

int run_live(const Args& a) {
  auto fe = tv::dvb::Frontend::open(a.adapter);
  if (!fe)
    return 1;
  const bool locked = a.freq_hz != 0
                          ? fe->tune(a.freq_hz, tv::dvb::Modulation::Vsb8)
                          : fe->tune_atsc_channel(a.channel);
  if (!locked)
    return 1;

  const auto prog = tv::ts::scan_program(a.adapter);
  if (!prog)
    return 1;

  return play(a, codec_from_ts(prog->video_codec),
              [&](tv::DecoderSource* feed) {
                auto dx = tv::ts::Demux::open(a.adapter, prog->video_pid);
                if (!dx) {
                  g_quit = true;
                  return;
                }
                dx->run(g_quit, [&](const uint8_t* d, size_t n, uint64_t pts) {
                  feed->submit_bitstream(d, n, pts);
                });
                std::fprintf(stderr, "demux: stream ended\n");
              });
}

int run_file(const Args& a) {
  return play(a, a.codec, [&](tv::DecoderSource* feed) {
    std::ifstream in(a.file, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "cannot open %s\n", a.file);
      g_quit = true;
      return;
    }
    std::vector<uint8_t> buf(64 * 1024);
    while (!g_quit && in) {
      in.read(reinterpret_cast<char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
      const auto n = static_cast<size_t>(in.gcount());
      if (n == 0)
        break;
      feed->submit_bitstream(buf.data(), n, 0);
    }
    std::fprintf(stderr, "feed: end of file\n");
  });
}

// Sweep one modulation family's channels: tune each, and report the locked ones
// with their first program. For QAM, tries QAM-256 then QAM-64 per channel.
// Returns the number of channels that locked.
int scan_pass(const tv::dvb::Frontend& fe,
              int adapter,
              const std::vector<int>& channels,
              uint32_t (*freq_fn)(int),
              bool qam,
              int timeout_ms) {
  int locked = 0;
  for (const int ch : channels) {
    if (g_quit)
      break;
    const uint32_t f = freq_fn(ch);
    if (f == 0)
      continue;
    const double mhz = f / 1e6;
    const char* used = "8-VSB";
    bool got = false;
    if (qam) {
      if (fe.tune(f, tv::dvb::Modulation::Qam256, timeout_ms, false)) {
        got = true;
        used = "QAM-256";
      } else if (fe.tune(f, tv::dvb::Modulation::Qam64, timeout_ms, false)) {
        got = true;
        used = "QAM-64";
      }
    } else {
      got = fe.tune(f, tv::dvb::Modulation::Vsb8, timeout_ms, false);
    }
    if (!got) {
      std::printf("  ch %3d (%7.3f MHz): no lock\n", ch, mhz);
      continue;
    }
    ++locked;
    const tv::dvb::SignalStatus s = fe.status();
    const auto prog = tv::ts::scan_program(adapter, 2500);
    if (prog)
      std::printf(
          "  ch %3d (%7.3f MHz): LOCK %-7s snr=%u  program %u: video pid %u "
          "(%s)\n",
          ch, mhz, used, s.snr, prog->program_number, prog->video_pid,
          ts_codec_name(prog->video_codec));
    else
      std::printf("  ch %3d (%7.3f MHz): LOCK %-7s snr=%u  (no program info)\n",
                  ch, mhz, used, s.snr);
    std::fflush(stdout);
  }
  return locked;
}

// Build the channel list for a pass: the explicit --channels set, or the full
// plan via `freq_fn` over [lo, hi].
std::vector<int> scan_channels(const Args& a,
                               uint32_t (*freq_fn)(int),
                               int lo,
                               int hi) {
  if (!a.channels.empty())
    return a.channels;
  std::vector<int> out;
  for (int c = lo; c <= hi; ++c)
    if (freq_fn(c) != 0)
      out.push_back(c);
  return out;
}

// Tuner-only channel scan (no DRM, so it runs without a display / DRM master).
// Sweeps 8-VSB and/or QAM per --modulation, skipping any the frontend can't do.
int run_scan(const Args& a) {
  auto fe = tv::dvb::Frontend::open(a.adapter);
  if (!fe)
    return 1;

  const bool want_vsb =
      a.scan_mod == ScanMod::Vsb || a.scan_mod == ScanMod::All;
  const bool want_qam =
      a.scan_mod == ScanMod::Qam || a.scan_mod == ScanMod::All;

  int total_locked = 0;
  size_t total_channels = 0;

  if (want_vsb) {
    if (!fe->supports(tv::dvb::Modulation::Vsb8)) {
      std::fprintf(stderr, "scan: frontend has no 8-VSB; skipping\n");
    } else {
      const auto ch = scan_channels(a, tv::dvb::atsc_channel_freq_hz, 2, 69);
      std::printf("== 8-VSB (ATSC OTA): %zu channels, %d ms/ch ==\n", ch.size(),
                  a.scan_timeout_ms);
      total_locked +=
          scan_pass(*fe, a.adapter, ch, tv::dvb::atsc_channel_freq_hz,
                    /*qam=*/false, a.scan_timeout_ms);
      total_channels += ch.size();
    }
  }
  if (want_qam) {
    if (!fe->supports(tv::dvb::Modulation::Qam256)) {
      std::fprintf(stderr, "scan: frontend has no QAM; skipping\n");
    } else {
      const auto ch = scan_channels(a, tv::dvb::cable_channel_freq_hz, 2, 158);
      std::printf(
          "== QAM (ClearQAM cable): %zu channels x{256,64}, %d ms/ch ==\n",
          ch.size(), a.scan_timeout_ms);
      total_locked +=
          scan_pass(*fe, a.adapter, ch, tv::dvb::cable_channel_freq_hz,
                    /*qam=*/true, a.scan_timeout_ms);
      total_channels += ch.size();
    }
  }
  std::printf("scan complete: %d of %zu channels locked\n", total_locked,
              total_channels);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--card") == 0 && i + 1 < argc)
      a.card = argv[++i];
    else if (std::strcmp(argv[i], "--adapter") == 0 && i + 1 < argc)
      a.adapter = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    else if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc)
      a.channel = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    else if (std::strcmp(argv[i], "--freq") == 0 && i + 1 < argc)
      a.freq_hz = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc)
      a.codec = parse_codec(argv[++i]);
    else if (std::strcmp(argv[i], "--scan") == 0)
      a.scan = true;
    else if (std::strcmp(argv[i], "--modulation") == 0 && i + 1 < argc)
      a.scan_mod = parse_scanmod(argv[++i]);
    else if (std::strcmp(argv[i], "--channels") == 0 && i + 1 < argc)
      parse_channel_list(argv[++i], a.channels);
    else if (std::strcmp(argv[i], "--scan-timeout") == 0 && i + 1 < argc)
      a.scan_timeout_ms = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    else if (argv[i][0] == '-') {
      // An unknown flag, or a known flag missing its value (which would
      // otherwise be silently taken as the input filename).
      std::fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
      return 2;
    } else
      a.file = argv[i];
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (a.scan)
    return run_scan(a);
  const bool live = a.channel > 0 || a.freq_hz != 0;
  if (live)
    return run_live(a);
  if (a.file != nullptr)
    return run_file(a);

  std::fprintf(stderr,
               "usage:\n"
               "  dvb-kms [--card DEV] --adapter N --channel C   (ATSC OTA)\n"
               "  dvb-kms [--card DEV] --adapter N --freq HZ      (explicit)\n"
               "  dvb-kms [--card DEV] [--codec mpeg2|h264|hevc] FILE.es\n"
               "  dvb-kms --adapter N --scan [--modulation vsb|qam|all]\n"
               "          [--channels 7,9,33] [--scan-timeout MS]\n");
  return 2;
}

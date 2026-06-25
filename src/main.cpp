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

struct Args {
  const char* card = "/dev/dri/card0";
  int adapter = 0;
  int channel = 0;       // ATSC physical channel (>0 selects live mode)
  uint32_t freq_hz = 0;  // explicit frequency (selects live mode)
  tv::Codec codec =
      tv::Codec::Mpeg2;  // file mode only; live mode reads the PMT
  const char* file = nullptr;
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

  std::thread feeder([&] { feed_fn(feed); });
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
    else
      a.file = argv[i];
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const bool live = a.channel > 0 || a.freq_hz != 0;
  if (live)
    return run_live(a);
  if (a.file != nullptr)
    return run_file(a);

  std::fprintf(stderr,
               "usage:\n"
               "  dvb-kms [--card DEV] --adapter N --channel C   (ATSC OTA)\n"
               "  dvb-kms [--card DEV] --adapter N --freq HZ      (explicit)\n"
               "  dvb-kms [--card DEV] [--codec mpeg2|h264|hevc] FILE.es\n");
  return 2;
}

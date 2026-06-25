// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "decoder_source.h"

#include <strings.h>

#include <cstdio>
#include <cstdlib>

#include "software_decoder_source.h"
#ifdef TVTUNER_HAVE_VAAPI
#include "vaapi_decoder_source.h"
#endif
#ifdef TVTUNER_HAVE_V4L2
#include "v4l2_decoder_source_adapter.h"
#endif

namespace tv {
namespace {

DecoderBackend parse_pref() {
  const char* d = std::getenv("TVTUNER_DECODER");
  if (d == nullptr || *d == '\0')
    return DecoderBackend::Auto;
  if (strcasecmp(d, "auto") == 0)
    return DecoderBackend::Auto;
  if (strcasecmp(d, "vaapi") == 0)
    return DecoderBackend::Vaapi;
  if (strcasecmp(d, "v4l2") == 0)
    return DecoderBackend::V4l2;
  if (strcasecmp(d, "software") == 0 || strcasecmp(d, "sw") == 0)
    return DecoderBackend::Software;
  std::fprintf(stderr, "TVTUNER_DECODER=%s unrecognized; using auto\n", d);
  return DecoderBackend::Auto;
}

const char* codec_name(Codec c) {
  switch (c) {
    case Codec::H264:
      return "H.264";
    case Codec::Hevc:
      return "HEVC";
    case Codec::Mpeg2:
    default:
      return "MPEG-2";
  }
}

void software_warning(Codec codec) {
  std::fprintf(
      stderr,
      "\n"
      "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
      "!!!  SOFTWARE DECODE WARNING\n"
      "!!!  No hardware video decoder is in use. %s is being\n"
      "!!!  decoded on the CPU and converted to NV12 every frame,\n"
      "!!!  which is slow and may not keep up at higher resolutions.\n"
      "!!!  Prefer a VAAPI or V4L2 decoder where one is available\n"
      "!!!  (note: modern AMD/Intel GPUs no longer decode MPEG-2).\n"
      "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n",
      codec_name(codec));
}

std::unique_ptr<DecoderSource> make_software(drm::Device& dev,
                                             Codec codec,
                                             uint32_t coded_w,
                                             uint32_t coded_h,
                                             uint64_t rot) {
  if (auto s =
          SoftwareDecoderSource::create(dev, codec, coded_w, coded_h, rot)) {
    software_warning(codec);
    std::fprintf(stderr, "decoder: software (CPU %s -> NV12 dumb buffer)\n",
                 codec_name(codec));
    return s;
  }
  std::fprintf(stderr, "software decoder open failed\n");
  return nullptr;
}

}  // namespace

std::unique_ptr<DecoderSource> create_decoder_source(drm::Device& dev,
                                                     Codec codec,
                                                     uint32_t coded_w,
                                                     uint32_t coded_h,
                                                     uint64_t rot) {
  const DecoderBackend pref = parse_pref();

  // VAAPI — the zero-copy HW-plane path. Engages only when the driver exposes a
  // VAProfile for `codec` (e.g. older Intel/AMD for MPEG-2; any modern GPU for
  // H.264/HEVC); otherwise its open fails and Auto moves on.
#ifdef TVTUNER_HAVE_VAAPI
  if (pref == DecoderBackend::Auto || pref == DecoderBackend::Vaapi) {
    if (auto s = VaapiDecoderSource::create(dev, codec, coded_w, coded_h)) {
      std::fprintf(stderr, "decoder: VAAPI (zero-copy HW plane)\n");
      return s;
    }
    if (pref == DecoderBackend::Vaapi) {
      std::fprintf(stderr, "TVTUNER_DECODER=vaapi but VAAPI open failed\n");
      return nullptr;
    }
    std::fprintf(stderr, "VAAPI unavailable for %s; trying next backend\n",
                 codec_name(codec));
  }
#else
  if (pref == DecoderBackend::Vaapi) {
    std::fprintf(stderr, "built without VAAPI backend\n");
    return nullptr;
  }
#endif

  // V4L2 — a stateful SoC decoder (RPi4 MPEG-2 block, Rockchip, …).
#ifdef TVTUNER_HAVE_V4L2
  if (pref == DecoderBackend::Auto || pref == DecoderBackend::V4l2) {
    if (auto s =
            V4l2DecoderSourceAdapter::create(dev, codec, coded_w, coded_h)) {
      std::fprintf(stderr, "decoder: V4L2 (SoC HW decoder)\n");
      return s;
    }
    if (pref == DecoderBackend::V4l2) {
      std::fprintf(stderr, "TVTUNER_DECODER=v4l2 but V4L2 open failed\n");
      return nullptr;
    }
    std::fprintf(stderr, "V4L2 unavailable for %s; trying next backend\n",
                 codec_name(codec));
  }
#else
  if (pref == DecoderBackend::V4l2) {
    std::fprintf(stderr, "built without V4L2 backend\n");
    return nullptr;
  }
#endif

  // Software — the always-available CPU fallback (pinned, or the end of Auto).
  return make_software(dev, codec, coded_w, coded_h, rot);
}

}  // namespace tv

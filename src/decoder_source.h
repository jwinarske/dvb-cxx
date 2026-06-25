// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// DecoderSource — the tvtuner-side interface a video-decode backend presents to
// the KMS scene. It extends drm-cxx's LayerBufferSource
// (acquire/release/format/ binding_model, consumed by the scene each commit)
// with the feed API the scene doesn't cover: a compressed elementary stream in,
// optional screenshot out.
//
// The compressed input is the video elementary stream demuxed out of the DVB
// transport stream (tv::ts::Demux). For ATSC 1.0 that is MPEG-2 video (H.262);
// the codec is selected at create() time so the same plumbing serves ATSC 3.0
// (HEVC) or cable QAM (MPEG-2 / H.264).
//
// Backends, in fallback order (see create_decoder_source):
//   * VAAPI    — libavcodec hardware decode to a vendor-tiled NV12 DMA-BUF,
//                scanned out zero-copy on a HW plane. Engages only when the
//                driver exposes a VAProfile for the codec (e.g. older Intel/AMD
//                for MPEG-2; any modern GPU for H.264/HEVC).
//   * V4L2     — a stateful SoC video decoder (NV12 LINEAR DMA-BUF). The
//                embedded path (RPi4 MPEG-2 block, Rockchip, …).
//   * software — libavcodec software decode into a CPU dumb buffer. Always
//                available, but slow (per-frame decode + convert + copy); the
//                last resort, announced with a loud warning when it engages.
#include <drm-cxx/scene/buffer_source.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace drm {
class Device;
}  // namespace drm

namespace tv {

// The compressed codec the demuxed elementary stream carries. Maps to both a
// libavcodec AV_CODEC_ID (VAAPI / software backends) and a V4L2 OUTPUT fourcc
// (V4L2 backend). MPEG-2 is the ATSC 1.0 over-the-air codec.
enum class Codec { Mpeg2, H264, Hevc };

class DecoderSource : public drm::scene::LayerBufferSource {
 public:
  // Feed a chunk of the video elementary stream (any chunking), called from the
  // demux thread. No default pts_ns: a default argument on a virtual is a lint
  // trap (an override could change it); callers pass 0 when pts is irrelevant.
  virtual void submit_bitstream(const uint8_t* data,
                                size_t len,
                                uint64_t pts_ns) = 0;

  // Request that the next decoded frame be written to `<dir>/capture-WxH.nv12`
  // (tightly-packed NV12). Backends with no CPU access to decoded frames may
  // ignore the request; the default is a no-op.
  virtual void request_capture(const char* /*dir*/) {}

  // The display rotation (a DRM_MODE_ROTATE_* bit) the source has already baked
  // into its output buffer, so format() is in final orientation and the caller
  // must not also rotate the plane. 0 means the source did not rotate (the
  // caller drives plane rotation). Only the software backend overrides this.
  [[nodiscard]] virtual uint64_t applied_rotation() const noexcept { return 0; }
};

// Which backend create_decoder_source should use. Auto runs the full fallback
// chain; the others pin a single backend (a pinned backend that fails to open
// is an error, not a fall-through).
enum class DecoderBackend { Auto, Vaapi, V4l2, Software };

// Build the video-decode source. The backend is chosen by TVTUNER_DECODER
// (vaapi|v4l2|software|auto, default auto); Auto tries VAAPI, then V4L2, then
// software, and uses the first that opens. coded_w/coded_h seed the decoder's
// buffer pool until the first frame locks in the real size. `rot` is a
// DRM_MODE_ROTATE_* bit the software backend bakes into its output (hardware
// backends ignore it and leave plane rotation to the caller); pass
// DRM_MODE_ROTATE_0 for none. Returns nullptr if no backend could be opened.
std::unique_ptr<DecoderSource> create_decoder_source(drm::Device& dev,
                                                     Codec codec,
                                                     uint32_t coded_w,
                                                     uint32_t coded_h,
                                                     uint64_t rot);

}  // namespace tv

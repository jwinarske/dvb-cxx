// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// KmsPresenter — owns the drm-cxx DRM/KMS scene that scans a DecoderSource's
// NV12 buffer onto a hardware plane. Open a card, hand it a decode source, then
// run() the page-flip loop. The source is fed (submit_bitstream) from another
// thread; the scene pulls the latest frame each commit.
//
// Non-movable: it owns a drm::Device by value and a PageFlip that references
// it, so it lives behind a unique_ptr (create()) and never relocates.
#include <atomic>
#include <cstdint>
#include <memory>

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include "decoder_source.h"
#include "kms_output.h"

namespace tv {

class KmsPresenter {
 public:
  // Open `card_path` (e.g. /dev/dri/card0), enable atomic + universal planes,
  // select an output, and create the scene. Requires DRM master (no compositor
  // on the card). Returns nullptr on failure.
  static std::unique_ptr<KmsPresenter> create(const char* card_path);

  KmsPresenter(const KmsPresenter&) = delete;
  KmsPresenter& operator=(const KmsPresenter&) = delete;

  [[nodiscard]] drm::Device& device() noexcept { return dev_; }
  [[nodiscard]] uint32_t width() const noexcept { return out_.mode.hdisplay; }
  [[nodiscard]] uint32_t height() const noexcept { return out_.mode.vdisplay; }

  // Add `src` as the video layer, aspect-fit and centered on the display via
  // the plane scaler. `plane_rotation` is a DRM_MODE_ROTATE_* bit for the HW
  // plane (pass 0 if the source already baked rotation in — see
  // applied_rotation()). Returns a borrowed pointer for feeding; the scene owns
  // the source. Returns nullptr on failure. Call once.
  DecoderSource* set_source(std::unique_ptr<DecoderSource> src,
                            uint64_t plane_rotation);

  // Run the page-flip-paced present loop until `quit` is set. Returns 0 on a
  // clean exit, non-zero on a commit error.
  int run(const std::atomic<bool>& quit);

 private:
  // scene_ is created *after* construction, against the member dev_: drm-cxx's
  // LayerScene caches &device and dereferences it on every commit, so the
  // Device must already live at its final (post-move) address before the scene
  // is built. See create().
  KmsPresenter(drm::Device dev, Output out) noexcept;

  drm::Device dev_;
  Output out_;
  std::unique_ptr<drm::scene::LayerScene> scene_;
  DecoderSource* source_ = nullptr;  // borrowed; owned by scene_
};

}  // namespace tv

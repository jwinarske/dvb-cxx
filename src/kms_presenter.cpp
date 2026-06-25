// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "kms_presenter.h"

#include <drm_mode.h>
#include <poll.h>

#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tv {

KmsPresenter::KmsPresenter(
    drm::Device dev,
    Output out,
    std::unique_ptr<drm::scene::LayerScene> scene) noexcept
    : dev_(std::move(dev)), out_(out), scene_(std::move(scene)) {}

std::unique_ptr<KmsPresenter> KmsPresenter::create(const char* card_path) {
  auto dev_r = drm::Device::open(card_path);
  if (!dev_r) {
    std::fprintf(stderr, "kms: Device::open(%s): %s\n", card_path,
                 dev_r.error().message().c_str());
    return nullptr;
  }
  drm::Device dev = std::move(*dev_r);
  if (auto r = dev.enable_universal_planes(); !r) {
    std::fprintf(stderr, "kms: enable_universal_planes: %s\n",
                 r.error().message().c_str());
    return nullptr;
  }
  if (auto r = dev.enable_atomic(); !r) {
    std::fprintf(stderr, "kms: enable_atomic: %s\n",
                 r.error().message().c_str());
    return nullptr;
  }

  auto out = select_output(dev.fd());
  if (!out)
    return nullptr;

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = out->crtc_id;
  cfg.connector_id = out->connector_id;
  cfg.mode = out->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    std::fprintf(stderr, "kms: LayerScene::create: %s\n",
                 scene_r.error().message().c_str());
    return nullptr;
  }

  return std::unique_ptr<KmsPresenter>(
      new KmsPresenter(std::move(dev), *out, std::move(*scene_r)));
}

DecoderSource* KmsPresenter::set_source(std::unique_ptr<DecoderSource> src,
                                        uint64_t plane_rotation) {
  if (!src)
    return nullptr;

  const drm::scene::SourceFormat sf = src->format();
  const uint32_t W = width();
  const uint32_t H = height();

  // 90/270 plane rotation presents the content's w/h swapped, so fit the
  // swapped extents into the display.
  const bool swap_wh = plane_rotation == DRM_MODE_ROTATE_90 ||
                       plane_rotation == DRM_MODE_ROTATE_270;
  const uint32_t ivw = swap_wh ? sf.height : sf.width;
  const uint32_t ivh = swap_wh ? sf.width : sf.height;
  if (ivw == 0 || ivh == 0 || W == 0 || H == 0) {
    std::fprintf(stderr,
                 "kms: zero source/display dimensions (%ux%u into %ux%u)\n",
                 ivw, ivh, W, H);
    return nullptr;
  }
  const double fit =
      std::min(static_cast<double>(W) / ivw, static_cast<double>(H) / ivh);
  const int fvw = static_cast<int>(std::lround(ivw * fit));
  const int fvh = static_cast<int>(std::lround(ivh * fit));
  const int fvx = (static_cast<int>(W) - fvw) / 2;
  const int fvy = (static_cast<int>(H) - fvh) / 2;

  drm::scene::LayerDesc desc;
  desc.source = std::move(src);
  desc.display.src_rect = drm::scene::Rect{0, 0, sf.width, sf.height};
  desc.display.dst_rect = drm::scene::Rect{fvx, fvy, static_cast<uint32_t>(fvw),
                                           static_cast<uint32_t>(fvh)};
  if (plane_rotation != 0)
    desc.display.rotation = plane_rotation;
  desc.content_type = drm::planes::ContentType::Video;

  auto lh = scene_->add_layer(std::move(desc));
  if (!lh) {
    std::fprintf(stderr, "kms: add_layer: %s\n", lh.error().message().c_str());
    return nullptr;
  }
  source_ = dynamic_cast<DecoderSource*>(&scene_->get_layer(*lh)->source());
  return source_;
}

int KmsPresenter::run(const std::atomic<bool>& quit) {
  std::atomic<bool> flip_pending{false};
  drm::PageFlip page_flip(dev_);
  page_flip.set_handler(
      [&](uint32_t, uint64_t, uint64_t) { flip_pending = false; });

  // First commit carries the modeset and must block (async modeset isn't
  // allowed); it brings the plane up.
  if (auto r = scene_->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    std::fprintf(stderr, "kms: first commit: %s\n",
                 r.error().message().c_str());
    return 1;
  }
  flip_pending = true;

  while (!quit) {
    // Drain page-flip events; longer timeout while a flip is outstanding.
    pollfd p{dev_.fd(), POLLIN, 0};
    if (::poll(&p, 1, flip_pending ? 100 : 8) > 0 && (p.revents & POLLIN) != 0)
      (void)page_flip.dispatch(0);

    if (!flip_pending) {
      // Non-blocking flip: the kernel paces it to vblank and the polled event
      // above drives the next commit, so the loop isn't gated by ioctl latency.
      auto r = scene_->commit(
          DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
      if (!r) {
        std::fprintf(stderr, "kms: commit: %s\n", r.error().message().c_str());
        return 1;
      }
      flip_pending = true;
    }
  }
  return 0;
}

}  // namespace tv

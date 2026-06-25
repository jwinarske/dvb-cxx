// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "kms_output.h"

#include <xf86drm.h>

#include <cstdio>

namespace tv {
namespace {

// The connector's preferred mode if it advertises one, else the mode with the
// most pixels. Returns false if the connector has no modes.
bool pick_mode(const drmModeConnector* c, drmModeModeInfo& out) {
  int best = -1;
  for (int i = 0; i < c->count_modes; ++i) {
    if ((c->modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0) {
      out = c->modes[i];
      return true;
    }
    if (best < 0 || c->modes[i].hdisplay * c->modes[i].vdisplay >
                        c->modes[best].hdisplay * c->modes[best].vdisplay)
      best = i;
  }
  if (best < 0)
    return false;
  out = c->modes[best];
  return true;
}

// A CRTC that can drive `c`: prefer the one already bound via its encoder, else
// the first CRTC any of the connector's encoders allows.
uint32_t pick_crtc(int fd, const drmModeRes* res, const drmModeConnector* c) {
  if (c->encoder_id != 0) {
    if (drmModeEncoder* enc = drmModeGetEncoder(fd, c->encoder_id)) {
      const uint32_t crtc = enc->crtc_id;
      drmModeFreeEncoder(enc);
      if (crtc != 0)
        return crtc;
    }
  }
  for (int e = 0; e < c->count_encoders; ++e) {
    drmModeEncoder* enc = drmModeGetEncoder(fd, c->encoders[e]);
    if (enc == nullptr)
      continue;
    for (int i = 0; i < res->count_crtcs; ++i) {
      if ((enc->possible_crtcs & (1U << i)) != 0U) {
        const uint32_t crtc = res->crtcs[i];
        drmModeFreeEncoder(enc);
        return crtc;
      }
    }
    drmModeFreeEncoder(enc);
  }
  return 0;
}

}  // namespace

std::optional<Output> select_output(int fd) {
  drmModeRes* res = drmModeGetResources(fd);
  if (res == nullptr) {
    std::fprintf(stderr, "kms: drmModeGetResources failed\n");
    return std::nullopt;
  }
  std::optional<Output> result;
  for (int i = 0; i < res->count_connectors && !result; ++i) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
    if (c == nullptr)
      continue;
    if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      Output o;
      o.connector_id = c->connector_id;
      if (pick_mode(c, o.mode)) {
        o.crtc_id = pick_crtc(fd, res, c);
        if (o.crtc_id != 0) {
          std::fprintf(stderr, "kms: connector %u, CRTC %u, mode %s %ux%u@%u\n",
                       o.connector_id, o.crtc_id, o.mode.name, o.mode.hdisplay,
                       o.mode.vdisplay, o.mode.vrefresh);
          result = o;
        }
      }
    }
    drmModeFreeConnector(c);
  }
  drmModeFreeResources(res);
  if (!result)
    std::fprintf(stderr, "kms: no connected output with a usable CRTC found\n");
  return result;
}

}  // namespace tv

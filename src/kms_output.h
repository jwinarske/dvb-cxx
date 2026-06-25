// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Minimal KMS output selection over raw libdrm: pick the first connected
// connector, its preferred (or largest) mode, and a CRTC that can drive it.
// Kept self-contained so tvtuner doesn't depend on drm-cxx's examples/common.
#include <xf86drmMode.h>

#include <cstdint>
#include <optional>

namespace tv {

struct Output {
  uint32_t connector_id = 0;
  uint32_t crtc_id = 0;
  drmModeModeInfo mode{};  // chosen mode (timing + hdisplay/vdisplay)
};

// Returns the selected output, or nullopt if no connected connector with a
// usable mode and CRTC was found. `fd` must be a DRM master fd
// (atomic-capable).
std::optional<Output> select_output(int fd);

}  // namespace tv

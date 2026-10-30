/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file gameengine/Ketsji/KX_CameraRenderMemory.cpp
 *  \ingroup ketsji
 */

#include "KX_CameraRenderMemory.h"

#include <algorithm>
#include <cmath>

namespace KX_CameraRenderMemory {

int ShadowPoolForSplitView(const int scene_pool_mb,
                           const int64_t view_pixels,
                           const int64_t window_pixels)
{
  if (scene_pool_mb <= 0 || view_pixels <= 0 || window_pixels <= 0 ||
      view_pixels >= window_pixels)
  {
    return scene_pool_mb;
  }
  const double share = std::sqrt(double(view_pixels) / double(window_pixels));
  const int pool = int(std::ceil(double(scene_pool_mb) * share / 16.0)) * 16;
  return std::clamp(pool, std::min(16, scene_pool_mb), scene_pool_mb);
}

bool CanShareRenderer(const PassSettings &settings)
{
  if (settings.image_render) {
    return false;
  }
  /* Workbench builds its whole draw from the objects at every pass, hidden ones included. */
  if (settings.workbench) {
    return true;
  }
  return !settings.hides_objects && !settings.temporal_reprojection && !settings.ray_tracing &&
         !settings.shadows && !settings.motion_blur && !settings.depth_of_field;
}

}  // namespace KX_CameraRenderMemory

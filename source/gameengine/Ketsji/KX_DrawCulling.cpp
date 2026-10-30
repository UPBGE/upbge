/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file gameengine/Ketsji/KX_DrawCulling.cpp
 *  \ingroup ketsji
 */

#include "KX_DrawCulling.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"

namespace KX_DrawCulling {

using namespace blender;

View MakeView(const float camera_to_world[4][4], const float projection[4][4])
{
  View view;
  /* The camera's place and directions, without its scale (a camera's scale changes nothing it
   * shows). */
  float camera[4][4];
  copy_m4_m4(camera, camera_to_world);
  normalize_m4(camera);
  copy_v3_v3(view.position, camera[3]);
  negate_v3_v3(view.axis, camera[2]);

  float clip_to_camera[4][4];
  float corners[8][3];
  bool valid = invert_m4_m4(clip_to_camera, projection);
  float far_depth = FLT_MAX;
  for (int i = 0; i < 8 && valid; i++) {
    float point[4] = {(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f, 1.0f};
    mul_m4_v4(clip_to_camera, point);
    if (!(std::fabs(point[3]) > 1e-12f)) {
      valid = false;
      break;
    }
    mul_v3_v3fl(corners[i], point, 1.0f / point[3]);
    if (i & 4) {
      far_depth = std::min(far_depth, -corners[i][2]);
    }
  }
  if (!valid || !(far_depth > 0.0f) || !std::isfinite(far_depth)) {
    /* An odd projection: everything counts as seen. */
    view.far_depth = FLT_MAX;
    copy_v3_v3(view.center, view.position);
    view.radius = FLT_MAX;
    return view;
  }
  view.far_depth = far_depth;

  /* Any point inside the corners is within the farthest corner's distance of their average. */
  float center[3] = {0.0f, 0.0f, 0.0f};
  for (const float *corner : corners) {
    madd_v3_v3fl(center, corner, 1.0f / 8.0f);
  }
  float radius = 0.0f;
  for (const float *corner : corners) {
    radius = std::max(radius, len_v3v3(center, corner));
  }
  mul_v3_m4v3(view.center, camera, center);
  view.radius = radius;
  return view;
}

bool BeyondFarPlane(const View &view, const float center[3], const float radius)
{
  float to_center[3];
  sub_v3_v3v3(to_center, center, view.position);
  return dot_v3v3(to_center, view.axis) - radius > view.far_depth;
}

bool ShadowMayReachView(const View &view,
                        const Light &light,
                        const float center[3],
                        const float radius)
{
  /* The shadow leaves the sphere along `direction` and widens by `spread` per unit of length:
   * its points at length t along the axis lie within radius + spread * t of it. */
  float direction[3];
  float spread;
  if (light.directional) {
    copy_v3_v3(direction, light.vector);
    spread = light.spread;
  }
  else {
    sub_v3_v3v3(direction, center, light.vector);
    const float length = normalize_v3(direction);
    /* Lines from the light's edge past the sphere's far side. */
    const float reach = radius + light.spread;
    if (length <= reach) {
      return true;
    }
    spread = reach / length;
  }
  if (!(spread < 1.0f)) {
    return true;
  }
  float to_view[3];
  sub_v3_v3v3(to_view, view.center, center);
  const float along = dot_v3v3(to_view, direction);
  /* A shadow point at length t meeting the view sphere is within view.radius + radius +
   * spread * t of its center, so t - along is at most that much: past `farthest` no shadow point
   * can. */
  const float farthest = (along + view.radius + radius) / (1.0f - spread);
  if (farthest < 0.0f) {
    return false;
  }
  float nearest[3];
  madd_v3_v3v3fl(nearest, center, direction, std::clamp(along, 0.0f, farthest));
  return len_v3v3(nearest, view.center) <= view.radius + radius + spread * farthest;
}

bool BeyondDrawDistance(const View &view,
                        const float center[3],
                        const float radius,
                        const float distance,
                        const bool culled_before)
{
  const float limit = culled_before ? distance * DistanceHysteresis : distance;
  return len_v3v3(center, view.position) - radius > limit;
}

}  // namespace KX_DrawCulling

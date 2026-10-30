/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_DrawCulling.h
 *  \ingroup ketsji
 *
 * Draw culling of a camera pass (KX_Scene::CulledInCameraPass): objects the camera cannot show
 * are left out of the drawing, which costs Blender's draw code time for every object it is given,
 * seen or not. Two rules:
 * - Clip End: an object entirely beyond the camera's far plane, whose shadow cannot reach what
 *   the camera sees either. Automatic, nothing on screen changes.
 * - Draw distance: an object farther than the distance its "draw_distance" game property or one
 *   of its collections' "draw_distance" custom property gives (measured from the camera to the
 *   nearest point of its bounding sphere). The game creator's choice.
 * Objects are spheres here (KX_GameObject's cull bounds); the math is conservative: when unsure,
 * an object is drawn.
 */

#pragma once

namespace KX_DrawCulling {

/** What a camera pass can see, in world space. */
struct View {
  float position[3] = {0.0f, 0.0f, 0.0f};
  /** The direction the camera looks (unit length). */
  float axis[3] = {0.0f, 0.0f, -1.0f};
  /** Depth of the far plane (Clip End) along #axis. */
  float far_depth = 0.0f;
  /** A sphere enclosing everything the camera can see. */
  float center[3] = {0.0f, 0.0f, 0.0f};
  float radius = 0.0f;
};

/** A light that casts shadows. */
struct Light {
  /** A sun: #vector is the direction its light travels (unit length), #spread the tangent of its
   * angular radius. Otherwise #vector is the light's position and #spread its own radius. */
  bool directional = false;
  float vector[3] = {0.0f, 0.0f, 0.0f};
  float spread = 0.0f;
};

/** Reappearing takes coming this much closer than the draw distance: a camera moving back and
 * forth at the distance (a walk's sway) does not make an object blink. */
constexpr float DistanceHysteresis = 0.95f;

/**
 * The view of a camera: \a camera_to_world is the camera's world transform (it looks down its -Z
 * axis), \a projection its projection matrix (column major, as OpenGL), whose clip space corners
 * give the view volume (perspective or orthographic, shifted or not).
 */
View MakeView(const float camera_to_world[4][4], const float projection[4][4]);

/** Whether a sphere lies entirely beyond the view's far plane. */
bool BeyondFarPlane(const View &view, const float center[3], float radius);

/** Whether \a light can cast the sphere's shadow into the view (true when unsure). */
bool ShadowMayReachView(const View &view, const Light &light, const float center[3], float radius);

/** Whether a sphere is farther than \a distance from the camera; \a culled_before: it was in
 * this camera's last pass (it comes back only inside #DistanceHysteresis of the distance). */
bool BeyondDrawDistance(const View &view,
                        const float center[3],
                        float radius,
                        float distance,
                        bool culled_before);

}  // namespace KX_DrawCulling

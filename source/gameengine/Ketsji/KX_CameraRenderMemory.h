/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_CameraRenderMemory.h
 *  \ingroup ketsji
 *
 * GPU memory rules of the game cameras. Each camera view draws through a renderer (a
 * GPUViewport with its own EEVEE or Workbench instance: world lighting, shadow pool, screen-size
 * buffers). These rules decide how big a view's shadow pool is, which camera views may draw
 * through one shared renderer, and when an unused renderer is given back.
 */

#pragma once

#include <cstdint>

namespace KX_CameraRenderMemory {

/** A camera's renderer is given back when the camera has not drawn for this long (seconds). */
constexpr double IdleSeconds = 5.0;

/**
 * The shadow pool (MB) of a split screen player's view covering `view_pixels` of a window of
 * `window_pixels`: the scene's pool times the square root of the view's share of the window,
 * rounded up to 16 MB (a window resize rarely changes it; a change makes EEVEE re-create the
 * pool). A view needs fewer shadow pages than the full window, but not fewer in proportion: at
 * 4K one view used 330 pages, each half of a split 211 (64%, under the square root's 71%), a
 * 557x626 view 100 (30%). So only player views, never smaller views, use this. A view as large
 * as the window keeps the scene's pool.
 */
int ShadowPoolForSplitView(int scene_pool_mb, int64_t view_pixels, int64_t window_pixels);

/** What one camera pass draws with. Passes with equal settings can draw through one renderer. */
struct PassSettings {
  int width = 0;
  int height = 0;
  /** The menu/HUD pass (KX_Scene::GetOverlayCamera). */
  bool overlay = false;
  bool workbench = false;
  /** Effective EEVEE flags of the pass (SceneEEVEE::flag after the camera switches). */
  int eevee_flag = 0;
  int shadow_pool = 0;
  bool motion_blur = false;
  /** Workbench options (GAME_CAM_WORKBENCH_* bits). */
  int workbench_flags = 0;
  bool perspective = true;

  /* What keeps history from one frame to the next in the camera's own renderer. */
  bool temporal_reprojection = false;
  bool ray_tracing = false;
  bool shadows = false;
  bool depth_of_field = false;
  /** Set Camera Visibility or the camera's Collection setting hides objects from this camera.
   * EEVEE notices a camera change, not a visibility change, so another camera at the same place
   * could blend into this one. */
  bool hides_objects = false;
  /** The camera has drawn into a texture (ImageRender): the texture uses its own renderer. */
  bool image_render = false;

  bool operator==(const PassSettings &other) const = default;
};

/**
 * True when the pass keeps nothing from one frame to the next in its renderer, so it can draw
 * through a renderer shared with other passes of equal settings: every pass then starts from a
 * clean renderer, as when the camera moves. With EEVEE that needs Temporal Reprojection, ray
 * tracing (temporal denoise), motion blur, depth of field (stabilize history) and shadows (the
 * cached shadow pages follow the view) off, and no objects hidden from the camera. Workbench
 * restarts its smoothing every game frame (the game counts as playback) and builds its draw from
 * the objects at every pass, so it shares even with objects hidden. Never a render to texture.
 */
bool CanShareRenderer(const PassSettings &settings);

}  // namespace KX_CameraRenderMemory

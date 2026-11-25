/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

class GameInstance;

struct SSRSettings {
  /* Reflection closures rougher than this keep the world reflection only. */
  float max_roughness = 0.5f;
  /* Upper bound of the screen-space march steps, one step every 4 pixels below it. */
  int max_steps = 48;
  /* Depth a hit may be behind the depth buffer, relative to the view distance. */
  float thickness = 0.05f;
  bool enabled = true;
};

/**
 * Screen-space reflections, half resolution: one mirror ray per pixel for the first GGX
 * reflection closure, against the opaque depth, reading the previous frame's opaque radiance.
 * The result (radiance, confidence) is blended over the world reflection by the lighting pass.
 */
class RayTraceModule {
 public:
  RayTraceModule(GameInstance *inst);

  void init();
  void sync();
  void render(gpu::Texture *scene_color_tx, gpu::Texture *depth_tx);

  /* When false the result texture holds nothing meaningful. */
  bool enabled() const
  {
    return settings_.enabled;
  }
  gpu::Texture *get_result()
  {
    return ssr_final_tx_;
  }

 private:
  GameInstance *inst_;
  SSRSettings settings_;
  /* Camera of the frame whose radiance is traced. */
  float4x4 viewproj_prev_ = float4x4::identity();

  Texture ssr_lowres_tx_ = {"ssr_lowres_tx"};
  Texture ssr_final_tx_ = {"ssr_final_tx"};

  PassSimple ssr_trace_ps_ = {"SSR.Trace"};
  PassSimple ssr_upsample_ps_ = {"SSR.Upsample"};
};

}  // namespace blender::eevee_game

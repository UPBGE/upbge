/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"
#include "GPU_framebuffer.hh"
#include "draw_handle.hh"   /* ResourceHandleRange */
#include "DRW_render.hh"    /* Object */

namespace blender::eevee_game {

/* Maximum punctual shadow slots in the lower portion of the atlas.
 * Atlas layout (4096x4096):
 *   Top row    : 4 cascade tiles of 1024x1024 (CSM for one directional light).
 *   Rows 1-3   : 4 columns x 3 rows = 12 punctual slots of 1024x1024.
 * Total punctual capacity: 12 spot/point lights with dedicated shadow maps. */
#define MAX_PUNCTUAL_SHADOW_SLOTS 12

struct ShadowData {
  float    global_lod_bias       = 0.0f;
  uint32_t shadow_ray_count      = 1;
  uint32_t shadow_ray_step_count = 6;
  bool     use_pcf               = true;
  bool     use_pcss              = false;
  float    pcf_offset_scale      = 1.0f;
  float    light_source_radius   = 0.01f; /* World-space radius for PCSS penumbra */

  /* Cascade matrices written by update_cascades() and uploaded as a UBO to shaders */
  float4x4 cascade_viewproj[MAX_SHADOW_CASCADES] = {};
  float4   cascade_splits                         = float4(0.0f);
};

/* One cascade: tight orthographic view-projection matrix + camera-space split depth */
struct ShadowCascade {
  float4x4 view_proj;
  float    split_depth;
};

/* One punctual light shadow slot */
struct ShadowPunctual {
  float4x4 view_proj;
  uint32_t atlas_index;
  bool     active;
};

class ShadowModule {
 public:
  ShadowModule(class GameInstance &inst, ShadowData &data);
  ~ShadowModule() = default;

  void init();
  void begin_sync();
  void end_sync();

  /* Register an object as a shadow caster.
   * Tracks which objects need to be rendered from each light's POV.
   * Called once per mesh from SyncModule::sync_mesh(). */
  void sync_object(Object *ob, ResourceHandleRange res_handle);

  /* Render all shadow maps into the atlas from the given camera's perspective,
   * then dispatch the PCF compute pass to populate shadow_mask_tx. */
  void set_view(View &view, int2 extent);

  const ShadowData  &get_data()  const { return data_; }
  gpu::Texture      *get_atlas()       { return atlas_tx_.get(); }

  /* Bind shadow atlas texture to a render pass (used by forward pass).
   * The deferred lighting pass reads shadow_mask_tx directly from RenderBuffers. */
  void bind_resources(PassSimple &ps);

 private:
  /* Compute tight cascade orthographic projections by projecting frustum corners
   * into light space and computing the resulting AABB. */
  void update_cascades(class Camera &camera, class LightEntry &light);

  void allocate_atlas();

  /* Build and dispatch the PCF 3x3 compute pass.
   * Reads depth_tx + normal_tx (G-Buffer), writes shadow_mask_tx.
   * Called from set_view() after the atlas rasterisation is complete. */
  void dispatch_pcf(int2 extent);

  GameInstance *inst_;
  ShadowData   &data_;

  std::unique_ptr<gpu::Texture> atlas_tx_;

  /* Depth-only pass submitted from each light's viewpoint */
  PassSimple  shadow_pass_ps_{"Shadow.Pass"};
  Framebuffer shadow_fb_;

  /* PCF compute pass: one shadow factor per pixel, written into shadow_mask_tx.
   * Re-initialised each set_view() call to rebind the current frame's textures. */
  PassSimple pcf_ps_{"Shadow.PCF"};

  std::vector<ShadowCascade>  cascades_;
  std::vector<ShadowPunctual> punctual_lights_;

  bool do_full_update_ = false;
};

} // namespace blender::eevee_game

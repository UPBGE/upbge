/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"
#include "GPU_framebuffer.hh"
#include "draw_handle.hh"   /* ResourceHandleRange */
#include "DRW_render.hh"    /* Object */

#include <memory>
#include <vector>

namespace blender::eevee_game {

/* -----------------------------------------------------------------------
 * Atlas layout
 *
 * STATIC atlas — 4096x4096, tiles 1024x1024.
 *   Top row  : 4 CSM cascade tiles (one directional light).
 *   Rows 1-3 : 4 columns × 3 rows = 12 punctual slots.
 *   Re-rendered only when geometry or a light changes (dirty flag).
 *   Most frames: zero draw calls here.
 *
 * DYNAMIC atlas — square, tiles 512x512. Re-rendered every frame.
 *   Size chosen at init() via DynamicShadowSlots:
 *
 *     OFF →  0 slots  → no VRAM allocated
 *     S4  →  4 slots  → 1024×1024  (~1 MB D32F)
 *     S16 → 16 slots  → 2048×2048  (~4 MB D32F)   ← default
 *     S64 → 64 slots  → 4096×4096  (~16 MB D32F)
 *
 *   N must be a perfect square: tiles_per_side = sqrt(N) must be integer
 *   so the atlas is square and UV coordinate math in the shader stays
 *   symmetric and branchless. (4→2, 16→4, 64→8.)
 * ----------------------------------------------------------------------- */

#define STATIC_ATLAS_RES      4096
#define STATIC_TILE_SIZE      1024
#define STATIC_PUNCTUAL_SLOTS 12   /* 4 cols × 3 rows below the CSM row */

#define DYNAMIC_TILE_SIZE     512

/* Selector for the dynamic atlas size. Exposed to game settings UI. */
enum class DynamicShadowSlots : uint32_t {
  OFF = 0,
  S4  = 4,
  S16 = 16,
  S64 = 64,
};

/* Set in LightData.flags for lights that should use the dynamic atlas.
 * Lights without this flag fall back to the static atlas. */
#define LIGHT_FLAG_SHADOW_DYNAMIC (1u << 0)

/* -----------------------------------------------------------------------
 * ShadowData — GPU-visible parameters (push constants / UBO payload)
 * ----------------------------------------------------------------------- */

struct ShadowData {
  float    global_lod_bias       = 0.0f;
  uint32_t shadow_ray_count      = 1;
  uint32_t shadow_ray_step_count = 6;
  bool     use_pcf               = true;
  bool     use_pcss              = false;
  float    pcf_offset_scale      = 1.0f;
  float    light_source_radius   = 0.01f; /* World-space radius for PCSS penumbra */

  /* Cascade matrices written by update_cascades(), uploaded as push constants */
  float4x4 cascade_viewproj[MAX_SHADOW_CASCADES] = {};
  float4   cascade_splits                         = float4(0.0f);
};

/* -----------------------------------------------------------------------
 * Per-slot CPU descriptors
 * ----------------------------------------------------------------------- */

/* One CSM cascade in the static atlas */
struct ShadowCascade {
  float4x4 view_proj;
  float    split_depth;
};

/* One punctual light slot (static or dynamic atlas) */
struct ShadowPunctual {
  float4x4 view_proj;
  uint32_t atlas_index;   /* Flat tile index within its own atlas */
  bool     active;
  bool     is_dynamic;    /* True → lives in the dynamic atlas */
  bool     needs_update;  /* True → must re-render this frame (static path only) */
};

/* -----------------------------------------------------------------------
 * ShadowModule
 * ----------------------------------------------------------------------- */

class ShadowModule {
 public:
  ShadowModule(class GameInstance &inst, ShadowData &data);
  ~ShadowModule() = default;

  /* Allocates both atlases. Must be called once before the first frame.
   * dynamic_slots controls dynamic atlas VRAM; static atlas is always 4096. */
  void init(DynamicShadowSlots dynamic_slots = DynamicShadowSlots::S16);

  void begin_sync();
  void end_sync();

  /* Register an object as a shadow caster.
   * Called once per mesh from SyncModule::sync_mesh().
   *
   * TODO (dirty tracking): intersect ob's world AABB against each static
   * light's influence radius; set needs_update = true on matching slots.
   * Until then, all static slots refresh every frame (conservative). */
  void sync_object(Object *ob, ResourceHandleRange res_handle);

  /* Render both shadow atlases, then dispatch the PCF compute pass that
   * writes shadow_mask_tx for the deferred lighting pass. */
  void set_view(View &view, int2 extent);

  const ShadowData &get_data()         const { return data_; }
  gpu::Texture     *get_static_atlas()       { return static_atlas_tx_.get(); }
  /* May be null when dynamic_slots_ == OFF. */
  gpu::Texture     *get_dynamic_atlas()      { return dynamic_atlas_tx_.get(); }

  /* Returns the configured slot count (0 when OFF). Used by instance.cc
   * for shadow_index encoding during end_sync(). */
  int dynamic_slot_count() const { return int(uint32_t(dynamic_slots_)); }

  /* Bind both atlas textures to a render pass (used by forward shading). */
  void bind_resources(PassSimple &ps);

 private:
  void update_cascades(class Camera &camera, class LightEntry &light);
  void allocate_static_atlas();
  void allocate_dynamic_atlas();

  /* Re-render static slots that have needs_update == true. */
  void render_static_atlas(View &view);

  /* Re-render all dynamic slots (runs every frame). */
  void render_dynamic_atlas(View &view);

  /* Dispatch the PCF 3x3 compute shader.
   * Reads G-Buffer depth + normals, writes shadow_mask_tx. */
  void dispatch_pcf(int2 extent);

  GameInstance       *inst_;
  ShadowData         &data_;
  DynamicShadowSlots  dynamic_slots_ = DynamicShadowSlots::S16;

  /* Static atlas: persistent, re-rendered on dirty only */
  std::unique_ptr<gpu::Texture> static_atlas_tx_;
  Framebuffer                   static_fb_;

  /* Dynamic atlas: re-rendered every frame; null when OFF */
  std::unique_ptr<gpu::Texture> dynamic_atlas_tx_;
  Framebuffer                   dynamic_fb_;

  /* Depth-only geometry pass — reused for both atlases */
  PassSimple shadow_pass_ps_{"Shadow.Pass"};

  /* PCF 3x3 compute pass → shadow_mask_tx */
  PassSimple pcf_ps_{"Shadow.PCF"};

  /* CSM cascades — always live in the static atlas */
  std::vector<ShadowCascade> cascades_;

  /* Punctual light slots — routed by LIGHT_FLAG_SHADOW_DYNAMIC */
  std::vector<ShadowPunctual> static_punctual_;   /* max STATIC_PUNCTUAL_SLOTS */
  std::vector<ShadowPunctual> dynamic_punctual_;  /* max dynamic_slots_ count  */
};

} // namespace blender::eevee_game

/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"
#include "GPU_framebuffer.hh"
#include "draw_handle.hh"   /* ResourceHandleRange */
#include "DRW_render.hh"    /* Object */
#include "../eevee/eevee_defines.hh" /* Resource slots of the forward interface */

#include <memory>
#include <vector>

namespace blender::eevee_game {

struct LightEntry;

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
  /* Kept separate: draw::View wants view and window matrices apart. */
  float4x4 view;
  float4x4 proj;
  float4x4 view_proj;
  float    split_depth;
};

/* One punctual light slot (static or dynamic atlas). A spot light renders one
 * face over the whole tile, a point light six 90 degree faces packed 3x2 in
 * the tile. */
struct ShadowPunctual {
  float4x4 face_view[6];
  float4x4 proj;
  float3   position;
  float    radius;        /* Influence radius: caster search and far plane */
  uint64_t uid;
  int      light_index;   /* Entry in LightModule::entries_ */
  int      face_count;    /* 1 spot, 6 point and area */
  uint32_t face_mask;     /* Faces to render: an area light skips those behind it */
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
   * Called once per mesh from SyncModule::sync_mesh(). uid is the stable
   * ObjectKey hash. Feeds the cascade near plane and the caster signatures
   * that decide which static tiles are re-rendered. */
  void sync_object(Object *ob, uint64_t uid);

  /* Render both shadow atlases, then dispatch the PCF compute pass that
   * writes shadow_mask_tx for the deferred lighting pass. */
  void set_view(View &view, int2 extent);

  const ShadowData &get_data()         const { return data_; }
  gpu::Texture     *get_static_atlas()       { return static_atlas_tx_; }
  /* May be null when dynamic_slots_ == OFF. */
  gpu::Texture     *get_dynamic_atlas()      { return dynamic_atlas_tx_; }

  /* Returns the configured slot count (0 when OFF). Used by instance.cc
   * for shadow_index encoding during end_sync(). */
  int dynamic_slot_count() const { return int(uint32_t(dynamic_slots_)); }

  /* Bind the shadow face buffer and both atlases (punctual shadow lookup). */
  void bind_resources(PassSimple &ps);
  /* Same, by the slots of `eevee_game_light_data`: for passes whose shaders
   * are only known per sub-pass (forward materials). */
  template<typename PassT> void bind_resources_by_slot(PassT &pass)
  {
    pass.bind_ssbo(LIGHT_TILE_BUF_SLOT, &tile_buf_);
    pass.bind_texture(SHADOW_TILEMAPS_TEX_SLOT, static_atlas_tx_);
    pass.bind_texture(SHADOW_ATLAS_TEX_SLOT,
                      dynamic_atlas_tx_.is_valid() ? (gpu::Texture *)dynamic_atlas_tx_ :
                                                     (gpu::Texture *)static_atlas_tx_);
  }

 private:
  void update_cascades(class Camera &camera, struct LightEntry &light);
  void allocate_static_atlas();
  void allocate_dynamic_atlas();

  /* Re-render the CSM cascades and the static punctual slots. */
  void render_static_atlas();

  /* Re-render all dynamic slots (runs every frame). */
  void render_dynamic_atlas();

  /* Draw the shadow casters into one atlas tile from one light's viewpoint. */
  void render_tile(Framebuffer &fb, View &view, int2 tile_origin, int tile_size);
  /* Render every face of a punctual slot into its tile. */
  void render_punctual(Framebuffer &fb,
                       std::vector<std::unique_ptr<View>> &views,
                       const ShadowPunctual &slot,
                       int2 tile_origin,
                       int tile_size);
  /* Build the faces of a slot and write them to tile_buf_. */
  void punctual_faces_build(ShadowPunctual &slot, const LightEntry &light, int tile_size);
  void punctual_tiles_write(const ShadowPunctual &slot,
                            int2 tile_origin,
                            int tile_size,
                            int atlas_res,
                            int atlas);
  /* Order independent hash of the casters touching a sphere. */
  uint64_t caster_signature(float3 center, float radius) const;

  /* Dispatch the PCF 3x3 compute shader.
   * Reads G-Buffer depth + normals, writes shadow_mask_tx. */
  void dispatch_pcf(int2 extent);

  GameInstance       *inst_;
  ShadowData         &data_;
  DynamicShadowSlots  dynamic_slots_ = DynamicShadowSlots::S16;

  /* Static atlas: persistent, re-rendered on dirty only */
  Texture static_atlas_tx_ = {"shadow_static_atlas"};
  Framebuffer static_fb_ = {"shadow_static_fb"};

  /* Dynamic atlas: re-rendered every frame; null when OFF */
  Texture dynamic_atlas_tx_ = {"shadow_dynamic_atlas"};
  Framebuffer dynamic_fb_ = {"shadow_dynamic_fb"};

  /* One persistent draw::View per atlas tile. A View owns GPU buffers
   * (matrices, culling data, visibility bits), so building one per tile per
   * frame would allocate and free dozens of buffers every frame. Created
   * once in init(); only sync() is called per frame. */
  std::vector<std::unique_ptr<View>> cascade_views_;
  std::vector<std::unique_ptr<View>> static_views_;
  std::vector<std::unique_ptr<View>> dynamic_views_;

  /* PCF 3x3 compute pass → shadow_mask_tx */
  PassSimple pcf_ps_{"Shadow.PCF"};

  /* CSM cascades — always live in the static atlas */
  std::vector<ShadowCascade> cascades_;

  /* Punctual light slots — routed by LIGHT_FLAG_SHADOW_DYNAMIC */
  std::vector<ShadowPunctual> static_punctual_;   /* max STATIC_PUNCTUAL_SLOTS */
  std::vector<ShadowPunctual> dynamic_punctual_;  /* max dynamic_slots_ count  */

  /* World AABB of the shadow casters synced this frame (empty: min > max). */
  float3 caster_min_ = float3(FLT_MAX);
  float3 caster_max_ = float3(-FLT_MAX);

  /* Casters of this frame: world box and a key that changes when the object
   * moves, deforms or is a different object. */
  struct CasterRecord {
    float3   center;
    float3   half_size;
    uint64_t key;
  };
  std::vector<CasterRecord> casters_;
  uint64_t casters_signature_ = 0;

  /* What each static tile holds, to skip the tiles that would be redrawn
   * identical: same light, same matrices, same casters. */
  struct StaticTileCache {
    uint64_t light_uid;
    float4x4 face_view0;
    float4x4 proj;
    uint64_t signature;
    uint32_t face_mask;
    bool     valid;
    bool     used;        /* Claimed by a light this frame */
  };
  StaticTileCache static_cache_[STATIC_PUNCTUAL_SLOTS] = {};

  struct CascadeCache {
    float4x4 view_proj[MAX_SHADOW_CASCADES];
    int      count;
    uint64_t signature;
    bool     valid;
  };
  CascadeCache cascade_cache_ = {};
  bool cascades_need_update_ = true;

  /* Faces of every shadowed punctual light, indexed by LightData::shadow_index. */
  StorageArrayBuffer<ShadowTileData, MAX_SHADOW_TILES> tile_buf_ = {"shadow_tile_buf"};
  int tile_count_ = 0;
};

} // namespace blender::eevee_game

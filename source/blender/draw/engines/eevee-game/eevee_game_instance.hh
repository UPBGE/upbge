/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "eevee_game_bloom.hh"
#include "eevee_game_culling.hh"
#include "eevee_game_depth_of_field.hh"
#include "eevee_game_film.hh"
#include "eevee_game_gtao.hh"
#include "eevee_game_pipeline.hh"
#include "eevee_game_raytrace.hh"
#include "eevee_game_renderbuffers.hh"
#include "eevee_game_material.hh"
#include "eevee_game_shader.hh"
#include "eevee_game_shadow.hh"
#include "eevee_game_ssgi.hh"
#include "eevee_game_sync.hh"
#include "eevee_game_upscaling.hh"
#include "eevee_game_velocity.hh"
#include "eevee_game_volume.hh"
#include "eevee_game_world.hh"
#include "eevee_game_view.hh"

#include "../eevee/eevee_defines.hh" /* HIZ_MIP_COUNT, HIZ_GROUP_SIZE */

#include "DRW_gpu_wrapper.hh"
#include "GPU_storage_buffer.hh"
#include "DNA_camera_types.h"
#include "DNA_object_types.h"

struct Depsgraph;
struct DRWContext;
struct Object;
struct RenderEngine;
struct Scene;
struct ViewLayer;

namespace blender::eevee_game {

/* Maximum punctual (spot/point) lights that can receive shadow atlas slots.
 * The atlas bottom half provides this many tiles; must match ShadowModule layout. */
#define MAX_PUNCTUAL_SHADOW_SLOTS 16

/* ---- HiZBuffer ---- */

/* Handle every consumer binds by reference (`&hiz_buffer.front.ref_tx_`). */
struct HiZBuffer {
  gpu::Texture *ref_tx_ = nullptr;
};

/* One pyramid is enough: within a frame it is read by culling (still holding
 * the previous frame), rebuilt right after the opaque prepass, then read by
 * GTAO / SSGI / SSR. Layout follows `eevee_hiz_update`: R32F, HIZ_MIP_COUNT
 * mips, extent padded to a multiple of the coarsest texel so no mip ever has
 * an odd size. Stores non reversed depth, farthest value per texel. */
struct HiZBufferPair {
  HiZBuffer front;

  draw::Texture tx_ = {"hiz_tx"};
  std::array<gpu::Texture *, HIZ_MIP_COUNT> mip_ref_ = {};
  /* Counts finished tiles so the last one down-samples the remaining mips. */
  draw::StorageBuffer<uint4, true> tile_counter_ = {"hiz_tile_counter"};

  /* View-projection the pyramid was rasterized with. */
  float4x4 persmat = float4x4::identity();
  /* render_extent / padded extent. Consumers must scale screen UVs by this. */
  float2 uv_scale = float2(1.0f);
  /* False until the first update after (re)allocation. */
  bool valid = false;

  void ensure(int2 render_res);
};

/* ---- Camera ---- */

struct CameraData {
  float clip_near    = 0.1f;
  float clip_far     = 1000.0f;
  float focal_length = 50.0f;
  float sensor_width = 36.0f;
};

class Camera {
 public:
  void sync(const Object *cam_ob);
  const CameraData &data_get() const { return data_; }
  float3             position()  const { return float3(object_to_world[3]); }
  float4x4 object_to_world = float4x4::identity();
 private:
  CameraData data_;
};

/* ---- LightEntry ---- */

struct LightEntry {
  LightType type;
  float4x4  object_to_world;
  LightData data;
  /* Stable identity across frames (ObjectKey hash): keeps a static shadow
   * slot and its cached atlas tile bound to the same light. */
  uint64_t  uid = 0;
  /* CPU only: spot cone angle (full, radians) of the shadow frustum, and
   * LIGHT_FLAG_* bits. */
  float spot_angle = 0.0f;
  uint32_t flags = 0;
};

/* ---- LightModule ---- */

struct LightModule {
  /* Rebuilt every frame and only ever walked front to back, so a flat
   * array: no hashing, and iteration order is the sync order. */
  Vector<LightEntry> entries_;
  StorageArrayBuffer<LightData, MAX_LIGHTS> light_buf_ = {"light_buf"};
  int active_light_count = 0;

  void begin_sync();
  void add(const LightEntry &entry);

  /**
   * Pack and upload the light array. Called once per frame from
   * GameInstance::end_sync(), after shadow indices have been written back
   * into entries_.
   */
  void upload();

  void bind_resources(PassSimple &ps);
};

/* ---- GameInstance ---- */

/**
 * GameInstance owns every sub-module and acts as the shared context.
 * Declaration order = construction order.
 * 'shaders' must be first.
 */
class GameInstance : public DrawEngine {
 public:
  GameInstance();
  ~GameInstance() = default;

  StringRefNull name_get() final
  {
    return "EEVEE Game";
  }

  /* ---- DrawEngine interface (viewport). The Draw Manager calls these in
   * this order every redraw and iterates the scene itself, calling
   * object_sync() once per evaluated object. ---- */
  void init() final;
  void begin_sync() final;
  void object_sync(ObjectRef &ob_ref, Manager &manager) final;
  void end_sync() final;
  void draw(Manager &manager) final;

  /* Shared by the viewport and the F12 path. */
  void init(const int2 &output_res, const rcti *output_rect);

  void render_frame(struct RenderEngine *engine, struct RenderLayer *layer);
  void render_sample();

  /* ---- Sub-modules (construction order = declaration order) ---- */

  ShaderModule   shaders;
  MaterialModule materials;   /* Must follow shaders — uses shaders.material_shader_get() */
  Film           film;
  RenderBuffers  render_buffers;
  Camera         camera;
  ShadowModule   shadows;
  VelocityModule velocity;
  SyncModule     sync;        /* Scene traversal — populates lights, velocity, shadows */
  BloomModule    bloom;
  DepthOfField   dof;
  GTAOModule     gtao;
  SSGIModule     ssgi;
  RayTraceModule raytrace;
  VolumeModule   volume;
  WorldModule    world;
  PipelineModule pipelines;
  UpscaleModule  upscale;
  LightModule    lights;

  /* ---- Shared state ---- */

  /* Shared per-frame state propagated to all modules before any sync.
   * camera_cut: set by the game logic layer when the active camera changes
   *   discontinuously (scene load, teleport, camera switch). Consumed by
   *   UpscaleModule (FSR3 reset), SSGIModule and GTAOModule (history discard).
   *   Reset to false at the end of begin_sync() after all modules have read it.
   * history_valid: false on the very first frame and after any camera_cut.
   *   Modules that read from previous-frame buffers (SSGI radiance cache,
   *   GTAO temporal, SSR reprojection) must skip history blending when false. */
  struct FrameState {
    bool camera_cut    = false;
    bool history_valid = false;
    uint32_t  frame_index   = 0;
  } frame_state;

  UniformData     uniform_data;
  UpscaleSettings upscale_settings;
  HiZBufferPair   hiz_buffer;

  PassSimple hiz_update_ps_{"HiZ.Update"};

  struct SamplingState {
    uint64_t sample_index = 0;
  } sampling;

  float delta_time_ms = 16.666f;

  /* Sampler filtering override passed to every material_set() call. */
  GPUSamplerFiltering anisotropic_filtering = GPU_SAMPLER_FILTERING_DEFAULT;

  /* Blender/UPBGE scene pointers (not owned) */
  Scene        *scene        = nullptr;
  ViewLayer    *view_layer   = nullptr;
  Depsgraph    *depsgraph    = nullptr;
  const DRWContext *draw_ctx = nullptr;
  RenderEngine *render_engine_ = nullptr;

  const Object *camera_eval_object = nullptr;
  Manager      *manager            = nullptr;

 private:
  void update_eval_members();

  /**
   * F12 path only: there the engine has to drive the whole sync itself,
   * including the Depsgraph traversal the viewport gets for free.
   */
  void render_sync();

  /* One-time module setup done; init() runs every redraw. */
  bool modules_initialized_ = false;
  /* Render + display extents the FSR3 context was created for. */
  int4 upscale_key_ = int4(0);

  ShadowData  shadow_data_;
  ShadingView main_view_;
};

} // namespace blender::eevee_game

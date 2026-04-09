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
#include "eevee_game_view.hh"

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

struct HiZBuffer {
  gpu::Texture ref_tx_; /* R32F mip chain, mip_count = floor(log2(max_dim)) + 1 */
  void ensure(int2 render_res);
};

struct HiZBufferPair {
  HiZBuffer front;
  HiZBuffer back;
  void swap() { std::swap(front, back); }
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
};

/* ---- LightModule ---- */

struct LightModule {
  blender::Map<int, LightEntry> light_map_;
  std::unique_ptr<gpu::StorageBuffer> light_buf_;
  int active_light_count = 0;

  void init();
  void begin_sync();
  void add(int id, const LightEntry &entry);
  void end_sync();

  /**
   * Re-pack and re-upload the light SSBO after shadow indices have been written
   * back into light_map_ entries by GameInstance::end_sync().
   * FIX: this method was missing — without it the shadow_index stamp was done
   * in memory but never pushed to the GPU buffer.
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
class GameInstance {
 public:
  GameInstance();
  ~GameInstance() = default;

  void init(const int2 &output_res, const rcti *output_rect);
  void begin_sync();
  void end_sync();

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
  CullingModule  culling;
  SyncModule     sync;        /* Scene traversal — populates culling, lights, velocity */
  BloomModule    bloom;
  DepthOfField   dof;
  GTAOModule     gtao;
  SSGIModule     ssgi;
  RayTraceModule raytrace;
  VolumeModule   volume;
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

  struct GBufferDescriptor {
    gpu::Texture *header_tx  = nullptr;
    gpu::Texture *closure_tx = nullptr;
    gpu::Texture *normal_tx  = nullptr;
  } gbuffer;

  struct SamplingState {
    uint64_t sample_index = 0;
  } sampling;

  float delta_time_ms = 16.666f;

  /* Anisotropic filtering level (1, 2, 4, 8, 16). */
  int anisotropic_filtering = 1;

  /* Blender/UPBGE scene pointers (not owned) */
  Scene        *scene        = nullptr;
  ViewLayer    *view_layer   = nullptr;
  Depsgraph    *depsgraph    = nullptr;
  DRWContext   *draw_ctx     = nullptr;
  RenderEngine *render_engine_ = nullptr;

  const Object *camera_eval_object = nullptr;
  Manager      *manager            = nullptr;

 private:
  void update_eval_members();

  /**
   * Full Depsgraph traversal: calls SyncModule::object_sync() for every object.
   * Populates culling, lights, velocity, shadows for the current frame.
   */
  void sync_scene();

  ShadowData  shadow_data_;
  ShadingView main_view_;
};

} // namespace blender::eevee_game

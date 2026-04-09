/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_shadow.hh"
#include "eevee_game_instance.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_geom.h"
#include "DNA_object_types.h"
#include "DNA_light_types.h"

#include <cmath>  /* sqrtf */

namespace blender::eevee_game {

ShadowModule::ShadowModule(GameInstance &inst, ShadowData &data)
    : inst_(&inst), data_(data)
{
}

/* =========================================================================
 * init()
 * ========================================================================= */

void ShadowModule::init(DynamicShadowSlots dynamic_slots)
{
  dynamic_slots_ = dynamic_slots;

  allocate_static_atlas();
  allocate_dynamic_atlas(); /* no-op when OFF */

  /* Depth-only shadow pass — geometry submitted from each light's viewpoint.
   * Shared between both atlases; only the bound framebuffer/viewport changes. */
  shadow_pass_ps_.init();
  shadow_pass_ps_.shader_set(inst_->shaders.static_shader_get(SH_SHADOW_DIRECTIONAL_CSM));
}

void ShadowModule::allocate_static_atlas()
{
  /* Fixed 4096×4096 atlas. Tiles 1024×1024.
   *   Row 0        : 4 CSM cascade tiles (directional light).
   *   Rows 1..3    : 4 columns × 3 rows = 12 punctual slots.
   *
   * Using SFLOAT_32_DEPTH avoids shadow acne at large world scales.
   * GPU_TEXTURE_USAGE_SHADER_READ so the PCF compute can sample it. */
  const int2 res   = int2(STATIC_ATLAS_RES, STATIC_ATLAS_RES);
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT |
                                 GPU_TEXTURE_USAGE_SHADER_READ;

  static_atlas_tx_ = std::make_unique<gpu::Texture>("shadow_static_atlas");
  static_atlas_tx_->ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH, res, usage);

  /* Nearest + compare mode: PCF taps use sampler2DShadow hardware comparison. */
  GPU_texture_filter_mode(static_atlas_tx_->get(), false);
  GPU_texture_compare_mode(static_atlas_tx_->get(), true);

  GPU_framebuffer_ensure_config(
      &static_fb_,
      { GPU_ATTACHMENT_TEXTURE(static_atlas_tx_.get()) });
}

void ShadowModule::allocate_dynamic_atlas()
{
  const uint32_t n = uint32_t(dynamic_slots_);
  if (n == 0) {
    /* DynamicShadowSlots::OFF — nothing to allocate. */
    dynamic_atlas_tx_.reset();
    return;
  }

  /* tiles_per_side = sqrt(n) is always an integer for the allowed values
   * (4→2, 16→4, 64→8). The atlas is square so UV math in the shader is
   * symmetric: tile_uv = vec2(col, row) / float(tiles_per_side). */
  const int tiles_per_side = int(sqrtf(float(n)));
  const int atlas_res      = tiles_per_side * DYNAMIC_TILE_SIZE;
  const int2 res           = int2(atlas_res, atlas_res);

  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT |
                                 GPU_TEXTURE_USAGE_SHADER_READ;

  dynamic_atlas_tx_ = std::make_unique<gpu::Texture>("shadow_dynamic_atlas");
  dynamic_atlas_tx_->ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH, res, usage);

  GPU_texture_filter_mode(dynamic_atlas_tx_->get(), false);
  GPU_texture_compare_mode(dynamic_atlas_tx_->get(), true);

  GPU_framebuffer_ensure_config(
      &dynamic_fb_,
      { GPU_ATTACHMENT_TEXTURE(dynamic_atlas_tx_.get()) });
}

/* =========================================================================
 * Sync
 * ========================================================================= */

void ShadowModule::begin_sync()
{
  static_punctual_.clear();
  dynamic_punctual_.clear();
}

void ShadowModule::update_cascades(Camera &camera, LightEntry &light)
{
  /* Logarithmic split distribution: more resolution near the camera where
   * angular density is highest.
   * split_i = near * (far / near)^(i / N)
   * Standard technique used by every AAA title since 2007. */
  cascades_.resize(MAX_SHADOW_CASCADES);

  const float near_clip = camera.data_get().clip_near;
  const float far_clip  = camera.data_get().clip_far;

  const float4x4 light_view      = math::invert(light.object_to_world);
  const float4x4 cam_viewproj_inv = math::invert(
      inst_->uniform_data.projectionmat * inst_->uniform_data.viewmat);

  for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
    const float slice_near = (i == 0) ?
        near_clip :
        near_clip * powf(far_clip / near_clip, float(i) / float(MAX_SHADOW_CASCADES));
    const float slice_far = near_clip *
        powf(far_clip / near_clip, float(i + 1) / float(MAX_SHADOW_CASCADES));

    cascades_[i].split_depth = slice_far;

    /* Fit the cascade projection tightly by projecting the 8 frustum slice
     * corners into light space and computing their AABB. */
    float3 ls_min = float3(FLT_MAX);
    float3 ls_max = float3(-FLT_MAX);

    const float ndc_z[2] = {-1.0f, 1.0f}; /* OpenGL NDC convention */

    for (int z = 0; z < 2; z++) {
      for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
          float4 ndc_corner = float4(x * 2.0f - 1.0f,
                                     y * 2.0f - 1.0f,
                                     ndc_z[z],
                                     1.0f);
          float4 ws_corner = cam_viewproj_inv * ndc_corner;
          ws_corner /= ws_corner.w;

          const float3 cam_pos = inst_->camera.position();
          const float3 ray_dir = normalize(float3(ws_corner) - cam_pos);
          const float  t_near  = slice_near / dot(ray_dir,
                                                   -float3(inst_->uniform_data.viewmat[2]));
          const float  t_far   = slice_far  / dot(ray_dir,
                                                   -float3(inst_->uniform_data.viewmat[2]));

          const float3 ws_near = cam_pos + ray_dir * t_near;
          const float3 ws_far  = cam_pos + ray_dir * t_far;

          const float3 ls_near = float3(light_view * float4(ws_near, 1.0f));
          const float3 ls_far  = float3(light_view * float4(ws_far,  1.0f));

          ls_min = math::min(ls_min, math::min(ls_near, ls_far));
          ls_max = math::max(ls_max, math::max(ls_near, ls_far));
        }
      }
    }

    /* Small margin to avoid sub-texel shimmer at cascade boundaries */
    const float margin = 0.01f * (ls_max.x - ls_min.x);
    ls_min -= float3(margin);
    ls_max += float3(margin);

    const float4x4 light_proj = math::projection::orthographic(
        ls_min.x, ls_max.x,
        ls_min.y, ls_max.y,
        -ls_max.z, -ls_min.z);

    cascades_[i].view_proj = light_proj * light_view;
  }
}

void ShadowModule::end_sync()
{
  /* Route each shadow-casting light to the correct atlas.
   * Priority: LIGHT_FLAG_SHADOW_DYNAMIC → dynamic (if not full).
   * Fallback: static (if not full). If both full: no shadow. */
  const uint32_t dynamic_cap = uint32_t(dynamic_slots_);

  for (auto &[id, light] : inst_->lights.light_map_.items()) {

    if (light.data.type == uint32_t(LightType::SUN)) {
      /* Directional lights always use CSM in the static atlas. */
      update_cascades(inst_->camera, light);
      continue;
    }

    if (light.data.type != uint32_t(LightType::PUNCTUAL_SPOT) &&
        light.data.type != uint32_t(LightType::PUNCTUAL_POINT))
    {
      continue; /* Area lights: shadow not yet supported */
    }

    /* Build the light-space view-projection */
    const float spot_angle = light.data.spot_angle > 0.0f ?
                             light.data.spot_angle : DEG2RADF(90.0f);
    const float4x4 lv = math::invert(light.object_to_world);
    const float4x4 lp = math::projection::perspective(
        spot_angle, 1.0f, 0.01f, light.data.attenuation);

    ShadowPunctual slot;
    slot.view_proj    = lp * lv;
    slot.active       = true;
    slot.needs_update = true; /* TODO: dirty tracking — set false when static & clean */

    const bool want_dynamic = (light.data.flags & LIGHT_FLAG_SHADOW_DYNAMIC) != 0;

    if (want_dynamic && dynamic_punctual_.size() < dynamic_cap) {
      slot.is_dynamic  = true;
      slot.atlas_index = uint32_t(dynamic_punctual_.size());
      dynamic_punctual_.push_back(slot);
    }
    else if (static_punctual_.size() < STATIC_PUNCTUAL_SLOTS) {
      slot.is_dynamic  = false;
      slot.atlas_index = uint32_t(static_punctual_.size());
      static_punctual_.push_back(slot);
    }
    /* else: both atlases full — light casts no shadow this frame. No crash. */
  }
}

void ShadowModule::sync_object(Object *ob, ResourceHandleRange res_handle)
{
  (void)ob;
  (void)res_handle;
  /* TODO (dirty tracking): intersect ob's world AABB against each static
   * light's influence sphere. If they overlap, set needs_update = true on
   * that ShadowPunctual slot so render_static_atlas() re-renders it.
   * Until then, conservative path: all static slots refresh every frame
   * (needs_update is initialised to true in end_sync()). */
}

/* =========================================================================
 * set_view() — main entry point called from pipeline.cc
 * ========================================================================= */

void ShadowModule::set_view(View &view, int2 extent)
{
  GPU_debug_group_begin("Shadow");

  render_static_atlas(view);
  render_dynamic_atlas(view);

  GPU_debug_group_end();

  dispatch_pcf(extent);
}

/* =========================================================================
 * render_static_atlas()
 * ========================================================================= */

void ShadowModule::render_static_atlas(View &view)
{
  GPU_debug_group_begin("Shadow.Static");

  /* --- CSM cascades (always re-rendered; directional light moves with camera) --- */
  for (int i = 0; i < int(cascades_.size()); ++i) {
    const int vp_x = i * STATIC_TILE_SIZE;
    const int vp_y = 0;

    GPU_framebuffer_bind(&static_fb_);
    GPU_framebuffer_viewport_set(&static_fb_, vp_x, vp_y,
                                 STATIC_TILE_SIZE, STATIC_TILE_SIZE);
    GPU_framebuffer_clear_depth(&static_fb_, 1.0f);

    View cascade_view("shadow_cascade");
    cascade_view.sync(cascades_[i].view_proj, float4x4::identity());

    inst_->manager->submit(shadow_pass_ps_, cascade_view);
  }

  /* --- Static punctual lights (re-render only when dirty) --- */
  for (int i = 0; i < int(static_punctual_.size()); ++i) {
    ShadowPunctual &p = static_punctual_[i];
    if (!p.active || !p.needs_update) {
      continue; /* Clean — skip the draw calls, reuse cached depth. */
    }

    /* Tile layout: row 1..3 of the atlas, 4 columns.
     * atlas_index is flat [0..11]; col = idx % 4, row = 1 + idx / 4. */
    const int col  = int(p.atlas_index) % 4;
    const int row  = 1 + int(p.atlas_index) / 4;
    const int vp_x = col * STATIC_TILE_SIZE;
    const int vp_y = row * STATIC_TILE_SIZE;

    GPU_framebuffer_bind(&static_fb_);
    GPU_framebuffer_viewport_set(&static_fb_, vp_x, vp_y,
                                 STATIC_TILE_SIZE, STATIC_TILE_SIZE);
    GPU_framebuffer_clear_depth(&static_fb_, 1.0f);

    View punctual_view("shadow_static_punctual");
    punctual_view.sync(p.view_proj, float4x4::identity());

    inst_->manager->submit(shadow_pass_ps_, punctual_view);

    p.needs_update = false; /* Mark clean after render */
  }

  GPU_framebuffer_viewport_reset(&static_fb_);
  GPU_debug_group_end();
}

/* =========================================================================
 * render_dynamic_atlas()
 * ========================================================================= */

void ShadowModule::render_dynamic_atlas(View &view)
{
  if (!dynamic_atlas_tx_ || dynamic_punctual_.empty()) {
    return; /* OFF or no dynamic lights this frame */
  }

  GPU_debug_group_begin("Shadow.Dynamic");

  /* tiles_per_side = sqrt(N): 4→2, 16→4, 64→8.
   * atlas_index is flat [0..N-1]; col = idx % tps, row = idx / tps. */
  const int n              = int(dynamic_slots_);
  const int tiles_per_side = int(sqrtf(float(n)));

  for (int i = 0; i < int(dynamic_punctual_.size()); ++i) {
    const ShadowPunctual &p = dynamic_punctual_[i];
    if (!p.active) {
      continue;
    }

    const int col  = int(p.atlas_index) % tiles_per_side;
    const int row  = int(p.atlas_index) / tiles_per_side;
    const int vp_x = col * DYNAMIC_TILE_SIZE;
    const int vp_y = row * DYNAMIC_TILE_SIZE;

    GPU_framebuffer_bind(&dynamic_fb_);
    GPU_framebuffer_viewport_set(&dynamic_fb_, vp_x, vp_y,
                                 DYNAMIC_TILE_SIZE, DYNAMIC_TILE_SIZE);
    GPU_framebuffer_clear_depth(&dynamic_fb_, 1.0f);

    View punctual_view("shadow_dynamic_punctual");
    punctual_view.sync(p.view_proj, float4x4::identity());

    inst_->manager->submit(shadow_pass_ps_, punctual_view);
  }

  GPU_framebuffer_viewport_reset(&dynamic_fb_);
  GPU_debug_group_end();
}

/* =========================================================================
 * dispatch_pcf() — PCF 3x3 compute pass
 * ========================================================================= */

void ShadowModule::dispatch_pcf(int2 extent)
{
  GPU_debug_group_begin("Shadow.PCF");

  RenderBuffers &rbufs = inst_->render_buffers;

  /* Re-init each frame: cheap, avoids stale bindings on resolution change. */
  pcf_ps_.init();
  pcf_ps_.shader_set(inst_->shaders.static_shader_get(SH_SHADOW_PCF_FILTER));

  /* G-Buffer inputs */
  pcf_ps_.bind_texture("depth_tx",     &rbufs.depth_tx);
  pcf_ps_.bind_texture("normal_tx",    rbufs.rp_color_tx.layer_view(0));
  /* PCF reads from the static atlas (CSM cascade) — the primary shadow source.
   * Dynamic atlas is not sampled here; dynamic punctual shadows are a future pass. */
  pcf_ps_.bind_texture("shadow_atlas", static_atlas_tx_.get());

  /* Output: one shadow factor per pixel */
  pcf_ps_.bind_image("shadow_mask_img", &rbufs.shadow_mask_tx);

  /* Inverse VP for world-space position reconstruction — same as GTAO/SSGI/SSR. */
  const float4x4 vp_inv = math::invert(
      inst_->uniform_data.projectionmat * inst_->uniform_data.viewmat);
  pcf_ps_.push_constant("viewprojinv", vp_inv);

  /* Upload cascade matrices individually as flat push constants.
   * 4 matrices + 1 vec4 = 260 bytes — no UBO allocation overhead justified. */
  const char *cascade_names[MAX_SHADOW_CASCADES] = {
      "cascade_viewproj[0]",
      "cascade_viewproj[1]",
      "cascade_viewproj[2]",
      "cascade_viewproj[3]",
  };
  for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
    const float4x4 &m = (i < int(cascades_.size())) ?
                         cascades_[i].view_proj :
                         float4x4::identity();
    pcf_ps_.push_constant(cascade_names[i], m);
  }

  /* Pack cascade split depths into a vec4 for branchless cascade selection. */
  float4 splits = float4(0.0f);
  for (int i = 0; i < math::min(int(cascades_.size()), MAX_SHADOW_CASCADES); ++i) {
    splits[i] = cascades_[i].split_depth;
  }
  pcf_ps_.push_constant("cascade_splits",   splits);
  pcf_ps_.push_constant("shadow_bias",      0.005f);
  pcf_ps_.push_constant("pcf_offset_scale", data_.pcf_offset_scale);
  pcf_ps_.push_constant("shadow_map_res",   STATIC_ATLAS_RES);

  /* One thread per pixel, 8×8 tiles (one NVIDIA warp-pair / one AMD wavefront). */
  pcf_ps_.dispatch(math::divide_ceil(extent, int2(8)));

  /* GPU_BARRIER_TEXTURE_FETCH ensures imageStore writes in this compute shader
   * are visible to the subsequent texture() calls in the deferred lighting pass. */
  pcf_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  inst_->manager->submit(pcf_ps_);

  GPU_debug_group_end();
}

/* =========================================================================
 * bind_resources() — forward shading
 * ========================================================================= */

void ShadowModule::bind_resources(PassSimple &ps)
{
  /* Both atlases are bound so the forward pass can resolve static and
   * dynamic punctual shadows directly without going through shadow_mask_tx. */
  ps.bind_texture("shadow_static_atlas_tx",  static_atlas_tx_.get());
  if (dynamic_atlas_tx_) {
    ps.bind_texture("shadow_dynamic_atlas_tx", dynamic_atlas_tx_.get());
  }
}

} // namespace blender::eevee_game

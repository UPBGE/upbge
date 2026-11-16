/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_shadow.hh"
#include "eevee_game_instance.hh"

#include "BKE_object.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h" /* DEG2RADF */
#include "DNA_light_types.h"
#include "DNA_object_types.h"

#include <cfloat>
#include <cmath> /* sqrtf */

namespace blender::eevee_game {

ShadowModule::ShadowModule(GameInstance &inst, ShadowData &data) : inst_(&inst), data_(data) {}

/* =========================================================================
 * init()
 * ========================================================================= */

void ShadowModule::init(DynamicShadowSlots dynamic_slots)
{
  dynamic_slots_ = dynamic_slots;

  allocate_static_atlas();
  allocate_dynamic_atlas(); /* no-op when OFF */

  auto views_ensure = [](std::vector<std::unique_ptr<View>> &views, int len, const char *name) {
    views.clear();
    for (int i = 0; i < len; i++) {
      views.push_back(std::make_unique<View>(name));
    }
  };
  views_ensure(cascade_views_, MAX_SHADOW_CASCADES, "Shadow.Cascade");
  views_ensure(static_views_, STATIC_PUNCTUAL_SLOTS, "Shadow.StaticPunctual");
  views_ensure(dynamic_views_, int(dynamic_slots_), "Shadow.DynamicPunctual");
}

void ShadowModule::allocate_static_atlas()
{
  /* Fixed 4096x4096 atlas. Tiles 1024x1024.
   *   Row 0        : 4 CSM cascade tiles (directional light).
   *   Rows 1..3    : 4 columns x 3 rows = 12 punctual slots.
   *
   * D32F avoids shadow acne at large world scales. */
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;

  static_atlas_tx_.ensure_2d(
      gpu::TextureFormat::SFLOAT_32_DEPTH, int2(STATIC_ATLAS_RES), usage);

  /* Nearest + compare mode: PCF taps use sampler2DShadow hardware comparison. */
  GPU_texture_filter_mode(static_atlas_tx_, false);
  GPU_texture_compare_mode(static_atlas_tx_, true);

  static_fb_.ensure(GPU_ATTACHMENT_TEXTURE(static_atlas_tx_));
}

void ShadowModule::allocate_dynamic_atlas()
{
  const int n = int(dynamic_slots_);
  if (n == 0) {
    /* DynamicShadowSlots::OFF: nothing to allocate. */
    dynamic_atlas_tx_.free();
    return;
  }

  /* tiles_per_side = sqrt(n) is an integer for the allowed values
   * (4 -> 2, 16 -> 4, 64 -> 8). A square atlas keeps the shader UV math
   * symmetric: tile_uv = vec2(col, row) / tiles_per_side. */
  const int tiles_per_side = int(sqrtf(float(n)));
  const int atlas_res = tiles_per_side * DYNAMIC_TILE_SIZE;
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;

  dynamic_atlas_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH, int2(atlas_res), usage);

  GPU_texture_filter_mode(dynamic_atlas_tx_, false);
  GPU_texture_compare_mode(dynamic_atlas_tx_, true);

  dynamic_fb_.ensure(GPU_ATTACHMENT_TEXTURE(dynamic_atlas_tx_));
}

/* =========================================================================
 * Sync
 * ========================================================================= */

void ShadowModule::begin_sync()
{
  cascades_.clear();
  static_punctual_.clear();
  dynamic_punctual_.clear();
  caster_min_ = float3(FLT_MAX);
  caster_max_ = float3(-FLT_MAX);
}

void ShadowModule::update_cascades(Camera &camera, LightEntry &light)
{
  /* Logarithmic split distribution: more resolution near the camera where
   * angular density is highest.
   *   split_i = near * (far / near)^(i / N) */
  cascades_.resize(MAX_SHADOW_CASCADES);

  const float near_clip = inst_->uniform_data.z_near;
  const float far_clip = inst_->uniform_data.z_far;

  const float4x4 light_view = math::invert(light.object_to_world);
  const float4x4 cam_viewproj_inv = math::invert(inst_->uniform_data.viewprojmat);

  const float3 cam_pos = camera.position();
  /* The camera looks down its local -Z axis; viewinv holds the camera axes in
   * world space as columns. */
  const float3 cam_forward = -float3(inst_->uniform_data.viewinv[2]);

  /* Casters between the light and a slice are outside the slice bounds but
   * must not be clipped by the ortho near plane. Without depth clamp the
   * near plane is pulled toward the light up to the farthest synced caster
   * (the CPU gate already restricted the synced set to the casters that can
   * shadow the view). */
  float caster_ls_zmax = -FLT_MAX;
  if (caster_min_.x <= caster_max_.x) {
    for (int c = 0; c < 8; c++) {
      const float3 corner((c & 1) ? caster_max_.x : caster_min_.x,
                          (c & 2) ? caster_max_.y : caster_min_.y,
                          (c & 4) ? caster_max_.z : caster_min_.z);
      caster_ls_zmax = math::max(caster_ls_zmax, math::transform_point(light_view, corner).z);
    }
  }

  for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
    const float slice_near = (i == 0) ? near_clip :
                                        near_clip * powf(far_clip / near_clip,
                                                         float(i) / float(MAX_SHADOW_CASCADES));
    const float slice_far = near_clip *
                            powf(far_clip / near_clip, float(i + 1) / float(MAX_SHADOW_CASCADES));

    cascades_[i].split_depth = slice_far;

    /* Fit the cascade projection by taking the 4 corner rays of the camera
     * frustum, cutting each at the slice near / far distance, and bounding
     * the 8 resulting points in light space. */
    float3 ls_min = float3(FLT_MAX);
    float3 ls_max = float3(-FLT_MAX);

    for (int y = 0; y < 2; y++) {
      for (int x = 0; x < 2; x++) {
        /* A point on the far plane gives the ray direction through this corner. */
        float4 ws_corner = cam_viewproj_inv * float4(x * 2.0f - 1.0f, y * 2.0f - 1.0f, 1.0f, 1.0f);
        ws_corner /= ws_corner.w;

        const float3 ray_dir = math::normalize(float3(ws_corner) - cam_pos);
        /* Slice distances are along the view axis, the ray is slanted. */
        const float cos_theta = math::dot(ray_dir, cam_forward);
        const float3 ws_near = cam_pos + ray_dir * (slice_near / cos_theta);
        const float3 ws_far = cam_pos + ray_dir * (slice_far / cos_theta);

        const float3 ls_near = math::transform_point(light_view, ws_near);
        const float3 ls_far = math::transform_point(light_view, ws_far);

        ls_min = math::min(ls_min, math::min(ls_near, ls_far));
        ls_max = math::max(ls_max, math::max(ls_near, ls_far));
      }
    }

    /* Small margin to avoid sub-texel shimmer at cascade boundaries. */
    const float margin = 0.01f * (ls_max.x - ls_min.x);
    ls_min -= float3(margin);
    ls_max += float3(margin);
    /* +Z points toward the light in light space. */
    ls_max.z = math::max(ls_max.z, caster_ls_zmax + margin);

    /* The light looks down its local -Z, so depth range is [-max.z, -min.z]. */
    cascades_[i].view = light_view;
    cascades_[i].proj = math::projection::orthographic(
        ls_min.x, ls_max.x, ls_min.y, ls_max.y, -ls_max.z, -ls_min.z);
    cascades_[i].view_proj = cascades_[i].proj * cascades_[i].view;

    data_.cascade_viewproj[i] = cascades_[i].view_proj;
    data_.cascade_splits[i] = slice_far;
  }
}

void ShadowModule::end_sync()
{
  /* Route each shadow-casting light to the correct atlas.
   * Priority: LIGHT_FLAG_SHADOW_DYNAMIC -> dynamic (if not full).
   * Fallback: static (if not full). If both full: no shadow. */
  const size_t dynamic_cap = size_t(dynamic_slots_);
  bool has_sun = false;

  for (LightEntry &light : inst_->lights.entries_) {
    if (light.data.shadow_index < 0) {
      continue; /* Shadow casting disabled on this light. */
    }

    if (light.data.type == uint32_t(LightType::SUN)) {
      /* One set of cascades: the first sun wins, the others stay unshadowed. */
      if (!has_sun) {
        update_cascades(inst_->camera, light);
        has_sun = true;
      }
      continue;
    }

    if (light.data.type != uint32_t(LightType::PUNCTUAL_SPOT) &&
        light.data.type != uint32_t(LightType::PUNCTUAL_POINT))
    {
      continue; /* Area lights: shadow not yet supported */
    }

    /* Light-space view and projection. A point light gets a single 90 degree
     * frustum down its -Z axis for now, not a full cube. */
    const float spot_angle = (light.data.spot_angle > 0.0f) ? light.data.spot_angle :
                                                              DEG2RADF(90.0f);
    const float half_angle = spot_angle * 0.5f;
    const float far_dist = math::max(light.data.attenuation, 0.02f);

    ShadowPunctual slot;
    slot.view = math::invert(light.object_to_world);
    slot.proj = math::projection::perspective_fov(
        -half_angle, half_angle, -half_angle, half_angle, 0.01f, far_dist);
    slot.view_proj = slot.proj * slot.view;
    slot.active = true;
    slot.needs_update = true; /* TODO: dirty tracking, false when static and clean. */

    const bool want_dynamic = (light.data.flags & LIGHT_FLAG_SHADOW_DYNAMIC) != 0;

    if (want_dynamic && dynamic_punctual_.size() < dynamic_cap) {
      slot.is_dynamic = true;
      slot.atlas_index = uint32_t(dynamic_punctual_.size());
      dynamic_punctual_.push_back(slot);
    }
    else if (static_punctual_.size() < STATIC_PUNCTUAL_SLOTS) {
      slot.is_dynamic = false;
      slot.atlas_index = uint32_t(static_punctual_.size());
      static_punctual_.push_back(slot);
    }
    /* else: both atlases full, the light casts no shadow this frame. */
  }
}

void ShadowModule::sync_object(Object *ob, ResourceHandleRange res_handle)
{
  (void)res_handle;
  if (ob->visibility_flag & OB_HIDE_SHADOW) {
    return;
  }
  const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_eval_cached_get(ob);
  if (!bounds) {
    return;
  }
  /* World AABB of the local box in center / half size form: the half size
   * along a world axis is |row| . local half size (Arvo), exact for a box. */
  const float4x4 &m = ob->object_to_world();
  const float3 lc = (bounds->min + bounds->max) * 0.5f;
  const float3 le = (bounds->max - bounds->min) * 0.5f;
  const float3 wc = math::transform_point(m, lc);
  const float3 we = math::abs(m.x_axis()) * le.x + math::abs(m.y_axis()) * le.y +
                    math::abs(m.z_axis()) * le.z;
  caster_min_ = math::min(caster_min_, wc - we);
  caster_max_ = math::max(caster_max_, wc + we);
  /* TODO (dirty tracking): intersect ob's world AABB against each static
   * light's influence sphere. If they overlap, set needs_update = true on
   * that ShadowPunctual slot so render_static_atlas() re-renders it.
   * Until then, conservative path: all static slots refresh every frame
   * (needs_update is initialised to true in end_sync()). */
}

/* =========================================================================
 * set_view(): main entry point called from pipeline.cc
 * ========================================================================= */

void ShadowModule::set_view(View & /*view*/, int2 extent)
{
  GPU_debug_group_begin("Shadow");
  render_static_atlas();
  render_dynamic_atlas();
  GPU_debug_group_end();

  dispatch_pcf(extent);
}

void ShadowModule::render_tile(Framebuffer &fb, View &view, int2 tile_origin, int tile_size)
{
  GPU_framebuffer_viewport_set(fb, tile_origin.x, tile_origin.y, tile_size, tile_size);

  /* Shadow casters are the opaque depth prepass drawn from the light. Same
   * draws, same material depth shaders (alpha clip included), and the
   * Manager culls them against this view's own frustum. No second geometry
   * pass has to be built or kept in sync with the first.
   *
   * Consequence: the atlas stores what those shaders write, reversed depth
   * (1 = near the light, 0 = far), tested with GREATER_EQUAL. */
  inst_->manager->submit(inst_->pipelines.prepass_opaque_get(), view);
}

/* =========================================================================
 * render_static_atlas()
 * ========================================================================= */

void ShadowModule::render_static_atlas()
{
  if (cascades_.empty() && static_punctual_.empty()) {
    return;
  }

  GPU_debug_group_begin("Shadow.Static");

  /* A depth clear ignores the viewport, so it cannot be done per tile: it
   * would wipe the tiles already rendered. Clear the whole atlas once. This
   * is also why every static slot is re-rendered below; skipping clean slots
   * needs a scissored clear first. */
  static_fb_.bind();
  GPU_framebuffer_clear_depth(static_fb_, 0.0f);

  /* CSM cascades: row 0. Re-rendered every frame, they follow the camera. */
  for (int i = 0; i < int(cascades_.size()); ++i) {
    View &view = *cascade_views_[i];
    view.sync(cascades_[i].view, cascades_[i].proj);
    render_tile(static_fb_, view, int2(i * STATIC_TILE_SIZE, 0), STATIC_TILE_SIZE);
  }

  /* Static punctual lights: rows 1..3, 4 columns.
   * atlas_index is flat [0..11]; col = idx % 4, row = 1 + idx / 4. */
  for (ShadowPunctual &p : static_punctual_) {
    if (!p.active) {
      continue;
    }
    const int col = int(p.atlas_index) % 4;
    const int row = 1 + int(p.atlas_index) / 4;

    View &view = *static_views_[p.atlas_index];
    view.sync(p.view, p.proj);
    render_tile(static_fb_, view, int2(col, row) * STATIC_TILE_SIZE, STATIC_TILE_SIZE);

    p.needs_update = false;
  }

  GPU_framebuffer_viewport_reset(static_fb_);
  GPU_debug_group_end();
}

/* =========================================================================
 * render_dynamic_atlas()
 * ========================================================================= */

void ShadowModule::render_dynamic_atlas()
{
  if (!dynamic_atlas_tx_.is_valid() || dynamic_punctual_.empty()) {
    return; /* OFF or no dynamic lights this frame */
  }

  GPU_debug_group_begin("Shadow.Dynamic");

  /* tiles_per_side = sqrt(N): 4 -> 2, 16 -> 4, 64 -> 8.
   * atlas_index is flat [0..N-1]; col = idx % tps, row = idx / tps. */
  const int tiles_per_side = int(sqrtf(float(int(dynamic_slots_))));

  dynamic_fb_.bind();
  GPU_framebuffer_clear_depth(dynamic_fb_, 0.0f);

  for (const ShadowPunctual &p : dynamic_punctual_) {
    if (!p.active) {
      continue;
    }
    const int col = int(p.atlas_index) % tiles_per_side;
    const int row = int(p.atlas_index) / tiles_per_side;

    View &view = *dynamic_views_[p.atlas_index];
    view.sync(p.view, p.proj);
    render_tile(dynamic_fb_, view, int2(col, row) * DYNAMIC_TILE_SIZE, DYNAMIC_TILE_SIZE);
  }

  GPU_framebuffer_viewport_reset(dynamic_fb_);
  GPU_debug_group_end();
}

/* =========================================================================
 * dispatch_pcf(): PCF 3x3 compute pass
 * ========================================================================= */

void ShadowModule::dispatch_pcf(int2 extent)
{
  RenderBuffers &rbufs = inst_->render_buffers;

  pcf_ps_.init();
  pcf_ps_.shader_set(inst_->shaders.static_shader_get(SH_SHADOW_PCF_FILTER));

  /* Depth, and shading normals decoded from the G-Buffer. */
  pcf_ps_.bind_texture("depth_tx", &rbufs.depth_tx);
  rbufs.gbuffer.bind_resources(pcf_ps_);

  /* PCF reads from the static atlas (CSM cascades), the primary shadow source.
   * Dynamic atlas is not sampled here; dynamic punctual shadows are a future pass. */
  pcf_ps_.bind_texture("shadow_atlas", static_atlas_tx_);

  /* Output: one shadow factor per pixel */
  pcf_ps_.bind_image("shadow_mask_img", &rbufs.shadow_mask_tx);

  /* Inverse VP for world-space position reconstruction. */
  pcf_ps_.push_constant("viewprojinv", math::invert(inst_->uniform_data.viewprojmat));

  /* One named constant per cascade: the pass API has no matrix array
   * overload. Unused cascades are identity with a zero split, so the shader
   * never selects them. */
  const char *cascade_names[MAX_SHADOW_CASCADES] = {
      "cascade_viewproj_0", "cascade_viewproj_1", "cascade_viewproj_2", "cascade_viewproj_3"};
  float4 splits = float4(0.0f);
  for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
    const bool used = i < int(cascades_.size());
    pcf_ps_.push_constant(cascade_names[i],
                          used ? cascades_[i].view_proj : float4x4::identity());
    splits[i] = used ? cascades_[i].split_depth : 0.0f;
  }
  pcf_ps_.push_constant("cascade_splits", splits);
  pcf_ps_.push_constant("shadow_bias", 0.005f);
  pcf_ps_.push_constant("pcf_offset_scale", data_.pcf_offset_scale);
  pcf_ps_.push_constant("shadow_map_res", int(STATIC_ATLAS_RES));

  /* One thread per pixel, 8x8 groups. */
  pcf_ps_.dispatch(math::divide_ceil(extent, int2(8)));

  /* The deferred lighting pass samples the mask right after. */
  pcf_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  inst_->manager->submit(pcf_ps_);
}

/* =========================================================================
 * bind_resources(): forward shading
 * ========================================================================= */

void ShadowModule::bind_resources(PassSimple &ps)
{
  /* Both atlases are bound so the forward pass can resolve static and
   * dynamic punctual shadows directly without going through shadow_mask_tx. */
  ps.bind_texture("shadow_static_atlas_tx", static_atlas_tx_);
  if (dynamic_atlas_tx_.is_valid()) {
    ps.bind_texture("shadow_dynamic_atlas_tx", dynamic_atlas_tx_);
  }
}

}  // namespace blender::eevee_game

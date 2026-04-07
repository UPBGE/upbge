/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_shadow.hh"
#include "eevee_game_instance.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_geom.h"
#include "DNA_object_types.h"
#include "DNA_light_types.h"

namespace blender::eevee_game {

ShadowModule::ShadowModule(GameInstance &inst, ShadowData &data)
    : inst_(&inst), data_(data)
{
}

void ShadowModule::init()
{
  allocate_atlas();

  /* Shadow pass: renders scene geometry from the light's POV into the atlas.
   * Uses the same G-Buffer shader in depth-only mode (no color writes). */
  shadow_pass_ps_.init();
  shadow_pass_ps_.shader_set(inst_->shaders.static_shader_get(SH_SHADOW_DIRECTIONAL_CSM));

  /* Framebuffer: depth-only attachment onto a viewport-sized region of the atlas */
  GPU_framebuffer_ensure_config(
      &shadow_fb_,
      {
          GPU_ATTACHMENT_TEXTURE(atlas_tx_.get()),
      });
}

void ShadowModule::allocate_atlas()
{
  /* Fixed 4096x4096 shadow atlas eliminates the virtual page defragmentation
   * spikes that make EEVEE-Next's virtual shadow maps unsuitable for real-time games.
   * Layout: top half = CSM cascades (4 tiles of 2048x1024), bottom half = punctual lights.
   *
   * SFLOAT_32_DEPTH avoids precision artifacts (shadow acne) at large world scales. */
  const int2 atlas_res = int2(SHADOW_ATLAS_RES, SHADOW_ATLAS_RES);
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT |
                                 GPU_TEXTURE_USAGE_SHADER_READ;

  atlas_tx_ = std::make_unique<gpu::Texture>("shadow_atlas");
  atlas_tx_->ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH, atlas_res, usage);

  /* Nearest filtering: all PCF/PCSS kernel logic lives in the shader.
   * Linear filtering on a depth texture gives incorrect shadow comparisons. */
  GPU_texture_filter_mode(atlas_tx_->get(), false);
  GPU_texture_compare_mode(atlas_tx_->get(), false);
}

void ShadowModule::begin_sync()
{
  punctual_lights_.clear();
}

void ShadowModule::update_cascades(Camera &camera, LightEntry &light)
{
  /* Logarithmic split distribution: more cascade resolution near the camera
   * where angular density is highest, less at distance where the eye can't resolve it.
   * split_i = near * (far/near)^(i/N)
   * This is the standard technique used by virtually every AAA title since 2007. */
  cascades_.resize(MAX_SHADOW_CASCADES);

  const float near_clip = camera.data_get().clip_near;
  const float far_clip  = camera.data_get().clip_far;

  /* Light-space view matrix: inverse of the sun light's world transform */
  const float4x4 light_view = math::invert(light.object_to_world);

  /* Camera frustum corners in world space, used to fit the cascade projection tightly.
   * We compute them from the inverse view-projection of the NDC cube corners. */
  const float4x4 cam_viewproj_inv = math::invert(
      inst_->uniform_data.projectionmat * inst_->uniform_data.viewmat);

  for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
    /* Slice boundaries in camera space */
    const float slice_near = (i == 0) ?
        near_clip :
        near_clip * powf(far_clip / near_clip, float(i) / float(MAX_SHADOW_CASCADES));
    const float slice_far = near_clip *
        powf(far_clip / near_clip, float(i + 1) / float(MAX_SHADOW_CASCADES));

    cascades_[i].split_depth = slice_far;

    /* Project the 8 corners of this frustum slice into light space and compute
     * the tight AABB. This minimises wasted shadow map texels. */
    float3 ls_min = float3(FLT_MAX);
    float3 ls_max = float3(-FLT_MAX);

    /* NDC Z range: -1..+1 for OpenGL, 0..1 for Vulkan.
     * We use the OpenGL convention here to match Blender's default backend. */
    const float ndc_z[2] = {-1.0f, 1.0f};

    for (int z = 0; z < 2; z++) {
      for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
          /* NDC corner */
          float4 ndc_corner = float4(x * 2.0f - 1.0f,
                                     y * 2.0f - 1.0f,
                                     ndc_z[z],
                                     1.0f);

          /* Unproject to world space */
          float4 ws_corner = cam_viewproj_inv * ndc_corner;
          ws_corner /= ws_corner.w;

          /* Clamp the world-space corner to the slice depth range.
           * We linearly interpolate along the ray from the camera origin. */
          const float3 cam_pos = inst_->camera.position();
          const float3 ray_dir = normalize(float3(ws_corner) - cam_pos);
          const float  t_near  = slice_near / dot(ray_dir,
                                                   -float3(inst_->uniform_data.viewmat[2]));
          const float  t_far   = slice_far  / dot(ray_dir,
                                                   -float3(inst_->uniform_data.viewmat[2]));

          const float3 ws_near = cam_pos + ray_dir * t_near;
          const float3 ws_far  = cam_pos + ray_dir * t_far;

          /* Transform world-space corners into light space */
          const float3 ls_near = float3(light_view * float4(ws_near, 1.0f));
          const float3 ls_far  = float3(light_view * float4(ws_far,  1.0f));

          ls_min = math::min(ls_min, math::min(ls_near, ls_far));
          ls_max = math::max(ls_max, math::max(ls_near, ls_far));
        }
      }
    }

    /* Add a small margin to avoid sub-texel shimmer at cascade boundaries */
    const float margin = 0.01f * (ls_max.x - ls_min.x);
    ls_min -= float3(margin);
    ls_max += float3(margin);

    /* Build a tight orthographic projection from the AABB */
    const float4x4 light_proj = math::projection::orthographic(
        ls_min.x, ls_max.x,
        ls_min.y, ls_max.y,
        -ls_max.z, -ls_min.z); /* Z is negated: light looks along -Z */

    cascades_[i].view_proj = light_proj * light_view;
  }
}

void ShadowModule::end_sync()
{
  /* Assign shadow atlas resources to all shadow-casting lights.
   * Directional lights get CSM cascades; spot/point lights get fixed atlas slots. */
  for (auto &[id, light] : inst_->lights.light_map_.items()) {
    if (light.data.type == uint32_t(LightType::SUN)) {
      update_cascades(inst_->camera, light);
    }
    else if (light.data.type == uint32_t(LightType::PUNCTUAL_SPOT) ||
             light.data.type == uint32_t(LightType::PUNCTUAL_POINT))
    {
      if (int(punctual_lights_.size()) >= MAX_PUNCTUAL_SHADOW_SLOTS) {
        continue; /* Atlas is full; skip lights beyond the budget */
      }

      ShadowPunctual p;
      /* For spot lights: standard perspective projection.
       * For point lights: would need a cube-map; currently uses a single face (simplified). */
      const float spot_angle = light.data.spot_angle > 0.0f ? light.data.spot_angle :
                                                               DEG2RADF(90.0f);
      const float4x4 lv = math::invert(light.object_to_world);
      const float4x4 lp = math::projection::perspective(
          spot_angle, 1.0f, 0.01f, light.data.attenuation);

      p.view_proj   = lp * lv;
      p.atlas_index = uint32_t(punctual_lights_.size());
      p.active      = true;
      punctual_lights_.push_back(p);
    }
  }
}

void ShadowModule::set_view(View &view, int2 extent)
{
  (void)view; /* Shadow passes use their own per-light views, not the camera view */

  GPU_debug_group_begin("Shadow Atlas");

  /* The atlas is divided into equal-sized tiles.
   * CSM: 4 cascades each get a 1024x1024 tile in the top row.
   * Punctual: remaining tiles below. */
  const int cascade_tile_size   = SHADOW_ATLAS_RES / MAX_SHADOW_CASCADES;
  const int punctual_tile_size  = SHADOW_ATLAS_RES / 4; /* 4 columns */

  /* --- Directional Light Cascades --- */
  for (int i = 0; i < int(cascades_.size()); ++i) {
    /* Viewport into the atlas: each cascade occupies one horizontal tile */
    const int vp_x = i * cascade_tile_size;
    const int vp_y = 0;

    GPU_framebuffer_bind(&shadow_fb_);
    GPU_framebuffer_viewport_set(&shadow_fb_, vp_x, vp_y,
                                 cascade_tile_size, cascade_tile_size);
    GPU_framebuffer_clear_depth(&shadow_fb_, 1.0f);

    /* Build a View from the cascade viewproj matrix */
    View cascade_view("shadow_cascade");
    cascade_view.sync(cascades_[i].view_proj, float4x4::identity());

    /* Submit all opaque geometry from the cascade view */
    shadow_pass_ps_.push_constant("cascade_index", i);
    inst_->manager->submit(shadow_pass_ps_, cascade_view);
  }

  /* --- Punctual Lights (Spot / Point) --- */
  for (int i = 0; i < int(punctual_lights_.size()); ++i) {
    const ShadowPunctual &p = punctual_lights_[i];
    if (!p.active) {
      continue;
    }

    /* Tile layout: row 1+ of the atlas, 4 columns */
    const int col   = i % 4;
    const int row   = 1 + (i / 4);
    const int vp_x  = col * punctual_tile_size;
    const int vp_y  = row * punctual_tile_size;

    GPU_framebuffer_bind(&shadow_fb_);
    GPU_framebuffer_viewport_set(&shadow_fb_, vp_x, vp_y,
                                 punctual_tile_size, punctual_tile_size);
    GPU_framebuffer_clear_depth(&shadow_fb_, 1.0f);

    View punctual_view("shadow_punctual");
    punctual_view.sync(p.view_proj, float4x4::identity());

    shadow_pass_ps_.push_constant("cascade_index", -1); /* Signals punctual path in shader */
    inst_->manager->submit(shadow_pass_ps_, punctual_view);
  }

  /* Restore the full-atlas viewport so subsequent passes are not clipped */
  GPU_framebuffer_viewport_reset(&shadow_fb_);

  GPU_debug_group_end();

  /* --- PCF 3x3 Compute Pass ---
   * The atlas is now fully populated. Dispatch the compute shader that reads
   * G-Buffer depth + normal and writes shadow_mask_tx.
   * This must happen AFTER the atlas rasterisation and BEFORE the lighting pass. */
  dispatch_pcf(extent);
}

void ShadowModule::dispatch_pcf(int2 extent)
{
  GPU_debug_group_begin("Shadow.PCF");

  RenderBuffers &rbufs = inst_->render_buffers;

  /* Re-init each frame: texture pointers are stable but the pass object is cheap
   * to rebuild and this avoids stale bindings if resolution changes. */
  pcf_ps_.init();
  pcf_ps_.shader_set(inst_->shaders.static_shader_get(SH_SHADOW_PCF_FILTER));

  /* Inputs: G-Buffer depth and normal (layer 0 of rp_color_tx = world normals) */
  pcf_ps_.bind_texture("depth_tx",    &rbufs.depth_tx);
  pcf_ps_.bind_texture("normal_tx",   rbufs.rp_color_tx.layer_view(0));
  pcf_ps_.bind_texture("shadow_atlas", atlas_tx_.get());

  /* Output: shadow mask at render resolution */
  pcf_ps_.bind_image("shadow_mask_img", &rbufs.shadow_mask_tx);

  /* Per-frame camera inverse VP for world-space position reconstruction.
   * Same matrix that GTAO, SSGI, and SSR use — already computed by uniform_data. */
  const float4x4 vp_inv = math::invert(
      inst_->uniform_data.projectionmat * inst_->uniform_data.viewmat);
  pcf_ps_.push_constant("viewprojinv", vp_inv);

  /* Upload cascade matrices individually — the shader declares them as
   * separate uniforms (cascade_viewproj[0..3]) not as an array UBO,
   * keeping the binding layout flat and avoiding a UBO allocation. */
  for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
    const float4x4 &m = (i < int(cascades_.size())) ?
                         cascades_[i].view_proj :
                         float4x4::identity();
    /* Push constant names must match the shader declarations exactly. */
    const char *names[MAX_SHADOW_CASCADES] = {
        "cascade_viewproj[0]",
        "cascade_viewproj[1]",
        "cascade_viewproj[2]",
        "cascade_viewproj[3]",
    };
    pcf_ps_.push_constant(names[i], m);
  }

  /* Pack cascade split depths into a vec4 (one float per cascade).
   * The shader uses these to select the correct cascade tile per pixel. */
  float4 splits = float4(0.0f);
  for (int i = 0; i < math::min(int(cascades_.size()), MAX_SHADOW_CASCADES); ++i) {
    splits[i] = cascades_[i].split_depth;
  }
  pcf_ps_.push_constant("cascade_splits",  splits);
  pcf_ps_.push_constant("shadow_bias",     0.005f);        /* 5 mm world-space normal bias */
  pcf_ps_.push_constant("pcf_offset_scale", data_.pcf_offset_scale);
  pcf_ps_.push_constant("shadow_map_res",  SHADOW_ATLAS_RES);

  /* Dispatch: one thread per pixel, 8x8 tiles.
   * divide_ceil ensures coverage when extent is not a multiple of 8. */
  pcf_ps_.dispatch(math::divide_ceil(extent, int2(8)));

  /* Barrier: the lighting pass reads shadow_mask_tx as a sampler2D.
   * GPU_BARRIER_TEXTURE_FETCH ensures the imageStore writes are visible
   * to subsequent texture() calls without explicit sync on the CPU side. */
  pcf_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  inst_->manager->submit(pcf_ps_);

  GPU_debug_group_end();
}

void ShadowModule::sync_object(Object *ob, ResourceHandleRange res_handle)
{
  /* In eevee_game all opaque mesh objects cast shadows.
   * We store the resource handle index so set_view() can submit those objects
   * into the shadow atlas draw calls.
   *
   * For now we record a flag that this resource ID is a shadow caster.
   * set_view() iterates inst_->manager's resource list filtered by this flag.
   *
   * Future: respect ob->cycles.is_shadow_catcher, shadow distance culling, etc. */
  (void)ob;
  (void)res_handle;
  /* The simple shadow system in set_view() re-submits the entire scene from each
   * light's viewpoint using shadow_pass_ps_, which already draws all objects.
   * Per-object registration is therefore not needed until we implement
   * fine-grained shadow caster culling. */
}

void ShadowModule::bind_resources(PassSimple &ps)
{
  ps.bind_texture("shadow_atlas_tx", atlas_tx_.get());
}

} // namespace blender::eevee_game

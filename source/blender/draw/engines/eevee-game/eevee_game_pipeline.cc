/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_pipeline.hh"
#include "eevee_game_instance.hh"

#include "BLI_smaa_textures.h"
#include "BLI_string.h"

#include "DNA_world_types.h"

namespace blender::eevee_game {

/* ================================================================
 * AAModule
 * ================================================================ */

void AAModule::init()
{
  /* Both tables are constant for every scene and resolution. Bilinear
   * filtering is part of how the SMAA weight pass reads them. */
  smaa_search_lut_tx_.ensure_2d(gpu::TextureFormat::UNORM_8,
                                int2(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT),
                                GPU_TEXTURE_USAGE_SHADER_READ);
  GPU_texture_update(smaa_search_lut_tx_, GPU_DATA_UBYTE, searchTexBytes);
  GPU_texture_filter_mode(smaa_search_lut_tx_, true);

  smaa_area_lut_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8,
                              int2(AREATEX_WIDTH, AREATEX_HEIGHT),
                              GPU_TEXTURE_USAGE_SHADER_READ);
  GPU_texture_update(smaa_area_lut_tx_, GPU_DATA_UBYTE, areaTexBytes);
  GPU_texture_filter_mode(smaa_area_lut_tx_, true);
}

void AAModule::sync_framebuffers(int2 extent)
{
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;

  smaa_edge_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8, extent, usage);
  smaa_weight_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, extent, usage);
  /* The weight and blend stages sample between texels on purpose. */
  GPU_texture_filter_mode(smaa_edge_tx_, true);
  GPU_texture_filter_mode(smaa_weight_tx_, true);

  smaa_edge_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(smaa_edge_tx_));
  smaa_weight_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(smaa_weight_tx_));
}

void AAModule::apply_fxaa(gpu::Texture *src, gpu::Texture *dst)
{
  /* FXAA 3.11: single full-screen pass that blends along edges found from
   * local luma contrast.
   *   subpix 0.75             : amount of sub-pixel aliasing removal.
   *   edge_threshold 0.166    : minimum local contrast to treat as an edge.
   *   edge_threshold_min 1/12 : skips dark areas where edges are invisible. */
  fxaa_ps_.init();
  fxaa_ps_.state_set(DRW_STATE_WRITE_COLOR);
  fxaa_ps_.shader_set(inst_->shaders.static_shader_get(SH_FXAA));
  fxaa_ps_.bind_texture("color_tx", src);
  fxaa_ps_.push_constant("fxaa_quality_subpix", 0.75f);
  fxaa_ps_.push_constant("fxaa_quality_edge_threshold", 0.166f);
  fxaa_ps_.push_constant("fxaa_quality_edge_threshold_min", 0.0833f);
  fxaa_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  output_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(dst));
  output_fb_.bind();
  inst_->manager->submit(fxaa_ps_);
}

void AAModule::apply_smaa(gpu::Texture *src, gpu::Texture *dst)
{
  /* SMAA (Subpixel Morphological Anti-Aliasing), 3 raster passes:
   *   1. Edge detection: 2 channel edge mask from luma.
   *   2. Blending weights: pattern search along the edges, area LUT lookup.
   *   3. Neighborhood blending: blend each pixel with its neighbours.
   * Reference: Jimenez et al., "SMAA: Enhanced Subpixel Morphological
   * Antialiasing", Eurographics 2012. */
  const int2 extent = texture_extent(src);
  const float4 rt_metrics = float4(1.0f / float2(extent), float2(extent));

  /* ---- Pass 1: Edge Detection ---- */
  smaa_edge_ps_.init();
  smaa_edge_ps_.state_set(DRW_STATE_WRITE_COLOR);
  smaa_edge_ps_.shader_set(inst_->shaders.static_shader_get(SH_SMAA_EDGE));
  smaa_edge_ps_.bind_texture("color_tx", src);
  smaa_edge_ps_.push_constant("smaa_rt_metrics", rt_metrics);
  /* Edge pixels are sparse and the shader discards the rest. */
  smaa_edge_ps_.clear_color(float4(0.0f));
  smaa_edge_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  smaa_edge_fb_.bind();
  inst_->manager->submit(smaa_edge_ps_);

  /* ---- Pass 2: Blending Weights ---- */
  smaa_weight_ps_.init();
  smaa_weight_ps_.state_set(DRW_STATE_WRITE_COLOR);
  smaa_weight_ps_.shader_set(inst_->shaders.static_shader_get(SH_SMAA_WEIGHT));
  smaa_weight_ps_.bind_texture("edges_tx", smaa_edge_tx_);
  smaa_weight_ps_.bind_texture("area_tx", smaa_area_lut_tx_);
  smaa_weight_ps_.bind_texture("search_tx", smaa_search_lut_tx_);
  smaa_weight_ps_.push_constant("smaa_rt_metrics", rt_metrics);
  smaa_weight_ps_.clear_color(float4(0.0f));
  smaa_weight_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  smaa_weight_fb_.bind();
  inst_->manager->submit(smaa_weight_ps_);

  /* ---- Pass 3: Neighborhood Blending ---- */
  smaa_blend_ps_.init();
  smaa_blend_ps_.state_set(DRW_STATE_WRITE_COLOR);
  smaa_blend_ps_.shader_set(inst_->shaders.static_shader_get(SH_SMAA_BLEND));
  smaa_blend_ps_.bind_texture("color_tx", src);
  smaa_blend_ps_.bind_texture("blend_tx", smaa_weight_tx_);
  smaa_blend_ps_.push_constant("smaa_rt_metrics", rt_metrics);
  smaa_blend_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  output_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(dst));
  output_fb_.bind();
  inst_->manager->submit(smaa_blend_ps_);
}

gpu::Texture *AAModule::apply_aa(gpu::Texture *src, gpu::Texture *dst, const AASettings &settings)
{
  if (!settings.enabled) {
    return src;
  }
  switch (settings.mode) {
    case AAMode::FXAA:
      apply_fxaa(src, dst);
      return dst;
    case AAMode::SMAA:
      apply_smaa(src, dst);
      return dst;
    default:
      return src;
  }
}

/* ================================================================
 * PipelineModule
 * ================================================================ */

PipelineModule::PipelineModule(ShaderModule *shaders, GameInstance *inst)
    : aa(inst), inst_(inst), shaders_(shaders)
{
}

void PipelineModule::begin_sync()
{
  RenderBuffers &rbufs = inst_->render_buffers;

  closure_bits_ = eevee::CLOSURE_NONE;

  /* Geometry passes. Materials add one sub-pass per shader during object
   * sync (material_add()), so the parents must be reset before that and
   * never after: init() drops every sub-pass and draw call recorded so far.
   *
   * Resources bound on the parent are inherited by every sub-pass. What is
   * bound here mirrors eevee::DeferredLayer::begin_sync() and
   * gbuffer_pass_sync(): it is the fixed part of the interface of EEVEE's
   * generated surface shaders. Everything is bound by reference, the
   * contents are filled in sync(). */
  {
    /* `eevee_surf_depth` (+ `eevee_velocity_geom` for moving objects). */
    PassMain &pass = opaque_layer_.prepass_ps;
    pass.init();
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, eevee_utility_tx_);
    pass.bind_ubo(UNIFORM_BUF_SLOT, &eevee_uniform_buf_);
    pass.bind_ssbo(SAMPLING_BUF_SLOT, &eevee_sampling_buf_);
    inst_->velocity.bind_resources(pass);
    pass.bind_ubo(VELOCITY_CAMERA_PREV_BUF, &camera_prev_buf_);
    pass.bind_ubo(VELOCITY_CAMERA_CURR_BUF, &camera_curr_buf_);
    /* No "next" step without motion blur. */
    pass.bind_ubo(VELOCITY_CAMERA_NEXT_BUF, &camera_curr_buf_);
  }
  {
    /* `eevee_surf_deferred` + render pass and cryptomatte outputs. */
    PassMain &pass = opaque_layer_.gbuffer_ps;
    pass.init();
    /* G-Buffer layers beyond the framebuffer attachments are written with
     * imageStore. */
    rbufs.gbuffer.bind_optional_layers(pass);
    pass.bind_image(RBUFS_COLOR_SLOT, &rbufs.rp_color_tx);
    pass.bind_image(RBUFS_VALUE_SLOT, &rbufs.rp_value_tx);
    pass.bind_image(RBUFS_CRYPTOMATTE_SLOT, &rbufs.cryptomatte_tx);
    pass.bind_ssbo(CRYPTOMATTE_BUF_SLOT, &cryptomatte_dummy_buf_);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, eevee_utility_tx_);
    pass.bind_ubo(UNIFORM_BUF_SLOT, &eevee_uniform_buf_);
    pass.bind_ssbo(SAMPLING_BUF_SLOT, &eevee_sampling_buf_);
    /* Materials with an Ambient Occlusion node trace the Hi-Z pyramid. */
    pass.bind_texture(HIZ_TEX_SLOT, &inst_->hiz_buffer.front.ref_tx_);
  }

  forward_layer_.prepass_ps.init();
  forward_layer_.shading_ps.init();
}

void PipelineModule::eevee_resources_update(int normal_layer_count)
{
  const UniformData &ud = inst_->uniform_data;
  const int2 render_extent = inst_->film.render_extent_get();

  eevee::CameraData camera = {};
  camera.viewmat = ud.viewmat;
  camera.viewinv = ud.viewinv;
  camera.winmat = ud.projectionmat;
  camera.wininv = math::invert(ud.projectionmat);
  camera.persmat = ud.viewprojmat;
  camera.persinv = math::invert(ud.viewprojmat);
  camera.uv_scale = float2(1.0f);
  camera.uv_bias = float2(0.0f);
  camera.clip_near = ud.z_near;
  camera.clip_far = ud.z_far;
  /* A perspective matrix has a zero in its last diagonal element. */
  camera.type = (ud.projectionmat[3][3] == 0.0f) ? eevee::CAMERA_PERSP : eevee::CAMERA_ORTHO;
  camera.initialized = true;

  /* Motion vectors are previous minus current screen position. Without a
   * valid previous frame (first frame, camera cut, resize) both steps are
   * the same camera, so only object motion remains and static geometry gets
   * exactly zero. */
  const eevee::CameraData camera_prev = inst_->frame_state.history_valid ?
                                            static_cast<eevee::CameraData>(camera_curr_buf_) :
                                            camera;
  camera_prev_buf_ = camera_prev;
  camera_curr_buf_ = camera;
  camera_prev_buf_.push_update();
  camera_curr_buf_.push_update();

  eevee::UniformData data = {};
  data.camera = camera;

  data.film.extent = inst_->film.display_extent_get();
  data.film.render_extent = render_extent;
  data.film.extent_inv = 1.0f / float2(data.film.extent);
  data.film.background_opacity = 1.0f;

  data.hiz.uv_scale = inst_->hiz_buffer.uv_scale;

  /* No render passes and no AOVs: -1 makes the shader skip the image write. */
  eevee::RenderBuffersInfoData &rp = data.render_pass;
  rp.aovs.display_id = -1;
  rp.normal_id = rp.position_id = -1;
  rp.diffuse_light_id = rp.diffuse_color_id = -1;
  rp.specular_light_id = rp.specular_color_id = -1;
  rp.volume_light_id = rp.emission_id = -1;
  rp.environment_id = rp.transparent_id = -1;
  rp.shadow_id = rp.ambient_occlusion_id = -1;

  /* Clamping is an accumulation renderer's firefly control; disabled. */
  data.clamp.sun_threshold = 1e20f;
  data.clamp.surface_direct = 1e20f;
  data.clamp.surface_indirect = 1e20f;
  data.clamp.volume_direct = 1e20f;
  data.clamp.volume_indirect = 1e20f;
  data.clamp.direct_scale = 1.0f;
  data.clamp.indirect_scale = 1.0f;

  /* Alpha hash at pixel scale: with a single sample per pixel, sub-pixel
   * noise would never converge, it would only flicker. */
  data.pipeline.alpha_hash_scale = 1.0f;
  data.pipeline.ray_type = eevee::RAY_TYPE_CAMERA;
  data.pipeline.is_main_view_inverted = math::is_negative(float3x3(ud.viewmat));
  /* Thickness / object data live in the last normal layer. */
  data.pipeline.gbuffer_additional_data_layer_id = normal_layer_count - 1;

  eevee_uniform_buf_ = data;
  eevee_uniform_buf_.push_update();

  static_cast<eevee::SamplingData &>(eevee_sampling_buf_) = {};
  eevee_sampling_buf_.push_update();
  cryptomatte_dummy_buf_.push_update();
}

void PipelineModule::sync()
{
  RenderBuffers &rbufs = inst_->render_buffers;
  const int2 extent = inst_->film.render_extent_get();

  /* ---- G-Buffer ----
   * Layer counts follow eevee::DeferredLayerBase: one extra header layer for
   * the object id, two data layers per closure bin, one normal layer per
   * bin plus one per closure that carries additional data. */
  const int bin_count = eevee::to_gbuffer_bin_count(closure_bits_);
  const int header_layers = 2;
  const int closure_layers = 2 * bin_count;
  const int normal_layers = bin_count +
                            count_bits_i(closure_bits_ & (eevee::CLOSURE_SSS |
                                                          eevee::CLOSURE_TRANSLUCENT |
                                                          eevee::CLOSURE_REFRACTION));
  rbufs.gbuffer.acquire(extent, header_layers, closure_layers, normal_layers);

  /* Attachment order is the fragment output order of `eevee_surf_deferred`:
   * radiance (emission), header, normal, closure, closure. Depth is
   * read-only here (written in the prepass, tested with EQUAL). */
  gbuffer_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                     GPU_ATTACHMENT_TEXTURE(rbufs.combined_tx),
                     GPU_ATTACHMENT_TEXTURE_LAYER(rbufs.gbuffer.header_tx.layer_view(0), 0),
                     GPU_ATTACHMENT_TEXTURE_LAYER(rbufs.gbuffer.normal_tx.layer_view(0), 0),
                     GPU_ATTACHMENT_TEXTURE_LAYER(rbufs.gbuffer.closure_tx.layer_view(0), 0),
                     GPU_ATTACHMENT_TEXTURE_LAYER(rbufs.gbuffer.closure_tx.layer_view(1), 0));

  eevee_resources_update(math::max(normal_layers, int(GBUF_NORMAL_FB_LAYER_COUNT)));

  /* ---- Deferred Lighting pass ----
   * One full-screen triangle. Decodes the G-Buffer, loops over the light
   * list and adds the result on top of the emission the G-Buffer pass
   * already wrote into combined_tx, hence the additive blend. */
  use_ao_ = inst_->gtao.enabled() ? 1 : 0;
  use_ssgi_ = (inst_->ssgi.enabled() && inst_->frame_state.history_valid) ? 1 : 0;

  world_ambient_ = float3(0.0f);
  if (inst_->scene->world != nullptr) {
    const blender::World &world = *inst_->scene->world;
    world_ambient_ = float3(world.horr, world.horg, world.horb);
  }

  PassSimple &light_ps = opaque_layer_.lighting_ps_;
  light_ps.init();
  light_ps.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ADD_FULL);
  light_ps.shader_set(shaders_->static_shader_get(SH_DEFERRED_LIGHT));
  rbufs.gbuffer.bind_resources(light_ps);
  /* Mip 0 of the pyramid is the opaque depth, already un-reversed. */
  light_ps.bind_texture("hiz_tx", &inst_->hiz_buffer.front.ref_tx_);
  light_ps.bind_texture("ao_tx", inst_->gtao.get_result());
  light_ps.bind_texture("ssgi_tx", inst_->ssgi.get_result());
  /* Pre-filtered sun shadow: one [0,1] float per pixel from the PCF compute. */
  light_ps.bind_texture("shadow_mask_tx", &rbufs.shadow_mask_tx);
  inst_->lights.bind_resources(light_ps);
  /* Punctual shadows are looked up per light in the lighting shader. */
  inst_->shadows.bind_resources(light_ps);
  light_ps.push_constant("light_count", &inst_->lights.active_light_count);
  light_ps.push_constant("use_ao", &use_ao_);
  light_ps.push_constant("use_ssgi", &use_ssgi_);
  light_ps.push_constant("world_ambient", &world_ambient_);
  light_ps.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* Intermediate AA targets follow the render resolution. */
  aa.sync_framebuffers(extent);
}

void PipelineModule::render(View &view, Framebuffer &combined_fb)
{
  RenderBuffers &rbufs = inst_->render_buffers;
  const int2 extent = inst_->film.render_extent_get();

  /* ================================================================
   * Pass 1: G-Buffer Fill
   * Opaque geometry writes its closures to the G-Buffer and its emission
   * straight to combined_tx. Depth comes from the prepass (EQUAL, no write).
   * GBuffer::bind() clears the header layer and nothing else: a zero header
   * means "no closure here", which is all the other layers need. */
  GPU_debug_group_begin("GBuffer");
  rbufs.gbuffer.bind(gbuffer_fb_);
  inst_->manager->submit(opaque_layer_.gbuffer_ps, view);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 2: Shadow Atlases + PCF Compute
   *
   * Has to come after the G-Buffer: the PCF compute reads its normals.
   * Has to come before lighting: that is what samples shadow_mask_tx. */
  inst_->shadows.set_view(view, extent);

  /* ================================================================
   * Pass 3: Screen-Space Ambient Occlusion (GTAO) */
  GPU_debug_group_begin("GTAO");
  inst_->gtao.render(view, rbufs.depth_tx);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 4: Screen-Space Global Illumination (SSGI)
   * One diffuse bounce of last frame's radiance. */
  GPU_debug_group_begin("SSGI");
  inst_->ssgi.render(view, rbufs.combined_prev_tx, rbufs.depth_tx);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 5: Deferred Lighting
   * Adds the lit radiance of all opaque surfaces to combined_tx. */
  GPU_debug_group_begin("DeferredLighting");
  combined_fb.bind();
  inst_->manager->submit(opaque_layer_.lighting_ps_, view);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 6: Volumetric fog over the opaque result (no-op when disabled). */
  GPU_debug_group_begin("Volumetrics");
  inst_->volume.render(view);
  inst_->volume.resolve(rbufs.combined_tx, rbufs.depth_tx);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 7: Transparent Forward + FSR Reactive Mask
   *
   * Not drawn yet. EEVEE's forward surface shader evaluates lighting itself
   * and reads EEVEE's own light culling buffers, virtual shadow maps and
   * light probes. None of that exists here, so submitting these passes would
   * run shaders against unbound buffers. They need either a forward variant
   * of the game light loop injected through the material create info, or the
   * game light list exposed in the layout `eevee_light_data` expects.
   *
   * The reactive mask logic below is what the pass needs once it draws: FSR
   * has to know which pixels were changed by transparency so its temporal
   * accumulation does not ghost them. */
  if (use_forward_pass) {
    GPU_debug_group_begin("Forward.Transparent");

    const bool use_fsr = inst_->upscale.is_initialized();
    if (use_fsr) {
      opaque_copy_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16);
      /* GPU_texture_copy(dst, src) */
      GPU_texture_copy(opaque_copy_tx_, rbufs.combined_tx);
    }

    combined_fb.bind();
    inst_->manager->submit(forward_layer_.shading_ps, view);

    if (use_fsr) {
      inst_->upscale.generate_masks(
          opaque_copy_tx_, rbufs.combined_tx, rbufs.reactive_mask_tx, rbufs.transp_mask_tx);
      opaque_copy_tx_.release();
    }

    GPU_debug_group_end();
  }
}

/* ================================================================
 * material_add
 *
 * Called by MaterialModule to register a new shader bucket into the
 * correct pipeline pass. Returns a PassMain::Sub* that MaterialModule
 * will further subdivide per material.
 *
 * Pipeline pass selection mirrors EEVEE's PipelineModule::material_add().
 * ================================================================ */

PassMain::Sub *PipelineModule::material_add(Object * /*ob*/,
                                            blender::Material *blender_mat,
                                            GPUMaterial *gpumat,
                                            eevee::eMaterialPipeline pipeline_type)
{
  using namespace eevee;

  /* Back-face culling halves the fragment work of closed meshes. */
  const DRWState cull_state = (blender_mat->blend_flag & MA_BL_CULL_BACKFACE) ?
                                  DRW_STATE_CULL_BACK :
                                  DRW_STATE_NO_DRAW;

  /* Shader To RGB and ray casting materials compile to the "hybrid" deferred
   * shader, which lights itself from EEVEE's light, shadow and probe buffers.
   * Those do not exist here; such materials are skipped, depth included, so
   * they do not leave unlit silhouettes. */
  const bool deferred_unsupported = GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_TO_RGBA) ||
                                    GPU_material_flag_get(gpumat, GPU_MATFLAG_RAYCAST);

  switch (pipeline_type) {
    /* Depth + optional velocity prepass, deferred opaque. */
    case MAT_PIPE_PREPASS_DEFERRED:
    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY: {
      if (deferred_unsupported) {
        return nullptr;
      }
      PassMain::Sub &sub = opaque_layer_.prepass_ps.sub(GPU_material_get_name(gpumat));
      /* Reversed depth (1 = near): the nearest fragment has the greatest value.
       * Unit range clip control is what EEVEE's vertex stage is written for. */
      sub.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_GREATER_EQUAL |
                    DRW_STATE_CLIP_CONTROL_UNIT_RANGE | cull_state);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* G-Buffer fill, deferred opaque surface shading. */
    case MAT_PIPE_DEFERRED: {
      if (deferred_unsupported) {
        return nullptr;
      }
      closure_bits_ |= shader_closure_bits_from_flag(gpumat);

      PassMain::Sub &sub = opaque_layer_.gbuffer_ps.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL |
                    DRW_STATE_CLIP_CONTROL_UNIT_RANGE | cull_state);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* Depth prepass for forward opaque / cutout. */
    case MAT_PIPE_PREPASS_FORWARD:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY: {
      PassMain::Sub &sub = forward_layer_.prepass_ps.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_GREATER_EQUAL |
                    DRW_STATE_CLIP_CONTROL_UNIT_RANGE | cull_state);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* Forward opaque shading (writes directly to combined_tx, not G-Buffer). */
    case MAT_PIPE_FORWARD: {
      if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT)) {
        /* Transparent forward: per-object sub-passes via material_transparent_add(). */
        return nullptr;
      }
      PassMain::Sub &sub = forward_layer_.shading_ps.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* Shadows reuse the depth prepass (see ShadowModule::render_tile). Volumes
     * and transparent overlap masking are handled per-object. */
    case MAT_PIPE_SHADOW:
    case MAT_PIPE_VOLUME_OCCUPANCY:
    case MAT_PIPE_VOLUME_MATERIAL:
    case MAT_PIPE_PREPASS_OVERLAP:
      return nullptr;

    default:
      BLI_assert_unreachable();
      return nullptr;
  }
}

/* ================================================================
 * Transparent forward: per-object sub-passes for back-to-front sorting
 * ================================================================ */

PassMain::Sub *PipelineModule::prepass_transparent_add(Object *ob,
                                                       blender::Material * /*blender_mat*/,
                                                       GPUMaterial *gpumat)
{
  /* Each transparent object gets its own sub so the draw list can reorder them.
   * Name by object pointer for uniqueness within this frame. */
  char name[32];
  SNPRINTF(name, "TranspPrepass_%p", static_cast<void *>(ob));
  PassMain::Sub &sub = forward_layer_.prepass_ps.sub(name);
  sub.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_GREATER_EQUAL);
  sub.shader_set(GPU_material_get_shader(gpumat));
  sub.material_set(*inst_->manager, gpumat, true, inst_->anisotropic_filtering);
  return &sub;
}

PassMain::Sub *PipelineModule::material_transparent_add(Object *ob,
                                                        blender::Material * /*blender_mat*/,
                                                        GPUMaterial *gpumat)
{
  char name[32];
  SNPRINTF(name, "Transp_%p", static_cast<void *>(ob));
  PassMain::Sub &sub = forward_layer_.shading_ps.sub(name);
  /* Alpha blend, depth test without write. */
  sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_GREATER_EQUAL | DRW_STATE_BLEND_ALPHA);
  sub.shader_set(GPU_material_get_shader(gpumat));
  sub.material_set(*inst_->manager, gpumat, true, inst_->anisotropic_filtering);
  return &sub;
}

}  // namespace blender::eevee_game

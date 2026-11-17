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
    /* `eevee_surf_depth` of the blended surfaces, from the lights only. */
    PassMain &pass = shadow_casters_ps_;
    pass.init();
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, eevee_utility_tx_);
    pass.bind_ubo(UNIFORM_BUF_SLOT, &eevee_uniform_buf_);
    pass.bind_ssbo(SAMPLING_BUF_SLOT, &eevee_sampling_buf_);
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

  {
    /* Forward surfaces. */
    if (!dummy_object_id_tx_.is_valid()) {
      const uint zero_u = 0u;
      const float zero_f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      dummy_object_id_tx_.ensure_2d(
          gpu::TextureFormat::UINT_32, int2(1), GPU_TEXTURE_USAGE_SHADER_READ);
      GPU_texture_clear(dummy_object_id_tx_, GPU_DATA_UINT, &zero_u);
      dummy_normal_tx_.ensure_2d(
          gpu::TextureFormat::SFLOAT_16_16, int2(1), GPU_TEXTURE_USAGE_SHADER_READ);
      GPU_texture_clear(dummy_normal_tx_, GPU_DATA_FLOAT, zero_f);
      /* Clear air: nothing scattered in, everything transmitted. */
      dummy_volume_tx_.ensure_3d(
          gpu::TextureFormat::SFLOAT_16_16_16_16, int3(1), GPU_TEXTURE_USAGE_SHADER_READ);
      GPU_texture_clear(dummy_volume_tx_, GPU_DATA_FLOAT, zero_f);
      const float one_f[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      dummy_transmittance_tx_.ensure_3d(
          gpu::TextureFormat::SFLOAT_16_16_16_16, int3(1), GPU_TEXTURE_USAGE_SHADER_READ);
      GPU_texture_clear(dummy_transmittance_tx_, GPU_DATA_FLOAT, one_f);
    }
    forward_layer_.opaque_ps.init();
    forward_resources_bind(forward_layer_.opaque_ps);

    forward_layer_.transparent_ps.init();
    /* A PassSortable binds through a sub-pass sorted before all the others. */
    PassMain::Sub &bind_sub = forward_layer_.transparent_ps.sub("ResourceBind", -FLT_MAX);
    forward_resources_bind(bind_sub);
  }
}

template<typename PassT> void PipelineModule::light_data_bind(PassT &pass)
{
  pass.bind_ssbo(LIGHT_BUF_SLOT, &inst_->lights.light_buf_);
  pass.bind_ssbo(LIGHT_ZBIN_BUF_SLOT, &light_tile_mask_buf_);
  pass.bind_ubo(SPHERE_PROBE_BUF_SLOT, &lighting_buf_);
  inst_->shadows.bind_resources_by_slot(pass);
  inst_->world.bind_resources(pass);
}
template void PipelineModule::light_data_bind(PassSimple &pass);

template<typename PassT> void PipelineModule::forward_resources_bind(PassT &pass)
{
  /* `eevee_game_surf_forward` + the depth shaders of the overlap masks. Bound
   * by slot: the generated shaders are only known per sub-pass. */
  pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, eevee_utility_tx_);
  pass.bind_ubo(UNIFORM_BUF_SLOT, &eevee_uniform_buf_);
  pass.bind_ssbo(SAMPLING_BUF_SLOT, &eevee_sampling_buf_);
  pass.bind_texture(HIZ_TEX_SLOT, &inst_->hiz_buffer.front.ref_tx_);
  light_data_bind(pass);
  /* Shader To RGB materials declare the previous layer inputs of EEVEE's
   * layered transparency; this shader never reads them. Not combined_tx: it
   * is the color attachment being drawn to. */
  pass.bind_texture(HIZ_PREVIOUS_LAYER_TEX_SLOT, &inst_->hiz_buffer.front.ref_tx_);
  pass.bind_texture(RADIANCE_PREVIOUS_LAYER_TEX_SLOT, &inst_->hiz_buffer.front.ref_tx_);
  pass.bind_texture(OBJECT_ID_TEX_SLOT, dummy_object_id_tx_);
  pass.bind_texture(PREPASS_NORMAL_TEX_SLOT, dummy_normal_tx_);
  pass.bind_texture(VOLUME_SCATTERING_TEX_SLOT, &volume_tx_);
  pass.bind_texture(VOLUME_TRANSMITTANCE_TEX_SLOT, &volume_transmittance_tx_);
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
  /* FSR is the only reader of the motion buffer. A still camera gives the
   * static pixels exactly zero motion, which is what the clear writes. */
  use_velocity_resolve_ = inst_->upscale.is_initialized() &&
                          (camera_prev.persmat != camera.persmat);

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

  /* Froxel mapping of the voxelized volume shaders. */
  inst_->volume.eevee_data_fill(data.volumes);

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

  /* ---- Camera motion of the static pixels ---- */
  if (use_velocity_resolve_) {
    PassSimple &pass = velocity_resolve_ps_;
    pass.init();
    pass.shader_set(shaders_->static_shader_get(SH_VELOCITY_RESOLVE));
    pass.bind_texture("depth_tx", &rbufs.depth_tx);
    pass.bind_image("vector_img", &rbufs.vector_tx);
    pass.bind_ubo(VELOCITY_CAMERA_PREV_BUF, &camera_prev_buf_);
    pass.bind_ubo(VELOCITY_CAMERA_CURR_BUF, &camera_curr_buf_);
    pass.bind_ubo(VELOCITY_CAMERA_NEXT_BUF, &camera_curr_buf_);
    pass.dispatch(int3((extent.x + 7) / 8, (extent.y + 7) / 8, 1));
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }

  /* ---- Lighting constants and tiled light culling ---- */
  {
    const int light_count = inst_->lights.active_light_count;
    const int2 tile_count = math::divide_ceil(extent, int2(LIGHT_TILE_SIZE));
    const int word_count = math::max(1, (light_count + 31) / 32);
    lighting_buf_.light_count = light_count;
    lighting_buf_.tile_count = tile_count;
    lighting_buf_.render_extent = extent;
    lighting_buf_.word_count = word_count;

    /* Fog of the forward surfaces: same depth slicing as the volume resolve. */
    const UniformData &ud = inst_->uniform_data;
    const bool use_volume = inst_->volume.enabled();
    volume_tx_ = use_volume ? inst_->volume.get_volume_texture() : dummy_volume_tx_;
    volume_transmittance_tx_ = use_volume ? inst_->volume.get_transmittance_texture() :
                                            dummy_transmittance_tx_;
    lighting_buf_.use_volume = use_volume;
    lighting_buf_.volume_z_near = ud.z_near;
    lighting_buf_.volume_log_inv = 1.0f / logf(ud.z_far / ud.z_near);

    /* Sized for the most lights, so the buffer only grows with the extent. */
    const int mask_len = tile_count.x * tile_count.y * (MAX_LIGHTS / 32);
    if (mask_len > light_tile_mask_capacity_) {
      light_tile_mask_buf_.resize(mask_len);
      light_tile_mask_capacity_ = mask_len;
    }

    PassSimple &cull = light_cull_ps_;
    cull.init();
    cull.shader_set(shaders_->static_shader_get(SH_LIGHT_CULL));
    cull.bind_ubo("game_lighting_buf", &lighting_buf_);
    cull.bind_ssbo("light_buf", &inst_->lights.light_buf_);
    cull.bind_ssbo("light_tile_mask_out_buf", &light_tile_mask_buf_);
    cull.bind_texture("hiz_tx", &inst_->hiz_buffer.front.ref_tx_);
    const int thread_count = tile_count.x * tile_count.y * word_count;
    cull.dispatch(int3((thread_count + 63) / 64, 1, 1));
    /* Read by the lighting and forward passes right after. */
    cull.barrier(GPU_BARRIER_SHADER_STORAGE);
  }

  /* ---- Deferred Lighting pass ----
   * One full-screen triangle. Decodes the G-Buffer, loops over the light
   * list and adds the result on top of the emission the G-Buffer pass
   * already wrote into combined_tx, hence the additive blend. */
  use_ao_ = inst_->gtao.enabled() ? 1 : 0;
  use_ssgi_ = (inst_->ssgi.enabled() && inst_->frame_state.history_valid) ? 1 : 0;

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
  inst_->world.bind_resources(light_ps);
  light_ps.bind_ubo("game_lighting_buf", &lighting_buf_);
  light_ps.bind_ssbo("light_tile_mask_buf", &light_tile_mask_buf_);
  light_ps.push_constant("use_ao", &use_ao_);
  light_ps.push_constant("use_ssgi", &use_ssgi_);
  light_ps.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* Forward surfaces: same lights as the deferred pass. */
  lighting_buf_.push_update();

  /* Intermediate AA targets follow the render resolution. */
  aa.sync_framebuffers(extent);
}

void PipelineModule::render_velocity_resolve(View &view)
{
  if (!use_velocity_resolve_) {
    return;
  }
  GPU_debug_group_begin("Velocity.Resolve");
  inst_->manager->submit(velocity_resolve_ps_, view);
  GPU_debug_group_end();
}

void PipelineModule::render(View &view, Framebuffer &combined_fb)
{
  RenderBuffers &rbufs = inst_->render_buffers;
  const int2 extent = inst_->film.render_extent_get();

  /* ================================================================
   * Pass 0: Tiled light culling, against this frame's Hi-Z (built by the
   * prepass). Read by the deferred lighting and the forward surfaces. */
  GPU_debug_group_begin("LightCulling");
  inst_->manager->submit(light_cull_ps_, view);
  GPU_debug_group_end();

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
   * Pass 7: Forward surfaces + FSR Reactive Mask
   *
   * Opaque forward surfaces first (EQUAL test against their prepass depth),
   * then the transparent ones back to front. FSR has to know which pixels
   * transparency changed so its temporal accumulation does not ghost them. */
  GPU_debug_group_begin("Forward");
  combined_fb.bind();
  inst_->manager->submit(forward_layer_.opaque_ps, view);

  const bool use_fsr = inst_->upscale.is_initialized();
  if (use_fsr) {
    opaque_copy_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16);
    /* GPU_texture_copy(dst, src) */
    GPU_texture_copy(opaque_copy_tx_, rbufs.combined_tx);
    combined_fb.bind();
  }

  inst_->manager->submit(forward_layer_.transparent_ps, view);

  if (use_fsr) {
    inst_->upscale.generate_masks(
        opaque_copy_tx_, rbufs.combined_tx, rbufs.reactive_mask_tx, rbufs.transp_mask_tx);
    opaque_copy_tx_.release();
  }
  GPU_debug_group_end();
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

  /* Shader To RGB and ray casting materials compile to EEVEE's "hybrid"
   * deferred shader, which lights itself from EEVEE's light, shadow and probe
   * buffers. MaterialModule moves them to the forward pipeline; never draw
   * them as deferred. */
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
    /* Depth prepass of forward opaque surfaces: with the deferred ones, so
     * they enter the Hi-Z, the AO and the shadow maps. */
    case MAT_PIPE_PREPASS_FORWARD:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY: {
      PassMain::Sub &sub = opaque_layer_.prepass_ps.sub(GPU_material_get_name(gpumat));
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
      PassMain::Sub &sub = forward_layer_.opaque_ps.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL |
                    DRW_STATE_CLIP_CONTROL_UNIT_RANGE | cull_state);
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

/* Back to front: the camera +Z axis points toward the viewer, so the
 * farthest object has the smallest value and PassSortable draws ascending. */
static float transparent_sorting_value(const Object *ob, const float4x4 &viewinv)
{
  return math::dot(float3(ob->object_to_world().location()), float3(viewinv[2]));
}

template<typename PassT> void PipelineModule::eevee_globals_bind(PassT &pass)
{
  pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, eevee_utility_tx_);
  pass.bind_ubo(UNIFORM_BUF_SLOT, &eevee_uniform_buf_);
  pass.bind_ssbo(SAMPLING_BUF_SLOT, &eevee_sampling_buf_);
}
template void PipelineModule::eevee_globals_bind(PassSimple &pass);
template void PipelineModule::eevee_globals_bind(PassMain::Sub &pass);

void PipelineModule::render_pass_outputs_bind(PassSimple &pass)
{
  RenderBuffers &rbufs = inst_->render_buffers;
  pass.bind_image(RBUFS_COLOR_SLOT, &rbufs.rp_color_tx);
  pass.bind_image(RBUFS_VALUE_SLOT, &rbufs.rp_value_tx);
  pass.bind_image(RBUFS_CRYPTOMATTE_SLOT, &rbufs.cryptomatte_tx);
  pass.bind_ssbo(CRYPTOMATTE_BUF_SLOT, &cryptomatte_dummy_buf_);
}

PassMain::Sub *PipelineModule::shadow_caster_add(blender::Material *blender_mat,
                                                 GPUMaterial *gpumat)
{
  /* Same depth state as the opaque prepass the atlases are drawn with. */
  DRWState state = DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_GREATER_EQUAL |
                   DRW_STATE_CLIP_CONTROL_UNIT_RANGE;
  if (blender_mat->blend_flag & MA_BL_CULL_BACKFACE_SHADOW) {
    state |= DRW_STATE_CULL_BACK;
  }
  PassMain::Sub &sub = shadow_casters_ps_.sub(GPU_material_get_name(gpumat));
  sub.state_set(state);
  sub.shader_set(GPU_material_get_shader(gpumat));
  return &sub;
}

PassMain::Sub *PipelineModule::prepass_transparent_add(Object *ob,
                                                       blender::Material *blender_mat,
                                                       GPUMaterial *gpumat)
{
  /* Depth-only mask so the object only shows its nearest layer. Same sorting
   * value as its shading sub-pass, added first: the sort keeps the order of
   * equal values. */
  if ((blender_mat->blend_flag & MA_BL_HIDE_BACKFACE) == 0) {
    return nullptr;
  }
  DRWState state = DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_GREATER_EQUAL |
                   DRW_STATE_CLIP_CONTROL_UNIT_RANGE;
  if (blender_mat->blend_flag & MA_BL_CULL_BACKFACE) {
    state |= DRW_STATE_CULL_BACK;
  }
  PassMain::Sub &sub = forward_layer_.transparent_ps.sub(
      GPU_material_get_name(gpumat), transparent_sorting_value(ob, inst_->uniform_data.viewinv));
  sub.state_set(state);
  sub.material_set(*inst_->manager, gpumat, true, inst_->anisotropic_filtering);
  return &sub;
}

PassMain::Sub *PipelineModule::material_transparent_add(Object *ob,
                                                        blender::Material *blender_mat,
                                                        GPUMaterial *gpumat)
{
  /* Premultiplied: dst = radiance + dst * transmittance. Depth tested, not
   * written: what is behind stays visible through the next layers. */
  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_GREATER_EQUAL |
                   DRW_STATE_BLEND_ALPHA_PREMUL | DRW_STATE_CLIP_CONTROL_UNIT_RANGE;
  if (blender_mat->blend_flag & MA_BL_CULL_BACKFACE) {
    state |= DRW_STATE_CULL_BACK;
  }
  PassMain::Sub &sub = forward_layer_.transparent_ps.sub(
      GPU_material_get_name(gpumat), transparent_sorting_value(ob, inst_->uniform_data.viewinv));
  sub.state_set(state);
  sub.material_set(*inst_->manager, gpumat, true, inst_->anisotropic_filtering);
  return &sub;
}

}  // namespace blender::eevee_game

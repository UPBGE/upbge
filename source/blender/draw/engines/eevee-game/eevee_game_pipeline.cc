/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_pipeline.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

/* ================================================================
 * AAModule
 * ================================================================ */

void AAModule::init()
{
  /* SMAA Area LUT (256x256 RG8): encodes the theoretical area covered by each
   * possible crossing pattern between two orthogonal edges.
   * This texture is constant for all scenes and resolutions. */
  smaa_area_lut_tx_ = std::make_unique<gpu::Texture>("smaa_area_lut");
  smaa_area_lut_tx_->ensure_2d(
      gpu::TextureFormat::RG8,
      int2(256, 256),
      GPU_TEXTURE_USAGE_SHADER_READ);

  /* Upload the SMAA area LUT data.
   * smaa_area_lut_bytes is defined in a companion header generated from the
   * official SMAA repository (smaa_area_lut.h / smaa_search_lut.h).
   * The data is a static const uint8_t array; no runtime allocation needed. */
  GPU_texture_update(smaa_area_lut_tx_->get(),
                     GPU_DATA_UBYTE,
                     smaa_area_lut_bytes);

  /* SMAA Search LUT (64x16 RGBA8): encodes the crossing pattern search path
   * used by the blending weight calculation pass to find edge endpoints.
   * Without this texture the SMAABlendingWeightCalculationPS function in
   * SMAA_STAGE==1 reads from an unbound sampler and produces zero weights —
   * the neighbourhood blend pass then outputs the unfiltered source unchanged,
   * making SMAA silently degrade to a no-op.
   *
   * smaa_search_lut_bytes is defined in smaa_search_lut.h, generated from
   * the official SMAA repository alongside smaa_area_lut.h. */
  smaa_search_lut_tx_ = std::make_unique<gpu::Texture>("smaa_search_lut");
  smaa_search_lut_tx_->ensure_2d(
      gpu::TextureFormat::RGBA8,
      int2(64, 16),
      GPU_TEXTURE_USAGE_SHADER_READ);
  GPU_texture_update(smaa_search_lut_tx_->get(),
                     GPU_DATA_UBYTE,
                     smaa_search_lut_bytes);
}

void AAModule::sync_framebuffers(int2 extent)
{
  /* Re-configure the three SMAA framebuffers whenever the render resolution
   * changes. The pooled textures (smaa_edge_tx_, smaa_weight_tx_) are
   * acquired per-call in apply_smaa() so they always carry the correct size,
   * but the framebuffer config must be rebuilt to match them.
   *
   * Without this, a resolution change leaves the FBO pointing at textures
   * from the previous resolution, causing SMAA_STAGE==1 and ==2 to write
   * outside the valid region and read undefined border texels on AMD/Intel.
   *
   * Called from PipelineModule::sync() every time the resolution changes. */
  smaa_edge_tx_.acquire(extent,   gpu::TextureFormat::RG8);
  smaa_weight_tx_.acquire(extent, gpu::TextureFormat::RGBA8);

  GPU_framebuffer_ensure_config(
      &smaa_edge_fb_,
      { GPU_ATTACHMENT_NONE,
        GPU_ATTACHMENT_TEXTURE(smaa_edge_tx_.get()) });

  GPU_framebuffer_ensure_config(
      &smaa_weight_fb_,
      { GPU_ATTACHMENT_NONE,
        GPU_ATTACHMENT_TEXTURE(smaa_weight_tx_.get()) });

  /* smaa_blend_fb_ target (dst) is set per-call in apply_smaa() because
   * the destination texture changes depending on whether FSR is active. */

  smaa_edge_tx_.release();
  smaa_weight_tx_.release();
}

void AAModule::apply_fxaa(gpu::Texture *src, gpu::Texture *dst)
{
  /* FXAA is a single-pass screen-space filter.
   * It blurs pixels where the local luma gradient exceeds fxaa_threshold,
   * using fxaa_mul to control how aggressively sub-pixel aliasing is blended.
   *
   * Threshold = 0.166 (1/6): standard "medium quality" preset from Nvidia's paper.
   * Mul = 0.125: sub-pixel blend strength; higher = softer but blurrier. */
  fxaa_ps_.init();
  fxaa_ps_.shader_set(inst_->shaders.static_shader_get(SH_FXAA));
  fxaa_ps_.bind_texture("color_tx", src);
  fxaa_ps_.bind_image("out_img",    dst);
  fxaa_ps_.push_constant("fxaa_params", float4(0.166f, 0.125f, 0.0f, 0.0f));

  /* Full-screen triangle: 3 vertices, no index buffer, covers [-1,3] in each axis */
  fxaa_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

void AAModule::apply_smaa(gpu::Texture *src, gpu::Texture *dst)
{
  /* SMAA (Subpixel Morphological Anti-Aliasing) - 3 rasterisation passes.
   *
   * Pass 1 - Edge Detection:
   *   Detects luma or color edges and writes a 2-channel edge mask.
   *   Only pixels with edges are processed in pass 2.
   *
   * Pass 2 - Blending Weights:
   *   For each edge pixel, finds crossing patterns using the Area LUT
   *   and computes per-pixel blend weights (RGBA8).
   *
   * Pass 3 - Neighborhood Blending:
   *   Uses the weight map to linearly blend neighbouring pixels,
   *   producing the final anti-aliased output.
   *
   * Reference: Jimenez et al., "Practical Morphological Anti-Aliasing", GPU Pro 2. */

  const int2 extent = src->size().xy();

  /* Acquire intermediate textures from the pool for this frame */
  smaa_edge_tx_.acquire(extent,   gpu::TextureFormat::RG8);
  smaa_weight_tx_.acquire(extent, gpu::TextureFormat::RGBA8);

  /* Attach pooled textures to their framebuffers now that they have GPU handles */
  GPU_framebuffer_ensure_config(
      &smaa_edge_fb_,
      { GPU_ATTACHMENT_NONE,
        GPU_ATTACHMENT_TEXTURE(smaa_edge_tx_.get()) });

  GPU_framebuffer_ensure_config(
      &smaa_weight_fb_,
      { GPU_ATTACHMENT_NONE,
        GPU_ATTACHMENT_TEXTURE(smaa_weight_tx_.get()) });

  GPU_framebuffer_ensure_config(
      &smaa_blend_fb_,
      { GPU_ATTACHMENT_NONE,
        GPU_ATTACHMENT_TEXTURE(dst) });

  /* ---- Pass 1: Edge Detection ---- */
  smaa_edge_ps_.init();
  smaa_edge_ps_.shader_set(inst_->shaders.static_shader_get(SH_SMAA_EDGE));
  smaa_edge_ps_.bind_texture("color_tx", src);
  smaa_edge_ps_.framebuffer_set(&smaa_edge_fb_);

  GPU_framebuffer_bind(&smaa_edge_fb_);
  GPU_framebuffer_clear_color(&smaa_edge_fb_, float4(0.0f));
  smaa_edge_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* ---- Pass 2: Blending Weights ---- */
  smaa_weight_ps_.init();
  smaa_weight_ps_.shader_set(inst_->shaders.static_shader_get(SH_SMAA_WEIGHT));
  smaa_weight_ps_.bind_texture("edges_tx", smaa_edge_tx_.get());
  smaa_weight_ps_.bind_texture("area_tx",  smaa_area_lut_tx_.get());
  smaa_weight_ps_.bind_texture("search_tx", smaa_search_lut_tx_.get());
  smaa_weight_ps_.framebuffer_set(&smaa_weight_fb_);

  GPU_framebuffer_bind(&smaa_weight_fb_);
  GPU_framebuffer_clear_color(&smaa_weight_fb_, float4(0.0f));
  smaa_weight_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* ---- Pass 3: Neighborhood Blending ---- */
  smaa_blend_ps_.init();
  smaa_blend_ps_.shader_set(inst_->shaders.static_shader_get(SH_SMAA_BLEND));
  smaa_blend_ps_.bind_texture("color_tx",  src);
  smaa_blend_ps_.bind_texture("blend_tx",  smaa_weight_tx_.get());
  /* mix_factor = 1.0: no TAA history to blend with in game mode */
  smaa_blend_ps_.push_constant("mix_factor", 1.0f);
  smaa_blend_ps_.framebuffer_set(&smaa_blend_fb_);

  GPU_framebuffer_bind(&smaa_blend_fb_);
  smaa_blend_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* Return intermediate textures to the pool */
  smaa_edge_tx_.release();
  smaa_weight_tx_.release();
}

void AAModule::apply_aa(gpu::Texture *src, gpu::Texture *dst, const AASettings &settings)
{
  /* Only active when FSR is OFF; spatial AA is the presentation fallback */
  if (!settings.enabled) {
    return;
  }
  switch (settings.mode) {
    case AAMode::FXAA:
      apply_fxaa(src, dst);
      break;
    case AAMode::SMAA:
      apply_smaa(src, dst);
      break;
    default:
      break;
  }
}

/* ================================================================
 * PipelineModule
 * ================================================================ */

PipelineModule::PipelineModule(ShaderModule *shaders, GameInstance *inst)
    : inst_(inst), shaders_(shaders), aa(inst)
{
}

void PipelineModule::sync()
{
  const int2 extent = inst_->film.render_extent_get();

  /* ---- G-Buffer fill pass ----
   * Renders all opaque geometry and writes the closure data into the G-Buffer layers:
   *   rp_color_tx layer 0 : packed world-space normal (RGB) + material flags (A)
   *   rp_color_tx layer 1 : base color / albedo (RGB) + roughness (A)
   *   rp_value_tx layer 0 : specular intensity, metalness, emission mask, ...
   * The shader selects the correct output layout from the closure bitmask. */
  opaque_layer_.gbuffer_ps_.init();
  opaque_layer_.gbuffer_ps_.shader_set(shaders_->static_shader_get(SH_GBUFFER));
  opaque_layer_.gbuffer_ps_.bind_resources(inst_->uniform_data);
  opaque_layer_.gbuffer_ps_.bind_resources(inst_->sampling);

  /* Attach the G-Buffer textures to the framebuffer.
   * Depth is read-only here (written in the prepass). */
  GPU_framebuffer_ensure_config(
      &gbuffer_fb_,
      {
          GPU_ATTACHMENT_TEXTURE(&inst_->render_buffers.depth_tx),
          GPU_ATTACHMENT_TEXTURE_LAYER(&inst_->render_buffers.rp_color_tx, 0),
          GPU_ATTACHMENT_TEXTURE_LAYER(&inst_->render_buffers.rp_color_tx, 1),
          GPU_ATTACHMENT_TEXTURE_LAYER(&inst_->render_buffers.rp_value_tx, 0),
      });

  /* Keep the GBufferDescriptor on GameInstance in sync so other modules
   * (SSR, SSGI, lighting) can find the textures without re-querying. */
  inst_->gbuffer.normal_tx  = &inst_->render_buffers.rp_color_tx;
  inst_->gbuffer.closure_tx = &inst_->render_buffers.rp_color_tx;
  inst_->gbuffer.header_tx  = &inst_->render_buffers.rp_value_tx;

  /* ---- Tiled Deferred Lighting pass ----
   * A full-screen pass that reads the G-Buffer and evaluates all lights
   * within each screen tile. The tile size (e.g. 16x16) is hardcoded in the shader.
   * Binds: shadow mask, light SSBO, AO result, SSGI result, uniform data.
   *
   * shadow_mask_tx is written by the PCF compute pass (dispatch_pcf) which runs
   * inside ShadowModule::set_view(), called from render() below before this pass. */
  opaque_layer_.lighting_ps_.init();
  opaque_layer_.lighting_ps_.shader_set(shaders_->static_shader_get(SH_DEFERRED_LIGHT));

  /* G-Buffer inputs for the lighting shader */
  opaque_layer_.lighting_ps_.bind_texture("gbuffer_normal_tx",  inst_->gbuffer.normal_tx);
  opaque_layer_.lighting_ps_.bind_texture("gbuffer_closure_tx", inst_->gbuffer.closure_tx);
  opaque_layer_.lighting_ps_.bind_texture("gbuffer_header_tx",  inst_->gbuffer.header_tx);

  /* AO and GI from the screen-space effect modules */
  opaque_layer_.lighting_ps_.bind_texture("ao_tx",   inst_->gtao.get_result());
  opaque_layer_.lighting_ps_.bind_texture("ssgi_tx", inst_->ssgi.get_result());

  /* Pre-filtered shadow mask: one [0,1] float per pixel written by the PCF compute.
   * Binding here is safe because the texture handle is stable across frames;
   * the actual pixel data is filled during set_view() before submit(). */
  opaque_layer_.lighting_ps_.bind_texture("shadow_mask_tx",
                                           &inst_->render_buffers.shadow_mask_tx);

  /* Shadow atlas and light list */
  inst_->shadows.bind_resources(opaque_layer_.lighting_ps_);
  inst_->lights.bind_resources(opaque_layer_.lighting_ps_);

  opaque_layer_.lighting_ps_.bind_resources(inst_->uniform_data);

  /* ---- Transparent Forward pass ----
   * Rendered after deferred so transparencies blend over the resolved lighting.
   * Uses the same depth buffer for correct depth testing against opaque geometry. */
  transparent_layer_.shading_ps_.init();
  transparent_layer_.shading_ps_.shader_set(shaders_->static_shader_get(SH_FORWARD));
  transparent_layer_.shading_ps_.bind_resources(inst_->uniform_data);
  inst_->lights.bind_resources(transparent_layer_.shading_ps_);
  inst_->shadows.bind_resources(transparent_layer_.shading_ps_);

  /* Sync SMAA framebuffers in case the resolution changed */
  aa.sync_framebuffers(extent);
}

void PipelineModule::render(View &view, Framebuffer &combined_fb)
{
  const int2 extent = inst_->film.render_extent_get();

  /* ================================================================
   * Pass 1: G-Buffer Fill
   * All opaque geometry writes its closure data.
   * Depth is already populated from the prepass (depth test = EQUAL, no write). */
  GPU_debug_group_begin("GBuffer");
  GPU_framebuffer_bind(&gbuffer_fb_);
  /* Clear only the closure layers; depth was cleared in the prepass. */
  GPU_framebuffer_clear_color(&gbuffer_fb_, float4(0.0f));
  inst_->manager->submit(opaque_layer_.gbuffer_ps_, view);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 2: Shadow Atlas + PCF Compute
   *
   * set_view() does two things in sequence:
   *   a) Rasterises scene geometry from each light's POV into the fixed atlas.
   *   b) Dispatches the PCF 3x3 compute shader that reads G-Buffer depth+normal
   *      and writes shadow_mask_tx (one float per pixel).
   *
   * This must happen BEFORE GTAO/SSGI/Lighting so that shadow_mask_tx is
   * populated when the lighting pass samples it.
   *
   * Why here and not in view.cc before pipelines.render()?
   *   The PCF compute reads rp_color_tx (normals) which is written in Pass 1.
   *   Calling set_view() before the G-Buffer fill would read stale normal data. */
  inst_->shadows.set_view(view, extent);

  /* ================================================================
   * Pass 3: Screen-Space Ambient Occlusion (GTAO)
   * Run before the lighting pass so ao_tx is ready to bind.
   * Works on the just-written normal and depth data. */
  GPU_debug_group_begin("GTAO");
  inst_->gtao.render(view,
                     &inst_->render_buffers.depth_tx,
                     inst_->gbuffer.normal_tx);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 4: Screen-Space Global Illumination (SSGI)
   * Computes one diffuse bounce from the current frame's radiance.
   * Uses the combined_tx from the previous frame as the radiance source,
   * which is still resident in GPU memory at this point. */
  GPU_debug_group_begin("SSGI");
  inst_->ssgi.render(view,
                     inst_->render_buffers.combined_tx.get(),
                     &inst_->render_buffers.depth_tx,
                     inst_->gbuffer.normal_tx);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 5: Tiled Deferred Lighting
   * Full-screen pass that evaluates all lights per tile, reading from:
   *   - G-Buffer layers (normals, closures)
   *   - GTAO result (ambient occlusion)
   *   - SSGI result (indirect bounce)
   *   - shadow_mask_tx (pre-filtered PCF shadow factor, written in Pass 2)
   *   - Shadow Atlas (CSM + punctual, for forward/punctual lookup)
   *   - Light SSBO
   * Writes the final HDR radiance into combined_tx. */
  GPU_debug_group_begin("DeferredLighting");
  combined_fb.bind();
  GPU_framebuffer_clear_color(&combined_fb, float4(0.0f, 0.0f, 0.0f, 1.0f));
  inst_->manager->submit(opaque_layer_.lighting_ps_, view);
  GPU_debug_group_end();

  /* ================================================================
   * Pass 6: Transparent Forward + FSR Reactive Mask
   *
   * Snapshot the opaque-only color before drawing transparencies.
   * The pixel-wise delta between the snapshot and the post-transparency result
   * is the FSR Reactive Mask: pixels that changed significantly are marked
   * as "reactive" so FSR's temporal accumulation does not ghost them.
   *
   * This is the standard FSR integration pattern recommended by AMD. */
  GPU_debug_group_begin("Forward.Transparent");

  opaque_copy_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16);

  /* GPU_texture_copy(dst, src) */
  GPU_texture_copy(opaque_copy_tx_.get(), inst_->render_buffers.combined_tx.get());

  /* Render transparencies additively/blended over combined_tx */
  combined_fb.bind();
  inst_->manager->submit(transparent_layer_.shading_ps_, view);

  /* Generate the FSR 3.0 reactive and transparency/composition masks.
   * reactive_mask_tx : high values = fast-moving pixels (smoke, fire, alpha)
   * transp_mask_tx   : high values = composited/transparent elements (glass, UI) */
  inst_->upscale.generate_masks(
      opaque_copy_tx_.get(),
      inst_->render_buffers.combined_tx.get(),
      inst_->render_buffers.reactive_mask_tx.get(),
      inst_->render_buffers.transp_mask_tx.get());

  opaque_copy_tx_.release();

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

  const bool use_forward = (blender_mat->surface_render_method == MA_SURFACE_METHOD_FORWARD);

  switch (pipeline_type) {
    /* Depth + optional velocity prepass — deferred opaque */
    case MAT_PIPE_PREPASS_DEFERRED:
    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY: {
      PassMain::Sub &sub = opaque_layer_.prepass_ps.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* G-Buffer fill — deferred opaque surface shading */
    case MAT_PIPE_DEFERRED: {
      PassMain::Sub &sub = opaque_layer_.gbuffer_ps.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* Depth prepass for forward opaque/cutout */
    case MAT_PIPE_PREPASS_FORWARD:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY: {
      PassMain::Sub &sub = forward_layer_.prepass_ps.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* Forward opaque shading (writes directly to combined_tx, not G-Buffer) */
    case MAT_PIPE_FORWARD: {
      if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT)) {
        /* Transparent forward: per-object sub-passes via material_transparent_add() */
        return nullptr;
      }
      PassMain::Sub &sub = forward_layer_.shading_ps.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* Shadow atlas rendering */
    case MAT_PIPE_SHADOW: {
      PassMain::Sub &sub = shadow_ps_.sub(GPU_material_get_name(gpumat));
      sub.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL);
      sub.shader_set(GPU_material_get_shader(gpumat));
      return &sub;
    }
    /* Volumes and transparent overlap masking are handled per-object */
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
 * Transparent forward — per-object sub-passes for back-to-front sorting
 * ================================================================ */

PassMain::Sub *PipelineModule::prepass_transparent_add(Object *ob,
                                                        blender::Material *blender_mat,
                                                        GPUMaterial *gpumat)
{
  /* Each transparent object gets its own sub so the draw list can reorder them.
   * Name by object pointer for uniqueness within this frame. */
  char name[32];
  BLI_snprintf(name, sizeof(name), "TranspPrepass_%p", ob);
  PassMain::Sub &sub = forward_layer_.prepass_ps.sub(name);
  sub.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL);
  sub.shader_set(GPU_material_get_shader(gpumat));
  sub.material_set(*inst_->manager, gpumat, true, inst_->anisotropic_filtering);
  return &sub;
}

PassMain::Sub *PipelineModule::material_transparent_add(Object *ob,
                                                         blender::Material *blender_mat,
                                                         GPUMaterial *gpumat)
{
  char name[32];
  BLI_snprintf(name, sizeof(name), "Transp_%p", ob);
  PassMain::Sub &sub = forward_layer_.shading_ps.sub(name);
  /* Additive/alpha blend state — depth test without write */
  sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL |
                DRW_STATE_BLEND_ALPHA);
  sub.shader_set(GPU_material_get_shader(gpumat));
  sub.material_set(*inst_->manager, gpumat, true, inst_->anisotropic_filtering);
  return &sub;
}

} // namespace blender::eevee_game

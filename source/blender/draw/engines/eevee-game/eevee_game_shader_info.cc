/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * \file
 * \ingroup eevee_game
 *
 * ShaderCreateInfo registrations for all static shaders used by eevee_game.
 *
 * Naming convention:
 *   "eevee_game_*" — must match static_shader_name_get() in eevee_game_shader.cc.
 *
 * Two categories:
 *
 *   1. ALIASES — reuse an upstream EEVEE-Next ShaderCreateInfo unchanged.
 *      Used when the binding layout and shader logic are identical.
 *      Prefer aliases to avoid divergence from upstream.
 *
 *   2. NEW INFOS — dedicated GPU_SHADER_CREATE_INFO blocks.
 *      Used when the eevee_game pass has a different binding layout,
 *      GLSL source, or push-constant set from the EEVEE upstream equivalent.
 *
 * FIX log (relative to the previous alias-only version):
 *
 *   DoF:
 *     Was aliased to eevee_depth_of_field_{setup,gather,resolve} — those are
 *     the 10-pass EEVEE DoF pipeline with bokeh LUT, tile dilation, hole-fill,
 *     stabilise, etc.  Binding layouts are incompatible with our 3-pass fast DoF.
 *     Replaced with three proper GPU_SHADER_CREATE_INFO blocks pointing to
 *     eevee_game_dof_fast_comp.glsl with DOF_STAGE=0/1/2 defines.
 *
 *   SMAA:
 *     The three alias names were crossed (edge→neighborhood_blending,
 *     weight→blending_weight_calculation, blend→neighborhood_blending again).
 *     Even if the names were correct, the EEVEE effect_smaa_* infos use the
 *     Workbench vertex layout which carries different varyings from our SMAA vert.
 *     Replaced with three proper GPU_SHADER_CREATE_INFO blocks pointing to
 *     eevee_game_smaa_{vert,frag}.glsl with SMAA_STAGE=0/1/2 defines.
 *
 *   Bloom downsample / upsample:
 *     Were aliased to eevee_film_copy_frag — a plain texture blit with zero
 *     Kawase filtering or threshold logic.  Replaced with compute infos.
 *
 *   GTAO / SSGI:
 *     Were aliased to eevee_horizon_scan / eevee_horizon_resolve.  Those infos
 *     use EEVEE's tile-classify + sparse dispatch architecture with a different
 *     SSBO layout.  Our shaders use a simpler full-screen dispatch.
 *     Replaced with dedicated compute infos.
 *
 *   Volumetrics:
 *     Aliases to eevee_volume_scatter / eevee_volume_integration referenced
 *     EEVEE shaders that have additional uniforms (history textures, jitter,
 *     lightprobe samples) absent from our simpler game-mode pipeline.
 *     eevee_volume_resolve did not exist at all in some EEVEE-Next branches.
 *     All three replaced with dedicated compute infos.
 *
 *   FSR reactive mask:
 *     Was entirely absent.  Added new compute info.
 */

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Aliases to EEVEE-Next shaders (binding-compatible)
 * \{ */

/* Deferred lighting — binding layout identical to EEVEE single-sample path. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_deferred_light",   "eevee_deferred_light_single")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_deferred_combine", "eevee_deferred_combine")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_tile_classify",    "eevee_deferred_tile_classify")

/* G-Buffer fill — uses the standard deferred surface shader. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_gbuffer",          "eevee_surf_deferred")

/* Shadow atlas rasterisation — depth-only surface pass, reused verbatim. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_csm",        "eevee_surf_shadow")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_atlas",      "eevee_surf_shadow")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_depth_write","eevee_surf_depth")

/* Screen-space reflections — EEVEE Hi-Z trace, same binding layout. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_ssr_hiz_trace",    "eevee_ray_trace_screen")

/* Forward surface shading. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_forward",          "eevee_surf_forward")

/* Hi-Z pyramid update — identical algorithm and bindings. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_hiz_update",       "eevee_hiz_update")

/* Motion blur — not used in game loop; alias to a no-op blit so the
 * enum slot is valid and static_shader_get() does not crash. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_motion_blur_fast", "eevee_film_copy_frag")

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — Bloom
 * \{ */

/* Bloom Downsample
 *
 * Level 0: applies soft-knee luminance threshold then 13-tap Kawase downsample.
 * Level 1+: plain 13-tap Kawase (params.x < 0 bypasses threshold).
 *
 * params.x = threshold, params.y = knee (both < 0 → bypass).
 * Output half the input resolution each level. */
GPU_SHADER_CREATE_INFO("eevee_game_bloom_downsample")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "in_color_tx",   Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::Float2D, "out_color_img", Frequency::PASS)
    .push_constant(Type::VEC4, "params")
    .compute_source("eevee_game_bloom_downsample_comp.glsl")
    .do_static_compilation(true);

/* Bloom Upsample
 *
 * Tent-filter upsample: reads in_blur_tx (coarser level) and ADDITIVELY blends
 * into out_color_img (finer level, READ_WRITE).  Accumulation in-place avoids
 * an extra RGBA16F allocation per pyramid level.
 *
 * radius: tent filter spread in UV fractions of the output image. */
GPU_SHADER_CREATE_INFO("eevee_game_bloom_upsample")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "in_blur_tx",    Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::READ_WRITE, ImageType::Float2D, "out_color_img", Frequency::PASS)
    .push_constant(Type::FLOAT, "radius")
    .compute_source("eevee_game_bloom_upsample_comp.glsl")
    .do_static_compilation(true);

/* Bloom Composite
 *
 * Additively blends bloom_pyramid_[0] (half-res) onto combined_img (full-res)
 * in a single READ_WRITE compute pass — saves one full-res RGBA16F roundtrip. */
GPU_SHADER_CREATE_INFO("eevee_game_bloom_composite")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "bloom_tx",      Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::READ_WRITE, ImageType::Float2D, "combined_img", Frequency::PASS)
    .push_constant(Type::FLOAT, "bloom_intensity")
    .compute_source("eevee_game_bloom_composite_comp.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — Depth of Field (3-pass fast approximation)
 *
 * All three stages share a single GLSL source file.
 * The DOF_STAGE define selects the active code path at compile time.
 *
 * focus_params: vec4(focus_distance, f_stop, max_bokeh_radius_px, unused)
 * z_planes:     vec2(z_near, z_far)
 * \{ */

/* Stage 0 — Circle of Confusion setup.
 * One thread per full-res pixel.
 * Reads depth_tx, writes signed CoC in pixels to out_coc_img (R16F).
 *   positive CoC → background (behind focus plane)
 *   negative CoC → foreground (in front of focus plane) */
GPU_SHADER_CREATE_INFO("eevee_game_dof_coc_setup")
    .local_group_size(8, 8, 1)
    .define("DOF_STAGE", "0")
    .sampler(0, ImageType::Float2D, "depth_tx",      Frequency::PASS)
    .image(0, GPU_R16F, Qualifier::WRITE, ImageType::Float2D, "out_coc_img", Frequency::PASS)
    .push_constant(Type::VEC4,  "focus_params")
    .push_constant(Type::VEC2,  "z_planes")
    .compute_source("eevee_game_dof_fast_comp.glsl")
    .do_static_compilation(true);

/* Stage 1 — Bokeh blur (half-resolution).
 * One thread per half-res pixel.
 * 16-tap Poisson disk weighted by per-sample CoC magnitude.
 * Half-res reduces bokeh sample cost by 4×; bilateral resolve in stage 2
 * restores edge sharpness. */
GPU_SHADER_CREATE_INFO("eevee_game_dof_bokeh_blur")
    .local_group_size(8, 8, 1)
    .define("DOF_STAGE", "1")
    .sampler(0, ImageType::Float2D, "in_color_tx",   Frequency::PASS)
    .sampler(1, ImageType::Float2D, "coc_tx",        Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::Float2D, "out_bokeh_img", Frequency::PASS)
    .push_constant(Type::VEC4,  "focus_params")
    .push_constant(Type::VEC2,  "z_planes")
    .compute_source("eevee_game_dof_fast_comp.glsl")
    .do_static_compilation(true);

/* Stage 2 — Resolve (full-resolution bilateral composite).
 * One thread per full-res pixel.
 * Blends sharp and blurred buffers using CoC as the bilateral weight.
 * Foreground (negative CoC) gets a 1.5× weight boost to prevent
 * in-focus geometry from bleeding through foreground objects. */
GPU_SHADER_CREATE_INFO("eevee_game_dof_resolve")
    .local_group_size(8, 8, 1)
    .define("DOF_STAGE", "2")
    .sampler(0, ImageType::Float2D, "in_sharp_tx",   Frequency::PASS)
    .sampler(1, ImageType::Float2D, "in_bokeh_tx",   Frequency::PASS)
    .sampler(2, ImageType::Float2D, "coc_tx",        Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::Float2D, "out_final_img", Frequency::PASS)
    .push_constant(Type::VEC4,  "focus_params")
    .push_constant(Type::VEC2,  "z_planes")
    .compute_source("eevee_game_dof_fast_comp.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New shaders — SMAA (3-pass morphological AA)
 *
 * All three stages share source files eevee_game_smaa_{vert,frag}.glsl.
 * The SMAA_STAGE define selects the active code path.
 *
 * smaa_rt_metrics: vec4(1/W, 1/H, W, H) at render resolution.
 *
 * area_tx / search_tx: static 1D/2D LUT textures loaded once at init.
 * They are the standard SMAA area and search textures — identical to the
 * ones used by EEVEE's Workbench SMAA pass.
 * \{ */

/* Stage 0 — Luma edge detection.
 * Output: RG8 edge texture (R=horizontal, G=vertical edge).
 * Discards pixels with no edges to save the stage-1 bandwidth.
 * Uses luma in linear HDR space (Rec.709 coefficients, no sqrt approx). */
GPU_SHADER_CREATE_INFO("eevee_game_smaa_edge")
    .define("SMAA_STAGE", "0")
    .vertex_out_named("SmaaVaryings",
                      {Type::VEC2, "uvs"},
                      {Type::VEC2, "pixcoord"},
                      {Type::VEC4, "offset[3]"})
    .sampler(0, ImageType::Float2D, "color_tx",      Frequency::PASS)
    .push_constant(Type::VEC4,  "smaa_rt_metrics")
    .fragment_out(0, Type::VEC2,  "out_edges")
    .vertex_source("eevee_game_smaa_vert.glsl")
    .fragment_source("eevee_game_smaa_frag.glsl")
    .do_static_compilation(true);

/* Stage 1 — Blending weight calculation.
 * Reads the edge texture and the SMAA area / search LUTs.
 * Output: RGBA8 blend weight texture consumed by stage 2.
 * SMAA_MAX_SEARCH_STEPS=16 (preset HIGH). */
GPU_SHADER_CREATE_INFO("eevee_game_smaa_weight")
    .define("SMAA_STAGE", "1")
    .vertex_out_named("SmaaVaryings",
                      {Type::VEC2, "uvs"},
                      {Type::VEC2, "pixcoord"},
                      {Type::VEC4, "offset[3]"})
    .sampler(0, ImageType::Float2D, "edges_tx",      Frequency::PASS)
    .sampler(1, ImageType::Float2D, "area_tx",       Frequency::PASS)
    .sampler(2, ImageType::Float2D, "search_tx",     Frequency::PASS)
    .push_constant(Type::VEC4,  "smaa_rt_metrics")
    .fragment_out(0, Type::VEC4,  "out_weights")
    .vertex_source("eevee_game_smaa_vert.glsl")
    .fragment_source("eevee_game_smaa_frag.glsl")
    .do_static_compilation(true);

/* Stage 2 — Neighbourhood blending.
 * Reads source color and the blend weights from stage 1.
 * Performs the final anti-aliased blit.
 * Alpha is hard-clamped to 0/1 so game overlays remain compositable. */
GPU_SHADER_CREATE_INFO("eevee_game_smaa_blend")
    .define("SMAA_STAGE", "2")
    .vertex_out_named("SmaaVaryings",
                      {Type::VEC2, "uvs"},
                      {Type::VEC2, "pixcoord"},
                      {Type::VEC4, "offset[3]"})
    .sampler(0, ImageType::Float2D, "color_tx",      Frequency::PASS)
    .sampler(1, ImageType::Float2D, "blend_tx",      Frequency::PASS)
    .push_constant(Type::VEC4,  "smaa_rt_metrics")
    .fragment_out(0, Type::VEC4,  "out_color")
    .vertex_source("eevee_game_smaa_vert.glsl")
    .fragment_source("eevee_game_smaa_frag.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — GTAO (Ground Truth Ambient Occlusion)
 * \{ */

/* GTAO Main — horizon search at half resolution.
 * 4 directions × quality_steps taps per direction.
 * Per-pixel spatial-hash noise eliminates directional banding.
 * Output: R8 AO factor at half render resolution. */
GPU_SHADER_CREATE_INFO("eevee_game_gtao_main")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "depth_tx",      Frequency::PASS)
    .sampler(1, ImageType::Float2D, "normal_tx",     Frequency::PASS)
    .sampler(2, ImageType::Float2D, "hiz_tx",        Frequency::PASS)
    .image(0, GPU_R8, Qualifier::WRITE, ImageType::Float2D, "out_ao_img", Frequency::PASS)
    .push_constant(Type::FLOAT, "gtao_radius")
    .push_constant(Type::FLOAT, "gtao_falloff")
    .push_constant(Type::FLOAT, "gtao_intensity")
    .push_constant(Type::INT,   "gtao_quality_steps")
    .push_constant(Type::MAT4,  "viewprojinv")
    .push_constant(Type::VEC2,  "z_planes")
    .push_constant(Type::VEC2,  "screen_res")
    .compute_source("eevee_game_gtao_main_comp.glsl")
    .do_static_compilation(true);

/* GTAO Upsample — joint bilateral half→full resolution upscale.
 * Uses full-res depth as the cross-guide to prevent AO bleeding across
 * geometry edges (the primary artifact of naive bilinear upsampling). */
GPU_SHADER_CREATE_INFO("eevee_game_gtao_upsample")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "ao_lowres_tx",  Frequency::PASS)
    .sampler(1, ImageType::Float2D, "depth_tx",      Frequency::PASS)
    .image(0, GPU_R8, Qualifier::WRITE, ImageType::Float2D, "out_ao_final_img", Frequency::PASS)
    .push_constant(Type::VEC2,  "z_planes")
    .push_constant(Type::VEC2,  "screen_res")
    .compute_source("eevee_game_gtao_upsample_comp.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — SSGI (Screen Space Global Illumination)
 * \{ */

/* SSGI Main — Hi-Z accelerated hemisphere ray march at half resolution.
 * 4 cosine-weighted samples per pixel using a Halton + per-pixel rotation.
 * Edge-fade suppresses missing out-of-screen data near the viewport border. */
GPU_SHADER_CREATE_INFO("eevee_game_ssgi_main")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "scene_color_tx", Frequency::PASS)
    .sampler(1, ImageType::Float2D, "depth_tx",       Frequency::PASS)
    .sampler(2, ImageType::Float2D, "normal_tx",      Frequency::PASS)
    .sampler(3, ImageType::Float2D, "hiz_tx",         Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::Float2D, "out_ssgi_img", Frequency::PASS)
    .push_constant(Type::FLOAT, "ssgi_intensity")
    .push_constant(Type::FLOAT, "ssgi_radius")
    .push_constant(Type::FLOAT, "ssgi_color_saturation")
    .push_constant(Type::INT,   "ssgi_quality_steps")
    .push_constant(Type::MAT4,  "viewprojinv")
    .push_constant(Type::MAT4,  "viewproj")
    .push_constant(Type::VEC2,  "z_planes")
    .push_constant(Type::VEC2,  "screen_res")
    .push_constant(Type::UINT,  "frame_count")
    .compute_source("eevee_game_ssgi_main_comp.glsl")
    .do_static_compilation(true);

/* SSGI Bilateral Blur + Upsample — joint bilateral Gaussian 3×3.
 * Suppresses Halton sampling noise from the trace pass.
 * Uses full-res depth as the cross-guide (σ_depth = 0.15m). */
GPU_SHADER_CREATE_INFO("eevee_game_ssgi_blur")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "ssgi_lowres_tx", Frequency::PASS)
    .sampler(1, ImageType::Float2D, "depth_tx",       Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::Float2D, "out_ssgi_final_img", Frequency::PASS)
    .push_constant(Type::VEC2,  "z_planes")
    .push_constant(Type::VEC2,  "screen_res")
    .compute_source("eevee_game_ssgi_blur_comp.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — Volumetrics
 * \{ */

/* Volume Scatter — froxel light injection.
 *
 * Iterates over all lights in light_buf[] and injects in-scattered radiance
 * into each froxel using the Henyey-Greenstein phase function.
 * Directional (sun) shadows are resolved via the static CSM atlas.
 * Punctual light shadows are deferred to the resolve pass.
 *
 * The Z dimension is an inner loop (not a grid dimension) to keep all Z slices
 * for a given XY tile on one thread — cache-friendly sequential 3D reads.
 *
 * Output: RGBA16F 3D grid. RGB = σ_s × L_in, A = σ_t. */
GPU_SHADER_CREATE_INFO("eevee_game_volume_scatter")
    .local_group_size(8, 8, 1)
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::Float3D, "out_grid_img", Frequency::PASS)
    .sampler(0, ImageType::Float2DShadow, "shadow_atlas", Frequency::PASS)
    .storage_buf(0, Qualifier::READ, "LightData", "light_buf[]")
    .push_constant(Type::INT,   "tile_size")
    .push_constant(Type::INT,   "samples_z")
    .push_constant(Type::FLOAT, "vol_density")
    .push_constant(Type::FLOAT, "vol_anisotropy")
    .push_constant(Type::INT,   "light_count")
    .push_constant(Type::MAT4,  "viewmat")
    .push_constant(Type::MAT4,  "viewprojinv")
    .push_constant(Type::VEC3,  "camera_pos")
    .push_constant(Type::FLOAT, "z_near")
    .push_constant(Type::FLOAT, "z_far")
    .push_constant(Type::VEC2,  "screen_res")
    .push_constant(Type::MAT4,  "cascade_viewproj")
    .compute_source("eevee_game_volume_scatter_comp.glsl")
    .do_static_compilation(true);

/* Volume Integration — front-to-back Beer-Lambert Z accumulation.
 *
 * Each thread owns one (X, Y) froxel column and accumulates transmittance
 * and in-scattered radiance from near to far using the analytical
 * homogeneous-medium integral.  Sequential Z reads stay in L1 cache.
 *
 * Output: RGBA16F 3D texture. RGB = L(z), A = T(z) at each slice. */
GPU_SHADER_CREATE_INFO("eevee_game_volume_integrate")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float3D,  "in_grid_tx",         Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::Float3D, "out_integrated_img", Frequency::PASS)
    .push_constant(Type::FLOAT, "z_near")
    .push_constant(Type::FLOAT, "z_far")
    .push_constant(Type::INT,   "samples_z")
    .compute_source("eevee_game_volume_integrate_comp.glsl")
    .do_static_compilation(true);

/* Volume Resolve — froxel-to-screen 2D composite.
 *
 * One thread per screen pixel.
 * Converts pixel + NDC depth → froxel UVW using exact log-inverse of the
 * exponential Z spacing used in the scatter pass.
 * Samples the integrated 3D texture (trilinear) and composites:
 *   out_color = scene_color × T + L_inscatter
 * Modifies combined_img in-place (READ_WRITE) to avoid a copy. */
GPU_SHADER_CREATE_INFO("eevee_game_volume_resolve")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float3D,  "volume_integrated_tx", Frequency::PASS)
    .sampler(1, ImageType::Float2D,  "depth_tx",             Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::READ_WRITE, ImageType::Float2D, "out_color_img", Frequency::PASS)
    .push_constant(Type::FLOAT, "z_near")
    .push_constant(Type::FLOAT, "z_far")
    .push_constant(Type::INT,   "tile_size")
    .push_constant(Type::INT,   "samples_z")
    .push_constant(Type::VEC2,  "screen_res")
    .compute_source("eevee_game_volume_resolve_comp.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — Anti-Aliasing
 * \{ */

/* FXAA — Fast Approximate Anti-Aliasing (FXAA 3.11, quality preset 25).
 *
 * Single-pass fragment shader on a full-screen triangle.
 * Luma computed in scene-linear space (Rec.709) — no sqrt approximation.
 * Alpha channel is preserved to keep game overlay compositing intact.
 *
 * Uniforms drive quality/sharpness trade-off:
 *   fxaa_quality_subpix              — sub-pixel aliasing removal [0,1] (0.75 default)
 *   fxaa_quality_edge_threshold      — min contrast to trigger AA (0.166 default)
 *   fxaa_quality_edge_threshold_min  — dark area floor (0.0312 default) */
GPU_SHADER_CREATE_INFO("eevee_game_fxaa")
    .vertex_out(gpu::shader::ScreenVertIface())
    .sampler(0, ImageType::Float2D, "color_tx",                      Frequency::PASS)
    .push_constant(Type::FLOAT, "fxaa_quality_subpix")
    .push_constant(Type::FLOAT, "fxaa_quality_edge_threshold")
    .push_constant(Type::FLOAT, "fxaa_quality_edge_threshold_min")
    .fragment_out(0, Type::VEC4,  "out_color")
    .vertex_source("eevee_fullscreen_vert.glsl")
    .fragment_source("eevee_game_fxaa_frag.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — Shadow
 * \{ */

/* PCF 3×3 Shadow Mask — compute shader.
 *
 * Reads G-Buffer depth and normal, reconstructs world-space position,
 * projects into CSM shadow-atlas space, and runs a 3×3 contact-hardening
 * PCF filter.  Writes one R16F shadow factor per pixel into shadow_mask_img.
 *
 * The deferred lighting pass reads shadow_mask_tx as a plain sampler2D
 * (no shadow comparison there — the filter result is already in [0,1]).
 *
 * Binding layout (must match eevee_game_shadow_pcf_comp.glsl exactly):
 *   sampler 0 : depth_tx        (SFLOAT_32_DEPTH)
 *   sampler 1 : normal_tx       (RGBA16F, G-Buffer layer 0)
 *   sampler 2 : shadow_atlas    (D32F, compare mode ON — hardware PCF)
 *   image   0 : shadow_mask_img (R16F, WRITE) */
GPU_SHADER_CREATE_INFO("eevee_game_shadow_pcf_compute")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D,       "depth_tx",     Frequency::PASS)
    .sampler(1, ImageType::Float2D,       "normal_tx",    Frequency::PASS)
    .sampler(2, ImageType::Float2DShadow, "shadow_atlas", Frequency::PASS)
    .image(0, GPU_R16F, Qualifier::WRITE, ImageType::Float2D, "shadow_mask_img", Frequency::PASS)
    .push_constant(Type::MAT4,  "viewprojinv")
    .push_constant(Type::MAT4,  "cascade_viewproj[0]")
    .push_constant(Type::MAT4,  "cascade_viewproj[1]")
    .push_constant(Type::MAT4,  "cascade_viewproj[2]")
    .push_constant(Type::MAT4,  "cascade_viewproj[3]")
    .push_constant(Type::VEC4,  "cascade_splits")
    .push_constant(Type::FLOAT, "shadow_bias")
    .push_constant(Type::FLOAT, "pcf_offset_scale")
    .push_constant(Type::INT,   "shadow_map_res")
    .compute_source("eevee_game_shadow_pcf_comp.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — Culling
 * \{ */

/* GPU-Driven Culling — one thread per instance.
 *
 * Frustum test (Gribb-Hartmann planes from viewproj) + optional Hi-Z
 * occlusion test.  Survivors are compacted into visible_indices_buf[] via
 * a coherent atomic counter.
 *
 * FIX: indirect_draw_buf is READ-only — never written by this shader.
 *      Previously declared READ_WRITE which prevented the driver from
 *      skipping coherency tracking (measurable stall on AMD RDNA). */
GPU_SHADER_CREATE_INFO("eevee_game_culling_compute")
    .local_group_size(64, 1, 1)
    .storage_buf(0, Qualifier::READ,       "InstanceData",                "instance_data_buf[]")
    .storage_buf(1, Qualifier::READ,       "DrawElementsIndirectCommand", "indirect_draw_buf[]")
    .storage_buf(2, Qualifier::WRITE,      "uint",                        "visible_indices_buf[]")
    .storage_buf(3, Qualifier::READ_WRITE, "uint",                        "visible_count_buf")
    .sampler(0, ImageType::Float2D, "hiz_tx", Frequency::PASS)
    .push_constant(Type::INT,  "instance_count")
    .push_constant(Type::MAT4, "viewproj")
    .compute_source("eevee_game_culling_comp.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New compute shaders — FSR Reactive Mask Generation
 * \{ */

/* FSR Reactive Mask Generator.
 *
 * Produces the per-pixel reactive and transparency masks required by FSR3
 * for correct temporal reconstruction of temporally unstable pixels
 * (particles, alpha surfaces, refractions, animated emitters).
 *
 * Three classification heuristics, combined via max():
 *   a) Motion vector magnitude — fast-moving pixels relative to geometry.
 *   b) Stencil bits — STENCIL_REFRACTIVE / STENCIL_TRANSPARENT.
 *   c) Luma delta vs. history — pixels whose brightness changed significantly.
 *
 * Outputs: two R8 textures at render resolution.
 *   reactive_mask_img  — [0,1]: 0 = fully accumulated, 1 = fully reactive.
 *   transp_mask_img    — [0,1]: 0 = opaque, 1 = fully transparent.
 *
 * Only compiled when WITH_AMD_FSR3 is set; still registered so the info
 * name exists for GPU_shader_create_from_info_name() lookup even without FSR. */
GPU_SHADER_CREATE_INFO("eevee_game_fsr3_mask_gen")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "color_tx",         Frequency::PASS)
    .sampler(1, ImageType::Float2D, "vector_tx",        Frequency::PASS)
    .sampler(2, ImageType::Float2D, "color_history_tx", Frequency::PASS)
    .sampler(3, ImageType::Float2D, "stencil_tx",       Frequency::PASS)
    .image(0, GPU_R8, Qualifier::WRITE, ImageType::Float2D, "reactive_mask_img", Frequency::PASS)
    .image(1, GPU_R8, Qualifier::WRITE, ImageType::Float2D, "transp_mask_img",   Frequency::PASS)
    .push_constant(Type::FLOAT, "reactive_motion_threshold")
    .push_constant(Type::FLOAT, "reactive_luma_threshold")
    .push_constant(Type::FLOAT, "reactive_base")
    .compute_source("eevee_game_fsr3_mask_gen_comp.glsl")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name New shaders — Film
 * \{ */

/* Film Present — final blit to the viewport backbuffer.
 *
 * Applied after FSR3 upscaling (or after the last AA pass when FSR is off).
 * Order of operations:
 *   1. Exposure (linear multiply in scene-linear space — must precede tonemapping).
 *   2. AgX tonemapping (optional; skip for RenderDoc HDR capture).
 *   3. sRGB gamma encode (skip when the swapchain is a hardware sRGB surface).
 *
 * do_tonemapping / do_gamma_encode are packed as INT push constants for
 * GLSL ES 3.0 compatibility (no bool push constants in some backends). */
GPU_SHADER_CREATE_INFO("eevee_game_film_present")
    .vertex_out(gpu::shader::ScreenVertIface())
    .sampler(0, ImageType::Float2D, "color_tx",        Frequency::PASS)
    .push_constant(Type::FLOAT, "exposure")
    .push_constant(Type::INT,   "do_tonemapping")
    .push_constant(Type::INT,   "do_gamma_encode")
    .fragment_out(0, Type::VEC4,  "out_color")
    .vertex_source("eevee_fullscreen_vert.glsl")
    .fragment_source("eevee_game_film_present_frag.glsl")
    .do_static_compilation(true);

/** \} */

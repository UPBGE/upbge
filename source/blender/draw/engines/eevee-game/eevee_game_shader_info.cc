/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * \file
 * \ingroup eevee_game
 *
 * ShaderCreateInfo registrations for all static shaders used by eevee_game.
 *
 * Strategy:
 *   Most passes reuse EEVEE-Next ShaderCreateInfos verbatim via GPU_SHADER_CREATE_INFO_ALIAS.
 *   Shaders that are genuinely new get their own info block.
 *
 * Naming convention: "eevee_game_*" — must match static_shader_name_get() in eevee_game_shader.cc.
 */

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Aliases to EEVEE-Next shaders
 * \{ */

/* Deferred lighting */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_deferred_light",   "eevee_deferred_light_single")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_deferred_combine", "eevee_deferred_combine")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_tile_classify",    "eevee_deferred_tile_classify")

/* G-Buffer */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_gbuffer",          "eevee_surf_deferred")

/* Shadows */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_csm",       "eevee_surf_shadow")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_atlas",     "eevee_surf_shadow")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_pcss",      "eevee_surf_depth")

/* SSR */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_ssr_hiz_trace",    "eevee_ray_trace_screen")

/* GTAO */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_gtao_main",        "eevee_horizon_scan")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_gtao_upsample",    "eevee_horizon_denoise")

/* SSGI */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_ssgi_main",        "eevee_horizon_scan")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_ssgi_blur",        "eevee_horizon_resolve")

/* Bloom downsample/upsample — placeholder alias until dedicated GLSL is written.
 * The composite pass IS a proper new shader defined below. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_bloom_downsample", "eevee_film_copy_frag")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_bloom_upsample",   "eevee_film_copy_frag")

/* Volumetrics */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_volume_scatter",   "eevee_volume_scatter")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_volume_integrate", "eevee_volume_integration")
/* FIX: eevee_game_volume_resolve was entirely missing.
 * VolumeModule::resolve() calls static_shader_get(SH_VOLUME_RESOLVE) which maps here.
 * Without this alias, GPU_shader_create_from_info_name() returns nullptr → crash.
 *
 * TODO: verify that "eevee_volume_resolve" exists in the target EEVEE-Next branch.
 * If it does not, replace this alias with a GPU_SHADER_CREATE_INFO block pointing to
 * a new eevee_game_volume_resolve_comp.glsl. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_volume_resolve",   "eevee_volume_resolve")

/* Forward surface */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_forward",          "eevee_surf_forward")

/* SMAA — 3-pass rasterisation, reuses EEVEE effect shaders unchanged */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_edge",        "effect_smaa_edge_aa")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_weight",      "effect_smaa_neighborhood_blending")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_blend",       "effect_smaa_blending_weight_calculation")

/* Depth of Field */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_coc_setup",    "eevee_depth_of_field_setup")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_bokeh_blur",   "eevee_depth_of_field_gather_foreground_no_lut")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_resolve",      "eevee_depth_of_field_resolve_no_lut")

/* Motion blur — unused in game mode; alias to no-op so the enum slot is filled. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_motion_blur_fast", "eevee_film_copy_frag")

/* Hi-Z */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_hiz_update",       "eevee_hiz_update")

/** \} */

/* -------------------------------------------------------------------- */
/** \name New shaders specific to eevee_game
 * \{ */

/* Film present — final blit from HDR combined_tx to the viewport default framebuffer.
 *
 * Exposure is applied here, AFTER FSR3 upscaling. This means combined_tx always enters
 * FSR with linear HDR values and no pre-exposure baked in, so preExposure = 1.0f in
 * apply_fsr3() is correct and intentional. Do not change this ordering. */
GPU_SHADER_CREATE_INFO("eevee_game_film_present")
    .vertex_out(gpu::shader::ScreenVertIface())
    .sampler(0, ImageType::Float2D, "combined_tx", Frequency::PASS)
    .push_constant(Type::FLOAT, "exposure")
    .fragment_out(0, Type::VEC4, "fragColor")
    .vertex_source("eevee_fullscreen_vert.glsl")
    .fragment_source("eevee_game_film_present_frag.glsl")
    .do_static_compilation(true);

/* GPU-Driven Culling compute shader.
 *
 * SSBO layout (must match eevee_game_culling_comp.glsl binding declarations exactly):
 *   binding 0 (READ)       : InstanceData[]                — per-instance world matrix + AABB
 *   binding 1 (READ)       : DrawElementsIndirectCommand[] — CPU-filled draw arguments
 *   binding 2 (WRITE)      : uint visible_indices_buf[]    — surviving resource_ids (compact)
 *   binding 3 (READ_WRITE) : uint visible_count_buf        — atomic counter, reset to 0 per frame
 *
 * FIX: original info only declared bindings 0 and 1 → GL_LINK_ERROR.
 * FIX: indirect_draw_buf changed from READ_WRITE to READ — never written by this shader. */
GPU_SHADER_CREATE_INFO("eevee_game_culling_compute")
    .local_group_size(64, 1, 1)
    .storage_buf(0, Qualifier::READ,       "InstanceData",                "instance_data_buf[]")
    .storage_buf(1, Qualifier::READ,       "DrawElementsIndirectCommand", "indirect_draw_buf[]")
    .storage_buf(2, Qualifier::WRITE,      "uint",                        "visible_indices_buf[]")
    .storage_buf(3, Qualifier::READ_WRITE, "uint",                        "visible_count_buf")
    .push_constant(Type::INT,  "instance_count")
    .push_constant(Type::MAT4, "viewproj")
    .compute_source("eevee_game_culling_comp.glsl")
    .do_static_compilation(true);

/* FXAA — Fast Approximate Anti-Aliasing.
 *
 * FIX: was aliased to "eevee_film_frag" (EEVEE's film accumulation blit).
 * That shader has no edge detection or filtering logic. SH_FXAA would silently
 * produce an unfiltered copy of the source — identical to AA being disabled.
 *
 * eevee_game_fxaa_frag.glsl is a standard 3-sample FXAA implementation:
 *   - Samples 4 diagonal luma neighbours to detect contrast.
 *   - Computes an edge gradient direction.
 *   - Performs a two-stage directional blur along the edge.
 *   - Writes the result via imageStore (image2D RGBA16F, WRITE binding).
 *
 * The shader runs in the fragment stage on a full-screen triangle (3 procedural
 * vertices from eevee_fullscreen_vert.glsl). imageStore in the fragment stage is
 * valid under OpenGL 4.2 (GL_ARB_shader_image_load_store), which is Blender's
 * minimum desktop GL requirement.
 *
 * fxaa_params.x = luma contrast threshold  (edge detection cutoff, default 0.166)
 * fxaa_params.y = sub-pixel blend strength (default 0.125, higher = softer) */
GPU_SHADER_CREATE_INFO("eevee_game_fxaa")
    .vertex_out(gpu::shader::ScreenVertIface())
    .sampler(0, ImageType::Float2D,  "color_tx", Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::WRITE, ImageType::Float2D, "out_img", Frequency::PASS)
    .push_constant(Type::VEC4, "fxaa_params")
    .vertex_source("eevee_fullscreen_vert.glsl")
    .fragment_source("eevee_game_fxaa_frag.glsl")
    .do_static_compilation(true);

/* Bloom Composite — additively blends the bloom pyramid base level onto combined_tx.
 *
 * For each pixel: combined_img[p] += texture(bloom_tx, uv) * bloom_intensity
 *
 * Single READ_WRITE imageLoad/imageStore compute pass. No temporary texture needed:
 * saves ~8 MB at 1440p, ~16 MB at 4K compared to a ping-pong framebuffer approach. */
GPU_SHADER_CREATE_INFO("eevee_game_bloom_composite")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "bloom_tx", Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::READ_WRITE, ImageType::Float2D, "combined_img", Frequency::PASS)
    .push_constant(Type::FLOAT, "bloom_intensity")
    .compute_source("eevee_game_bloom_composite_comp.glsl")
    .do_static_compilation(true);

/* Deterministic 3x3 PCF Shadow Filter — compute shader.
 *
 * Runs as a full-screen compute dispatch after the shadow atlas is rendered
 * and before the deferred lighting pass. Reads the G-Buffer depth and normal
 * to reconstruct world-space position, projects into shadow-atlas space, and
 * writes one shadow factor (R16F) per pixel into shadow_mask_img.
 *
 * The lighting pass reads shadow_mask_tx as a plain sampler2D — no shadow
 * comparison needed there, the filter result is already in [0, 1].
 *
 * Binding layout (must match eevee_game_shadow_pcf_comp.glsl exactly):
 *   sampler 0 : depth_tx       — scene depth (SFLOAT_32_DEPTH)
 *   sampler 1 : normal_tx      — G-Buffer normals (RGBA16F, layer 0 of rp_color_tx)
 *   sampler 2 : shadow_atlas   — fixed 4096x4096 depth atlas, compare mode ON (D32F)
 *   image   0 : shadow_mask    — output R16F (shadow factor [0,1])
 *
 * Uniforms are bound via push_constant; cascade matrices + splits are uploaded
 * as individual MAT4/VEC4 constants (no UBO indirection, avoids binding point
 * allocation overhead for a small constant payload of 4 matrices + 1 vec4).
 *
 * Local group 8x8 = 64 threads: fills one NVIDIA warp-pair / one AMD wavefront. */
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

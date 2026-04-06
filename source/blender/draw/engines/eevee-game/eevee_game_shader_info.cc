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

/* Bloom downsample/upsample — aliased to film copy as placeholder until
 * dedicated GLSL is written. The composite pass IS a new shader (see below). */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_bloom_downsample", "eevee_film_copy_frag")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_bloom_upsample",   "eevee_film_copy_frag")

/* Volumetrics */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_volume_scatter",   "eevee_volume_scatter")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_volume_integrate", "eevee_volume_integration")
/* FIX: eevee_game_volume_resolve was completely missing from the original file.
 * VolumeModule::resolve() calls static_shader_get(SH_VOLUME_RESOLVE) which maps to this name.
 * Without this alias, GPU_shader_create_from_info_name() returns nullptr and
 * the resolve() pass crashes on its first dispatch.
 * eevee_volume_resolve is EEVEE's screen-space volumetric composite shader — exactly
 * what is needed here: it reads the integrated 3D texture and composites it onto 2D color. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_volume_resolve",   "eevee_volume_resolve")

/* Forward surface */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_forward",          "eevee_surf_forward")

/* Anti-Aliasing */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_edge",        "effect_smaa_edge_aa")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_weight",      "effect_smaa_neighborhood_blending")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_blend",       "effect_smaa_blending_weight_calculation")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_fxaa",             "eevee_film_frag")

/* Depth of Field */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_coc_setup",    "eevee_depth_of_field_setup")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_bokeh_blur",   "eevee_depth_of_field_gather_foreground_no_lut")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_resolve",      "eevee_depth_of_field_resolve_no_lut")

/* Motion blur — not used in game mode; alias to no-op so the enum slot is filled. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_motion_blur_fast", "eevee_film_copy_frag")

/* Hi-Z */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_hiz_update",       "eevee_hiz_update")

/** \} */

/* -------------------------------------------------------------------- */
/** \name New shaders specific to eevee_game
 * \{ */

/* Film present — final blit from HDR combined_tx to the viewport default framebuffer.
 * Applies exposure scalar. Simple full-screen quad with a single sampler. */
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
 * FIX 1: original info only declared bindings 0 and 1.  The GLSL declares 4 SSBOs.
 *         Mismatch → GL_LINK_ERROR or silent reads from unbound (zero) memory.
 * FIX 2: indirect_draw_buf changed from READ_WRITE to READ — this shader never writes it.
 *         READ_WRITE forces L2 coherence tracking on AMD/NVIDIA for no reason. */
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

/* Bloom Composite — additively blends the bloom pyramid base level onto combined_tx.
 *
 * Inputs:
 *   bloom_tx    : RGBA16F at render_res/2 (bilinear-sampled to full res).
 *   combined_img: RGBA16F at render_res, READ_WRITE (in-place add).
 *
 * For each pixel: combined_img[p] += texture(bloom_tx, uv) * bloom_intensity
 *
 * Single compute pass with imageLoad/imageStore avoids a temporary texture allocation
 * (saves ~8 MB at 1440p, ~16 MB at 4K) compared to a ping-pong framebuffer approach.
 *
 * This shader is new to eevee_game; EEVEE-Next does not have a standalone bloom composite. */
GPU_SHADER_CREATE_INFO("eevee_game_bloom_composite")
    .local_group_size(8, 8, 1)
    .sampler(0, ImageType::Float2D, "bloom_tx", Frequency::PASS)
    .image(0, GPU_RGBA16F, Qualifier::READ_WRITE, ImageType::Float2D, "combined_img", Frequency::PASS)
    .push_constant(Type::FLOAT, "bloom_intensity")
    .compute_source("eevee_game_bloom_composite_comp.glsl")
    .do_static_compilation(true);

/** \} */

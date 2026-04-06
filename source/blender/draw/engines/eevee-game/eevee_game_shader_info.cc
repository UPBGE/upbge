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
 *   This is correct because we use the same GLSL source files and the same pipeline state logic.
 *   Shaders that are genuinely new (GPU-Driven Culling, film present blit) get their own info.
 *
 * Naming convention: "eevee_game_*" — must match static_shader_name_get() in eevee_game_shader.cc.
 */

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Aliases to EEVEE-Next shaders
 *
 * GPU_SHADER_CREATE_INFO_ALIAS(new_name, existing_name) registers a new name
 * that resolves to an already-registered ShaderCreateInfo. This means no GLSL
 * duplication and no separate compilation — the GPU backend hands out the same
 * compiled shader object for both names.
 * \{ */

/* Deferred lighting — use EEVEE single-closure variant as default.
 * PipelineModule::material_add() switches between single/double/triple at runtime
 * by specializing via the CLOSURE_BIN_COUNT define set in MaterialModule. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_deferred_light",   "eevee_deferred_light_single")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_deferred_combine", "eevee_deferred_combine")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_tile_classify",    "eevee_deferred_tile_classify")

/* G-Buffer — reuse EEVEE surface deferred frag/vert pair */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_gbuffer",          "eevee_surf_deferred")

/* Shadows — use EEVEE shadow surface shader (writes moment/depth to atlas) */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_csm",       "eevee_surf_shadow")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_atlas",     "eevee_surf_shadow")
/* PCSS filter — EEVEE shadow lib handles this internally; no separate shader needed.
 * Map to the depth shader as a safe fallback. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_shadow_pcss",      "eevee_surf_depth")

/* SSR — EEVEE Hi-Z screen-space ray tracing */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_ssr_hiz_trace",    "eevee_ray_trace_screen")

/* GTAO — EEVEE horizon scan suite */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_gtao_main",        "eevee_horizon_scan")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_gtao_upsample",    "eevee_horizon_denoise")

/* SSGI — EEVEE horizon scan + resolve (SSGI reuses the same compute path as GTAO) */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_ssgi_main",        "eevee_horizon_scan")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_ssgi_blur",        "eevee_horizon_resolve")

/* Bloom — EEVEE does not ship a dedicated bloom shader.
 * We use the EEVEE film comp pass as a blit base and drive it with
 * push constants for threshold/knee. These will be proper new shaders
 * in eevee_game_bloom_infos.hh once GLSL is written.
 * For now alias to the film copy shader so the engine at least links. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_bloom_downsample", "eevee_film_copy_frag")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_bloom_upsample",   "eevee_film_copy_frag")

/* Volumetrics — exact EEVEE shaders */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_volume_scatter",   "eevee_volume_scatter")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_volume_integrate", "eevee_volume_integration")

/* Forward surface */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_forward",          "eevee_surf_forward")

/* Anti-Aliasing — SMAA and FXAA from the shared effect shaders */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_edge",        "effect_smaa_edge_aa")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_weight",      "effect_smaa_neighborhood_blending")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_smaa_blend",       "effect_smaa_blending_weight_calculation")
/* FXAA — EEVEE does not have a dedicated FXAA shader; use the film comp as a pass-through
 * until eevee_game_fxaa.glsl is written. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_fxaa",             "eevee_film_frag")

/* Depth of Field — map to EEVEE DoF setup + gather for the fast single-pass variant.
 * eevee_game only uses the setup + foreground gather + resolve path (no stabilize). */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_coc_setup",    "eevee_depth_of_field_setup")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_bokeh_blur",   "eevee_depth_of_field_gather_foreground_no_lut")
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_dof_resolve",      "eevee_depth_of_field_resolve_no_lut")

/* Motion blur — not used in game mode (FSR3 motion vectors replace it).
 * Alias to a safe no-op so the slot is filled. */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_motion_blur_fast", "eevee_film_copy_frag")

/* Hi-Z — exact EEVEE hierarchical-Z mip chain update */
GPU_SHADER_CREATE_INFO_ALIAS("eevee_game_hiz_update",       "eevee_hiz_update")

/** \} */

/* -------------------------------------------------------------------- */
/** \name New shaders specific to eevee_game
 * \{ */

/* Film present — final blit from HDR combined_tx to the viewport default framebuffer.
 * Applies exposure and optional gamma. Simple full-screen quad with a single sampler. */
GPU_SHADER_CREATE_INFO("eevee_game_film_present")
    .vertex_out(gpu::shader::ScreenVertIface())
    .sampler(0, ImageType::Float2D, "combined_tx", Frequency::PASS)
    .push_constant(Type::FLOAT, "exposure")
    .fragment_out(0, Type::VEC4, "fragColor")
    .vertex_source("eevee_fullscreen_vert.glsl")
    .fragment_source("eevee_game_film_present_frag.glsl")
    .do_static_compilation(true);

/* GPU-Driven Culling — compute shader that reads an instance SSBO (world matrix + AABB),
 * performs frustum culling against the current view, and writes a DrawElementsIndirect
 * buffer. One thread per instance.
 *
 * Uniforms:
 *   frustum_planes[6]  — 6 x float4 clip planes in world space
 *   instance_count     — total instances in the input SSBO
 *
 * SSBOs:
 *   instances_in[]     — InstanceData { mat4 world; vec4 aabb_min; vec4 aabb_max; }
 *   draw_indirect[]    — DrawElementsIndirectCommand (count, prim_count, first, base_vtx, base_inst)
 */
GPU_SHADER_CREATE_INFO("eevee_game_culling_compute")
    .local_group_size(64, 1, 1)
    .storage_buf(0, Qualifier::READ,  "InstanceData", "instances_in[]")
    .storage_buf(1, Qualifier::READ_WRITE, "DrawElementsIndirectCommand", "draw_indirect[]")
    .push_constant(Type::INT, "instance_count")
    .push_constant(Type::MAT4, "viewproj")
    .compute_source("eevee_game_culling_comp.glsl")
    .do_static_compilation(true);

/** \} */

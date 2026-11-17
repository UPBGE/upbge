/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Create infos for every static shader of eevee_game.
 *
 * Names must match ShaderModule::static_shader_name_get() (eevee_game_shader.cc).
 * Resource names must match the bind / push_constant calls of the module that
 * builds the pass; a name that exists on one side only is silently ignored.
 *
 * Not here on purpose:
 *  - Hi-Z update: EEVEE's `eevee_hiz_update` is used directly.
 *  - Geometry shaders: generated per material by EEVEE's codegen.
 *
 * Shaders that need a surface normal pull in `eevee_gbuffer_data` and decode
 * it with `eevee_gbuffer_read_lib.glsl`. That info owns sampler slots 16-18;
 * everything declared here stays below that.
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_view_infos.hh"
#  include "eevee_common_infos.hh" /* eevee_gbuffer_data */
#  include "eevee_sampling_infos.hh"
#  include "eevee_uniform_infos.hh"
#  include "eevee_velocity_infos.hh" /* eevee_velocity_camera */
#  include "gpu_shader_fullscreen_infos.hh"

#  include "eevee_game_shader_shared.hh"
#endif

#ifdef GLSL_CPP_STUBS
/* Per-variant defines, so the shader sources also parse as C++. */
#  define DOF_STAGE 0
#  define SMAA_STAGE 1
#  define SMAA_GLSL_3
#  define SMAA_PRESET_HIGH
#  define SMAA_RT_METRICS smaa_rt_metrics
#  define SMAA_LUMA_WEIGHT float4(0.2126f, 0.7152f, 0.0722f, 0.0f)
#endif

#include "draw_defines.hh"
#include "eevee_defines.hh" /* HIZ_MIP_COUNT */
#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Culling
 * \{ */

/* Same interface as `draw_visibility_compute` plus the Hi-Z inputs, so the
 * output bitmask can be consumed by `draw_command_generate` unchanged. */
GPU_SHADER_CREATE_INFO(eevee_game_culling_compute)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(DRW_VISIBILITY_GROUP_SIZE)
DEFINE_VALUE("DRW_VIEW_LEN", STRINGIFY(DRW_VIEW_MAX))
STORAGE_BUF(0, read, ObjectBounds, bounds_buf[])
STORAGE_BUF(1, read_write, uint, visibility_buf[])
SAMPLER(0, sampler2D, hiz_tx)
PUSH_CONSTANT(int, resource_len)
PUSH_CONSTANT(bool, use_occlusion)
PUSH_CONSTANT(float4x4, hiz_persmat)
PUSH_CONSTANT(float2, hiz_uv_scale)
COMPUTE_SOURCE("eevee_game_culling_comp.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lights
 * \{ */

/* Game light list and shadow faces, read by `eevee_game_light_lib.glsl`.
 * The slots are those EEVEE's `eevee_light_data` and `eevee_shadow_data`
 * reserve: the forward surface shaders get their material resources
 * allocated around EEVEE's forward interface, these slots are free there. */
GPU_SHADER_CREATE_INFO(eevee_game_light_data)
TYPEDEF_SOURCE("eevee_game_shader_shared.hh")
UNIFORM_BUF(SPHERE_PROBE_BUF_SLOT, GameLightingData, game_lighting_buf)
STORAGE_BUF(LIGHT_BUF_SLOT, read, LightData, light_buf[])
STORAGE_BUF(LIGHT_ZBIN_BUF_SLOT, read, uint, light_tile_mask_buf[])
STORAGE_BUF(LIGHT_TILE_BUF_SLOT, read, ShadowTileData, shadow_tile_buf[])
SAMPLER(SHADOW_TILEMAPS_TEX_SLOT, sampler2DShadow, shadow_static_atlas)
SAMPLER(SHADOW_ATLAS_TEX_SLOT, sampler2DShadow, shadow_dynamic_atlas)
/* World lighting: GGX pre-filtered radiance per mip and the L1 spherical
 * harmonics of the diffuse radiance, one float4 per channel. The slots of
 * EEVEE's own probe inputs. */
SAMPLER(SPHERE_PROBE_TEX_SLOT, samplerCube, world_radiance_tx)
STORAGE_BUF(IRRADIANCE_BRICK_BUF_SLOT, read, float4, world_sh_buf[])
GPU_SHADER_CREATE_END()

/* One thread per (screen tile, word of 32 lights): tests the bounding
 * sphere of each light against the tile frustum, clipped to the farthest
 * opaque depth of the tile. */
GPU_SHADER_CREATE_INFO(eevee_game_light_cull)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(64)
TYPEDEF_SOURCE("eevee_game_shader_shared.hh")
UNIFORM_BUF(0, GameLightingData, game_lighting_buf)
STORAGE_BUF(0, read, LightData, light_buf[])
STORAGE_BUF(1, write, uint, light_tile_mask_out_buf[])
/* Conventional depth, the pyramid keeps the farthest value. */
SAMPLER(0, sampler2D, hiz_tx)
COMPUTE_SOURCE("eevee_game_light_cull_comp.glsl")
ADDITIONAL_INFO(draw_view)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Forward surfaces
 * \{ */

/* Replaces `eevee_surf_forward` in the forward materials (blended render
 * method): EEVEE's generated node tree, lit by the game light loop. The
 * swap is done by ShaderModule::material_shader_get() after EEVEE's codegen
 * amended the create info. One output, premultiplied: radiance and
 * 1 - average transmittance. */
GPU_SHADER_CREATE_INFO(eevee_game_surf_forward)
DEFINE("MAT_FORWARD")
EARLY_FRAGMENT_TEST(true)
FRAGMENT_OUT(0, float4, out_radiance)
FRAGMENT_SOURCE("eevee_game_surf_forward_frag.glsl")
/* Integrated fog grid from the camera: in-scatter and transmittance (RGB).
 * The slots of EEVEE's own forward volume inputs. */
SAMPLER(VOLUME_SCATTERING_TEX_SLOT, sampler3D, volume_tx)
SAMPLER(VOLUME_TRANSMITTANCE_TEX_SLOT, sampler3D, volume_transmittance_tx)
ADDITIONAL_INFO(eevee_game_light_data)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(draw_view_culling)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred lighting
 * \{ */

/* Full-screen pass, additive over the emission already in combined_tx. */
GPU_SHADER_CREATE_INFO(eevee_game_deferred_light)
DO_STATIC_COMPILATION()
TYPEDEF_SOURCE("eevee_game_shader_shared.hh")
/* Mip 0 = opaque depth, conventional range. */
SAMPLER(0, sampler2D, hiz_tx)
SAMPLER(1, sampler2D, ao_tx)
SAMPLER(2, sampler2D, ssgi_tx)
SAMPLER(3, sampler2D, shadow_mask_tx)
PUSH_CONSTANT(int, use_ao)
PUSH_CONSTANT(int, use_ssgi)
FRAGMENT_OUT(0, float4, out_radiance)
FRAGMENT_SOURCE("eevee_game_deferred_light_frag.glsl")
ADDITIONAL_INFO(eevee_game_light_data)
ADDITIONAL_INFO(eevee_gbuffer_data)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(gpu_fullscreen)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow
 * \{ */

/* PCF 3x3 sun shadow mask. Reads depth + G-Buffer normal, writes one factor
 * per pixel. The atlas holds reversed depth (1 = near the light). */
GPU_SHADER_CREATE_INFO(eevee_game_shadow_pcf_compute)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2DDepth, depth_tx)
SAMPLER(1, sampler2DShadow, shadow_atlas)
ADDITIONAL_INFO(eevee_gbuffer_data)
IMAGE(0, SFLOAT_16, write, image2D, shadow_mask_img)
PUSH_CONSTANT(float4x4, viewprojinv)
PUSH_CONSTANT(float4x4, cascade_viewproj_0)
PUSH_CONSTANT(float4x4, cascade_viewproj_1)
PUSH_CONSTANT(float4x4, cascade_viewproj_2)
PUSH_CONSTANT(float4x4, cascade_viewproj_3)
PUSH_CONSTANT(float4, cascade_splits)
PUSH_CONSTANT(float, shadow_bias)
PUSH_CONSTANT(float, pcf_offset_scale)
PUSH_CONSTANT(int, shadow_map_res)
COMPUTE_SOURCE("eevee_game_shadow_pcf_comp.glsl")
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name GTAO
 * \{ */

/* Horizon search at half resolution. */
GPU_SHADER_CREATE_INFO(eevee_game_gtao_main)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2DDepth, depth_tx)
SAMPLER(1, sampler2D, hiz_tx)
ADDITIONAL_INFO(eevee_gbuffer_data)
IMAGE(0, UNORM_8, write, image2D, out_ao_img)
PUSH_CONSTANT(float, gtao_radius)
PUSH_CONSTANT(float, gtao_falloff)
PUSH_CONSTANT(float, gtao_intensity)
PUSH_CONSTANT(int, gtao_quality_steps)
PUSH_CONSTANT(float4x4, viewprojinv)
PUSH_CONSTANT(float2, z_planes)
PUSH_CONSTANT(float2, screen_res)
PUSH_CONSTANT(float2, hiz_uv_scale)
COMPUTE_SOURCE("eevee_game_gtao_main_comp.glsl")
GPU_SHADER_CREATE_END()

/* Joint bilateral half to full resolution upscale. */
GPU_SHADER_CREATE_INFO(eevee_game_gtao_upsample)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2D, ao_lowres_tx)
SAMPLER(1, sampler2DDepth, depth_tx)
IMAGE(0, UNORM_8, write, image2D, out_ao_final_img)
PUSH_CONSTANT(float2, z_planes)
PUSH_CONSTANT(float2, screen_res)
COMPUTE_SOURCE("eevee_game_gtao_upsample_comp.glsl")
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name SSGI
 * \{ */

/* Hi-Z accelerated hemisphere ray march at half resolution. */
GPU_SHADER_CREATE_INFO(eevee_game_ssgi_main)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2D, scene_color_tx)
SAMPLER(1, sampler2DDepth, depth_tx)
SAMPLER(2, sampler2D, hiz_tx)
ADDITIONAL_INFO(eevee_gbuffer_data)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, out_ssgi_img)
PUSH_CONSTANT(float, ssgi_intensity)
PUSH_CONSTANT(float, ssgi_radius)
PUSH_CONSTANT(float, ssgi_color_saturation)
PUSH_CONSTANT(int, ssgi_quality_steps)
PUSH_CONSTANT(float4x4, viewprojinv)
PUSH_CONSTANT(float4x4, viewproj)
PUSH_CONSTANT(float2, z_planes)
PUSH_CONSTANT(float2, screen_res)
PUSH_CONSTANT(int, frame_count)
PUSH_CONSTANT(float2, hiz_uv_scale)
COMPUTE_SOURCE("eevee_game_ssgi_main_comp.glsl")
GPU_SHADER_CREATE_END()

/* Joint bilateral 3x3 blur + upsample. */
GPU_SHADER_CREATE_INFO(eevee_game_ssgi_blur)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2D, ssgi_lowres_tx)
SAMPLER(1, sampler2DDepth, depth_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, out_ssgi_final_img)
PUSH_CONSTANT(float2, z_planes)
PUSH_CONSTANT(float2, screen_res)
COMPUTE_SOURCE("eevee_game_ssgi_blur_comp.glsl")
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bloom
 * \{ */

/* Level 0 applies the soft-knee threshold (params.xy), every level does a
 * 13 tap downsample. params.x < 0 bypasses the threshold. */
GPU_SHADER_CREATE_INFO(eevee_game_bloom_downsample)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2D, in_color_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, out_color_img)
PUSH_CONSTANT(float4, params)
COMPUTE_SOURCE("eevee_game_bloom_downsample_comp.glsl")
GPU_SHADER_CREATE_END()

/* Tent filter upsample, accumulated into the next finer level. */
GPU_SHADER_CREATE_INFO(eevee_game_bloom_upsample)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2D, in_blur_tx)
IMAGE(0, SFLOAT_16_16_16_16, read_write, image2D, out_color_img)
PUSH_CONSTANT(float, radius)
COMPUTE_SOURCE("eevee_game_bloom_upsample_comp.glsl")
GPU_SHADER_CREATE_END()

/* combined_img += bloom_tx * bloom_intensity, in place. */
GPU_SHADER_CREATE_INFO(eevee_game_bloom_composite)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2D, bloom_tx)
IMAGE(0, SFLOAT_16_16_16_16, read_write, image2D, combined_img)
PUSH_CONSTANT(float, bloom_intensity)
COMPUTE_SOURCE("eevee_game_bloom_composite_comp.glsl")
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Depth of Field (3 stage fast approximation)
 *
 * One source file, three variants selected by DOF_STAGE.
 * \{ */

GPU_SHADER_CREATE_INFO(eevee_game_dof_common)
LOCAL_GROUP_SIZE(8, 8)
PUSH_CONSTANT(float4, focus_params)
PUSH_CONSTANT(float2, z_planes)
COMPUTE_SOURCE("eevee_game_dof_fast_comp.glsl")
GPU_SHADER_CREATE_END()

/* Stage 0: circle of confusion from depth. */
GPU_SHADER_CREATE_INFO(eevee_game_dof_coc_setup)
DO_STATIC_COMPILATION()
DEFINE_VALUE("DOF_STAGE", "0")
SAMPLER(0, sampler2DDepth, depth_tx)
IMAGE(0, SFLOAT_16, write, image2D, out_coc_img)
ADDITIONAL_INFO(eevee_game_dof_common)
GPU_SHADER_CREATE_END()

/* Stage 1: bokeh gather at half resolution. */
GPU_SHADER_CREATE_INFO(eevee_game_dof_bokeh_blur)
DO_STATIC_COMPILATION()
DEFINE_VALUE("DOF_STAGE", "1")
SAMPLER(0, sampler2D, in_color_tx)
SAMPLER(1, sampler2D, coc_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, out_bokeh_img)
ADDITIONAL_INFO(eevee_game_dof_common)
GPU_SHADER_CREATE_END()

/* Stage 2: full resolution composite of sharp and blurred color. */
GPU_SHADER_CREATE_INFO(eevee_game_dof_resolve)
DO_STATIC_COMPILATION()
DEFINE_VALUE("DOF_STAGE", "2")
SAMPLER(0, sampler2D, in_sharp_tx)
SAMPLER(1, sampler2D, in_bokeh_tx)
SAMPLER(2, sampler2D, coc_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, out_final_img)
ADDITIONAL_INFO(eevee_game_dof_common)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volumetrics
 * \{ */

/* Froxel light injection. */
GPU_SHADER_CREATE_INFO(eevee_game_volume_scatter)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
TYPEDEF_SOURCE("eevee_game_shader_shared.hh")
IMAGE(0, SFLOAT_16_16_16_16, write, image3D, out_grid_img)
/* Extinction per channel: colored absorption tints what is behind. */
IMAGE(1, UFLOAT_11_11_10, write, image3D, out_extinction_img)
STORAGE_BUF(0, read, VolumeBoxData, volume_box_buf[])
PUSH_CONSTANT(int, volume_box_count)
/* Properties summed by the voxelized volume shaders (EEVEE's grids). Slots
 * around the light data ones. */
PUSH_CONSTANT(int, use_voxels)
SAMPLER(0, sampler3D, prop_scattering_tx)
SAMPLER(1, sampler3D, prop_extinction_tx)
SAMPLER(2, sampler3D, prop_emission_tx)
SAMPLER(3, sampler3D, prop_phase_tx)
SAMPLER(6, sampler3D, prop_phase_weight_tx)
/* Lights by screen tile and their shadows, as the surfaces. */
ADDITIONAL_INFO(eevee_game_light_data)
PUSH_CONSTANT(int, tile_size)
PUSH_CONSTANT(int, samples_z)
PUSH_CONSTANT(float4x4, viewmat)
PUSH_CONSTANT(float4x4, viewprojinv)
PUSH_CONSTANT(float3, camera_pos)
PUSH_CONSTANT(float, z_near)
PUSH_CONSTANT(float, z_far)
PUSH_CONSTANT(float2, screen_res)
COMPUTE_SOURCE("eevee_game_volume_scatter_comp.glsl")
GPU_SHADER_CREATE_END()

/* Front to back Beer-Lambert accumulation along Z. */
GPU_SHADER_CREATE_INFO(eevee_game_volume_integrate)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler3D, in_grid_tx)
SAMPLER(1, sampler3D, in_extinction_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image3D, out_integrated_img)
IMAGE(1, UFLOAT_11_11_10, write, image3D, out_transmittance_img)
PUSH_CONSTANT(float, z_near)
PUSH_CONSTANT(float, z_far)
PUSH_CONSTANT(int, samples_z)
COMPUTE_SOURCE("eevee_game_volume_integrate_comp.glsl")
GPU_SHADER_CREATE_END()

/* Froxel to screen composite, in place on the scene color. */
GPU_SHADER_CREATE_INFO(eevee_game_volume_resolve)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler3D, volume_integrated_tx)
SAMPLER(1, sampler2DDepth, depth_tx)
SAMPLER(2, sampler3D, volume_transmittance_tx)
IMAGE(0, SFLOAT_16_16_16_16, read_write, image2D, out_color_img)
PUSH_CONSTANT(float, z_near)
PUSH_CONSTANT(float, z_far)
PUSH_CONSTANT(int, tile_size)
PUSH_CONSTANT(int, samples_z)
PUSH_CONSTANT(float2, screen_res)
COMPUTE_SOURCE("eevee_game_volume_resolve_comp.glsl")
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Anti-Aliasing
 * \{ */

/* FXAA 3.11, single full-screen pass. */
GPU_SHADER_CREATE_INFO(eevee_game_fxaa)
DO_STATIC_COMPILATION()
SAMPLER(0, sampler2D, color_tx)
PUSH_CONSTANT(float, fxaa_quality_subpix)
PUSH_CONSTANT(float, fxaa_quality_edge_threshold)
PUSH_CONSTANT(float, fxaa_quality_edge_threshold_min)
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_SOURCE("eevee_game_fxaa_frag.glsl")
ADDITIONAL_INFO(gpu_fullscreen)
GPU_SHADER_CREATE_END()

/* SMAA: one vertex and one fragment source, three variants selected by
 * SMAA_STAGE. Varyings cannot be arrays, hence offset0..2. */
GPU_SHADER_INTERFACE_INFO(eevee_game_smaa_iface)
SMOOTH(float2, uvs)
SMOOTH(float2, pixcoord)
SMOOTH(float4, offset0)
SMOOTH(float4, offset1)
SMOOTH(float4, offset2)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(eevee_game_smaa_common)
DEFINE("SMAA_GLSL_3")
DEFINE("SMAA_PRESET_HIGH")
DEFINE_VALUE("SMAA_RT_METRICS", "smaa_rt_metrics")
DEFINE_VALUE("SMAA_LUMA_WEIGHT", "float4(0.2126f, 0.7152f, 0.0722f, 0.0f)")
VERTEX_OUT(eevee_game_smaa_iface)
/* float4(1/W, 1/H, W, H) */
PUSH_CONSTANT(float4, smaa_rt_metrics)
VERTEX_SOURCE("eevee_game_smaa_vert.glsl")
FRAGMENT_SOURCE("eevee_game_smaa_frag.glsl")
GPU_SHADER_CREATE_END()

/* Stage 0: luma edge detection. */
GPU_SHADER_CREATE_INFO(eevee_game_smaa_edge)
DO_STATIC_COMPILATION()
DEFINE_VALUE("SMAA_STAGE", "0")
SAMPLER(0, sampler2D, color_tx)
FRAGMENT_OUT(0, float2, out_edges)
ADDITIONAL_INFO(eevee_game_smaa_common)
GPU_SHADER_CREATE_END()

/* Stage 1: blending weight calculation. */
GPU_SHADER_CREATE_INFO(eevee_game_smaa_weight)
DO_STATIC_COMPILATION()
DEFINE_VALUE("SMAA_STAGE", "1")
SAMPLER(0, sampler2D, edges_tx)
SAMPLER(1, sampler2D, area_tx)
SAMPLER(2, sampler2D, search_tx)
FRAGMENT_OUT(0, float4, out_weights)
ADDITIONAL_INFO(eevee_game_smaa_common)
GPU_SHADER_CREATE_END()

/* Stage 2: neighbourhood blending. */
GPU_SHADER_CREATE_INFO(eevee_game_smaa_blend)
DO_STATIC_COMPILATION()
DEFINE_VALUE("SMAA_STAGE", "2")
SAMPLER(0, sampler2D, color_tx)
SAMPLER(1, sampler2D, blend_tx)
FRAGMENT_OUT(0, float4, out_color)
ADDITIONAL_INFO(eevee_game_smaa_common)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name World lighting
 * \{ */

/* Replaces `eevee_surf_world` in the world capture: the world radiance only,
 * none of EEVEE's probe, blur or render pass inputs. */
GPU_SHADER_CREATE_INFO(eevee_game_surf_world)
FRAGMENT_OUT(0, float4, out_background)
FRAGMENT_SOURCE("eevee_game_surf_world_frag.glsl")
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_utility_texture)
GPU_SHADER_CREATE_END()

/* L1 spherical harmonics of the captured radiance, one work group. */
GPU_SHADER_CREATE_INFO(eevee_game_world_sh)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(64)
TYPEDEF_SOURCE("eevee_game_shader_shared.hh")
SAMPLER(0, samplerCube, radiance_tx)
STORAGE_BUF(0, write, float4, out_sh_buf[])
PUSH_CONSTANT(float, sh_lod)
COMPUTE_SOURCE("eevee_game_world_sh_comp.glsl")
GPU_SHADER_CREATE_END()

/* GGX pre-filtering of one mip of the lighting cube map, faces in Z. */
GPU_SHADER_CREATE_INFO(eevee_game_world_prefilter)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8, 1)
TYPEDEF_SOURCE("eevee_game_shader_shared.hh")
SAMPLER(0, samplerCube, radiance_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2DArray, out_img)
PUSH_CONSTANT(float, roughness)
/* Solid angle of one texel of the source mip 0. */
PUSH_CONSTANT(float, texel_solid_angle)
COMPUTE_SOURCE("eevee_game_world_prefilter_comp.glsl")
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Camera motion
 * \{ */

/* Motion of the pixels the velocity prepass did not write (static geometry
 * and background), from depth and the previous / current cameras. */
GPU_SHADER_CREATE_INFO(eevee_game_velocity_resolve)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2DDepth, depth_tx)
IMAGE(0, SFLOAT_16_16, read_write, image2D, vector_img)
COMPUTE_SOURCE("eevee_game_velocity_resolve_comp.glsl")
ADDITIONAL_INFO(eevee_velocity_camera)
ADDITIONAL_INFO(draw_view)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name FSR reactive mask
 *
 * Fallback mask generator. Not referenced from C++ while the FSR3 SDK pass
 * (UpscaleModule::generate_masks) is used.
 * \{ */

GPU_SHADER_CREATE_INFO(eevee_game_fsr3_mask_gen)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8)
SAMPLER(0, sampler2D, color_tx)
SAMPLER(1, sampler2D, vector_tx)
SAMPLER(2, sampler2D, color_history_tx)
SAMPLER(3, sampler2D, stencil_tx)
IMAGE(0, UNORM_8, write, image2D, reactive_mask_img)
IMAGE(1, UNORM_8, write, image2D, transp_mask_img)
PUSH_CONSTANT(float, reactive_motion_threshold)
PUSH_CONSTANT(float, reactive_luma_threshold)
PUSH_CONSTANT(float, reactive_base)
COMPUTE_SOURCE("eevee_game_fsr3_mask_gen_comp.glsl")
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Film
 * \{ */

/* Final draw into the viewport: exposure, optional AgX, optional sRGB encode. */
GPU_SHADER_CREATE_INFO(eevee_game_film_present)
DO_STATIC_COMPILATION()
SAMPLER(0, sampler2D, color_tx)
PUSH_CONSTANT(float, exposure)
PUSH_CONSTANT(int, do_tonemapping)
PUSH_CONSTANT(int, do_gamma_encode)
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_SOURCE("eevee_game_film_present_frag.glsl")
ADDITIONAL_INFO(gpu_fullscreen)
GPU_SHADER_CREATE_END()

/** \} */

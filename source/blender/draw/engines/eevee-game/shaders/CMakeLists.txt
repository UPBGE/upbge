# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC_GLSL
  .
  ..

  # eevee_* library headers included by our shaders (phase function, shadow lib, etc.)
  ../../eevee/shaders

  ../../../intern
  ../../../intern/shaders

  ../../../../gpu
  ../../../../gpu/intern
  ../../../../gpu/shaders
  ../../../../gpu/shaders/common
  ../../../../gpu/shaders/infos

  # For variadic macros used by BLI_assert in GLSL stubs
  ../../../../blenlib
)

set(SRC_GLSL_VERT
  eevee_game_smaa_vert.glsl
)

set(SRC_GLSL_FRAG
  eevee_game_film_present_frag.glsl
  eevee_game_fxaa_frag.glsl
  eevee_game_smaa_frag.glsl
)

set(SRC_GLSL_COMP
  # Bloom -- Kawase pyramid build and HDR additive composite
  eevee_game_bloom_downsample_comp.glsl
  eevee_game_bloom_upsample_comp.glsl
  eevee_game_bloom_composite_comp.glsl

  # Depth of Field -- 3-stage fast approximation (DOF_STAGE=0/1/2)
  eevee_game_dof_fast_comp.glsl

  # Ambient Occlusion -- GTAO horizon search (half-res) + bilateral upsample
  eevee_game_gtao_main_comp.glsl
  eevee_game_gtao_upsample_comp.glsl

  # Screen-Space Global Illumination -- Hi-Z ray march (half-res) + bilateral blur
  eevee_game_ssgi_main_comp.glsl
  eevee_game_ssgi_blur_comp.glsl

  # Volumetrics -- froxel scatter, Beer-Lambert Z integration, screen resolve
  eevee_game_volume_scatter_comp.glsl
  eevee_game_volume_integrate_comp.glsl
  eevee_game_volume_resolve_comp.glsl

  # Shadows -- PCF 3x3 compute filter writing shadow_mask_tx
  eevee_game_shadow_pcf_comp.glsl

  # GPU-driven culling -- frustum + Hi-Z occlusion, indirect draw compaction
  eevee_game_culling_comp.glsl

  # FSR3 reactive / transparency mask generation
  eevee_game_fsr3_mask_gen_comp.glsl
)

if(WITH_GPU_SHADER_CPP_COMPILATION)
  compile_sources_as_cpp(eevee_game_cpp_shaders_vert "${SRC_GLSL_VERT}" "GPU_VERTEX_SHADER")
  compile_sources_as_cpp(eevee_game_cpp_shaders_frag "${SRC_GLSL_FRAG}" "GPU_FRAGMENT_SHADER")
  compile_sources_as_cpp(eevee_game_cpp_shaders_comp "${SRC_GLSL_COMP}" "GPU_COMPUTE_SHADER")
endif()

# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  ../eevee                          # eevee_material.hh, eevee_shader.hh, eevee_velocity_shared.hh, etc.
  ../../../gpu                      # GPU_texture.hh, GPU_material.hh, GPU_shader.hh
  ../../../gpu/intern               # gpu_shader_create_info.hh (for shader_info.cc)
  ../..                             # DRW_render.hh, draw_pass.hh, draw_handle.hh
  ../../intern                      # draw_cache.hh, draw_common.hh, draw_sculpt.hh
  ../../../../blenkernel            # BKE_material.hh, BKE_object.hh, BKE_image.hh, etc.
  ../../../../blenlib               # BLI_map.hh, BLI_vector.hh, BLI_task.hh, etc.
  ../../../../depsgraph             # DEG_depsgraph_query.hh
  ../../../../makesdna              # DNA_material_types.h, DNA_object_types.h, DNA_light_types.h
  ../../../../makesrna              # (for RNA registration hooks if needed)
  ../../../../render                # RE_pipeline.h
  ../../../../nodes                 # NOD_shader.h (used by MaterialModule constructor)
)

set(INC_SYS
)

set(SRC
  eevee_game_bloom.cc
  eevee_game_culling.cc
  eevee_game_depth_of_field.cc
  eevee_game_engine.cc
  eevee_game_film.cc
  eevee_game_gtao.cc
  eevee_game_instance.cc
  eevee_game_material.cc
  eevee_game_pipeline.cc
  eevee_game_raytrace.cc
  eevee_game_renderbuffers.cc
  eevee_game_shader.cc
  eevee_game_shader_info.cc
  eevee_game_shadow.cc
  eevee_game_ssgi.cc
  eevee_game_sync.cc
  eevee_game_upscaling.cc
  eevee_game_velocity.cc
  eevee_game_view.cc
  eevee_game_volume.cc

  eevee_game_bloom.hh
  eevee_game_culling.hh
  eevee_game_defines.hh
  eevee_game_depth_of_field.hh
  eevee_game_engine.h
  eevee_game_film.hh
  eevee_game_gtao.hh
  eevee_game_instance.hh
  eevee_game_material.hh
  eevee_game_pipeline.hh
  eevee_game_raytrace.hh
  eevee_game_renderbuffers.hh
  eevee_game_shader.hh
  eevee_game_shadow.hh
  eevee_game_ssgi.hh
  eevee_game_sync.hh
  eevee_game_upscaling.hh
  eevee_game_velocity.hh
  eevee_game_view.hh
  eevee_game_volume.hh

  # GLSL shaders — listed so CMake tracks them for incremental rebuilds.
  # The actual compilation happens via ShaderCreateInfo at runtime.
  shaders/eevee_game_bloom_composite_comp.glsl
  shaders/eevee_game_culling_comp.glsl
  shaders/eevee_game_film_present_frag.glsl
  shaders/eevee_game_fsr2_mask_gen.glsl
  shaders/eevee_game_fxaa_frag.glsl
  shaders/eevee_game_shadow_pcf_comp.glsl
  shaders/eevee_game_smaa_frag.glsl
)

set(LIB
  PRIVATE bf_blenkernel
  PRIVATE bf_blenlib
  PRIVATE bf_depsgraph
  PRIVATE bf_draw
  PRIVATE bf_gpu
  PRIVATE bf_nodes
  PRIVATE bf_render
  PRIVATE bf_rna
)

if(WITH_GAMEENGINE_FSR)
  list(APPEND INC_SYS
    ${AMD_FSR3_INCLUDE_DIRS}
  )
  list(APPEND LIB
    PRIVATE ${AMD_FSR3_LIBRARIES}
  )
  add_definitions(-DWITH_AMD_FSR3)
endif()

blender_add_lib(bf_draw_engine_eevee_game "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# Suppress warnings from EEVEE headers included transitively.
# EEVEE uses designated initialisers and C-style casts that trigger pedantic warnings.
target_compile_options(bf_draw_engine_eevee_game PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-old-style-cast -Wno-pedantic>
)

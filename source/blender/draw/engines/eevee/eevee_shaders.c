/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2016, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#include "DRW_render.h"

#include "BLI_string_utils.h"

#include "MEM_guardedalloc.h"

#include "GPU_shader.h"

#include "eevee_private.h"

static const char *filter_defines = "#define HAMMERSLEY_SIZE " STRINGIFY(HAMMERSLEY_SIZE) "\n"
#if defined(IRRADIANCE_SH_L2)
                               "#define IRRADIANCE_SH_L2\n"
#elif defined(IRRADIANCE_CUBEMAP)
                               "#define IRRADIANCE_CUBEMAP\n"
#elif defined(IRRADIANCE_HL2)
                               "#define IRRADIANCE_HL2\n"
#endif
                               "#define NOISE_SIZE 64\n";

static struct {
  /* Probes */
  struct GPUShader *probe_default_sh;
  struct GPUShader *probe_default_studiolight_sh;
  struct GPUShader *probe_background_studiolight_sh;
  struct GPUShader *probe_grid_display_sh;
  struct GPUShader *probe_cube_display_sh;
  struct GPUShader *probe_planar_display_sh;
  struct GPUShader *probe_filter_glossy_sh;
  struct GPUShader *probe_filter_diffuse_sh;
  struct GPUShader *probe_filter_visibility_sh;
  struct GPUShader *probe_grid_fill_sh;
  struct GPUShader *probe_planar_downsample_sh;

  /* Velocity Resolve */
  struct GPUShader *velocity_resolve_sh;

  /* Temporal Anti Aliasing */
  struct GPUShader *taa_resolve_sh;
  struct GPUShader *taa_resolve_reproject_sh;

  ///////////////////TRY
  struct GPUShader *aa_accum_sh;
  struct GPUShader *smaa_sh[3];
  struct DRWShaderLibrary *lib;

} e_data = {NULL}; /* Engine data */

extern char datatoc_bsdf_common_lib_glsl[];
extern char datatoc_bsdf_sampling_lib_glsl[];
extern char datatoc_common_uniforms_lib_glsl[];
extern char datatoc_common_view_lib_glsl[];

extern char datatoc_background_vert_glsl[];
extern char datatoc_default_world_frag_glsl[];
extern char datatoc_lightprobe_geom_glsl[];
extern char datatoc_lightprobe_vert_glsl[];
extern char datatoc_lightprobe_cube_display_frag_glsl[];
extern char datatoc_lightprobe_cube_display_vert_glsl[];
extern char datatoc_lightprobe_filter_diffuse_frag_glsl[];
extern char datatoc_lightprobe_filter_glossy_frag_glsl[];
extern char datatoc_lightprobe_filter_visibility_frag_glsl[];
extern char datatoc_lightprobe_grid_display_frag_glsl[];
extern char datatoc_lightprobe_grid_display_vert_glsl[];
extern char datatoc_lightprobe_grid_fill_frag_glsl[];
extern char datatoc_lightprobe_planar_display_frag_glsl[];
extern char datatoc_lightprobe_planar_display_vert_glsl[];
extern char datatoc_lightprobe_planar_downsample_frag_glsl[];
extern char datatoc_lightprobe_planar_downsample_geom_glsl[];
extern char datatoc_lightprobe_planar_downsample_vert_glsl[];
extern char datatoc_irradiance_lib_glsl[];
extern char datatoc_lightprobe_lib_glsl[];
extern char datatoc_octahedron_lib_glsl[];
extern char datatoc_cubemap_lib_glsl[];

/* Velocity Resolve */
extern char datatoc_effect_velocity_resolve_frag_glsl[];

/* Temporal Sampling */
extern char datatoc_effect_temporal_aa_glsl[];

/////TRY
extern char datatoc_common_smaa_lib_glsl[];
extern char datatoc_workbench_effect_taa_frag_glsl[];
extern char datatoc_workbench_effect_smaa_frag_glsl[];
extern char datatoc_workbench_effect_smaa_vert_glsl[];

void eevee_shader_library_ensure(void)
{
  if (e_data.lib == NULL) {
    e_data.lib = DRW_shader_library_create();
    /* NOTE: Theses needs to be ordered by dependencies. */
    DRW_SHADER_LIB_ADD(e_data.lib, common_view_lib);
    DRW_SHADER_LIB_ADD(e_data.lib, common_smaa_lib);
  }
}

/* *********** FUNCTIONS *********** */

void EEVEE_shaders_lightprobe_shaders_init(void)
{
  BLI_assert(e_data.probe_filter_glossy_sh == NULL);
  char *shader_str = NULL;

  shader_str = BLI_string_joinN(datatoc_common_view_lib_glsl,
                                datatoc_common_uniforms_lib_glsl,
                                datatoc_bsdf_common_lib_glsl,
                                datatoc_bsdf_sampling_lib_glsl,
                                datatoc_lightprobe_filter_glossy_frag_glsl);

  e_data.probe_filter_glossy_sh = DRW_shader_create(
      datatoc_lightprobe_vert_glsl, datatoc_lightprobe_geom_glsl, shader_str, filter_defines);

  e_data.probe_default_sh = DRW_shader_create_with_lib(datatoc_background_vert_glsl,
                                                       NULL,
                                                       datatoc_default_world_frag_glsl,
                                                       datatoc_common_view_lib_glsl,
                                                       NULL);

  MEM_freeN(shader_str);

  shader_str = BLI_string_joinN(datatoc_common_view_lib_glsl,
                                datatoc_common_uniforms_lib_glsl,
                                datatoc_bsdf_common_lib_glsl,
                                datatoc_bsdf_sampling_lib_glsl,
                                datatoc_lightprobe_filter_diffuse_frag_glsl);

  e_data.probe_filter_diffuse_sh = DRW_shader_create_fullscreen(shader_str, filter_defines);

  MEM_freeN(shader_str);

  shader_str = BLI_string_joinN(datatoc_common_view_lib_glsl,
                                datatoc_common_uniforms_lib_glsl,
                                datatoc_bsdf_common_lib_glsl,
                                datatoc_bsdf_sampling_lib_glsl,
                                datatoc_lightprobe_filter_visibility_frag_glsl);

  e_data.probe_filter_visibility_sh = DRW_shader_create_fullscreen(shader_str, filter_defines);

  MEM_freeN(shader_str);

  e_data.probe_grid_fill_sh = DRW_shader_create_fullscreen(datatoc_lightprobe_grid_fill_frag_glsl,
                                                           filter_defines);

  e_data.probe_planar_downsample_sh = DRW_shader_create(
      datatoc_lightprobe_planar_downsample_vert_glsl,
      datatoc_lightprobe_planar_downsample_geom_glsl,
      datatoc_lightprobe_planar_downsample_frag_glsl,
      NULL);
}

GPUShader *EEVEE_shaders_probe_filter_glossy_sh_get(void)
{
  return e_data.probe_filter_glossy_sh;
}

GPUShader *EEVEE_shaders_probe_default_sh_get(void)
{
  return e_data.probe_default_sh;
}

GPUShader *EEVEE_shaders_probe_filter_diffuse_sh_get(void)
{
  return e_data.probe_filter_diffuse_sh;
}

GPUShader *EEVEE_shaders_probe_filter_visibility_sh_get(void)
{
  return e_data.probe_filter_visibility_sh;
}

GPUShader *EEVEE_shaders_probe_grid_fill_sh_get(void)
{
  return e_data.probe_grid_fill_sh;
}

GPUShader *EEVEE_shaders_probe_planar_downsample_sh_get(void)
{
  return e_data.probe_planar_downsample_sh;
}

GPUShader *EEVEE_shaders_default_studiolight_sh_get(void)
{
  if (e_data.probe_default_studiolight_sh == NULL) {
    e_data.probe_default_studiolight_sh = DRW_shader_create_with_lib(
        datatoc_background_vert_glsl,
        NULL,
        datatoc_default_world_frag_glsl,
        datatoc_common_view_lib_glsl,
        "#define LOOKDEV\n");
  }
  return e_data.probe_default_studiolight_sh;
}

GPUShader *EEVEE_shaders_background_studiolight_sh_get(void)
{
  if (e_data.probe_background_studiolight_sh == NULL) {
    char *frag_str = BLI_string_joinN(datatoc_octahedron_lib_glsl,
                                      datatoc_cubemap_lib_glsl,
                                      datatoc_common_uniforms_lib_glsl,
                                      datatoc_bsdf_common_lib_glsl,
                                      datatoc_lightprobe_lib_glsl,
                                      datatoc_default_world_frag_glsl);

    e_data.probe_background_studiolight_sh = DRW_shader_create_with_lib(
        datatoc_background_vert_glsl,
        NULL,
        frag_str,
        datatoc_common_view_lib_glsl,
        "#define LOOKDEV_BG\n" SHADER_DEFINES);

    MEM_freeN(frag_str);
  }
  return e_data.probe_background_studiolight_sh;
}

GPUShader *EEVEE_shaders_probe_cube_display_sh_get(void)
{
  if (e_data.probe_cube_display_sh == NULL) {
    char *shader_str = BLI_string_joinN(datatoc_octahedron_lib_glsl,
                                        datatoc_cubemap_lib_glsl,
                                        datatoc_common_view_lib_glsl,
                                        datatoc_common_uniforms_lib_glsl,
                                        datatoc_bsdf_common_lib_glsl,
                                        datatoc_lightprobe_lib_glsl,
                                        datatoc_lightprobe_cube_display_frag_glsl);

    char *vert_str = BLI_string_joinN(datatoc_common_view_lib_glsl,
                                      datatoc_lightprobe_cube_display_vert_glsl);

    e_data.probe_cube_display_sh = DRW_shader_create(vert_str, NULL, shader_str, SHADER_DEFINES);

    MEM_freeN(vert_str);
    MEM_freeN(shader_str);
  }
  return e_data.probe_cube_display_sh;
}

GPUShader *EEVEE_shaders_probe_grid_display_sh_get(void)
{
  if (e_data.probe_grid_display_sh == NULL) {
    char *shader_str = BLI_string_joinN(datatoc_octahedron_lib_glsl,
                                        datatoc_cubemap_lib_glsl,
                                        datatoc_common_view_lib_glsl,
                                        datatoc_common_uniforms_lib_glsl,
                                        datatoc_bsdf_common_lib_glsl,
                                        datatoc_irradiance_lib_glsl,
                                        datatoc_lightprobe_lib_glsl,
                                        datatoc_lightprobe_grid_display_frag_glsl);

    char *vert_str = BLI_string_joinN(datatoc_common_view_lib_glsl,
                                      datatoc_lightprobe_grid_display_vert_glsl);

    e_data.probe_grid_display_sh = DRW_shader_create(vert_str, NULL, shader_str, filter_defines);

    MEM_freeN(vert_str);
    MEM_freeN(shader_str);
  }
  return e_data.probe_grid_display_sh;
}

GPUShader *EEVEE_shaders_probe_planar_display_sh_get(void)
{
  if (e_data.probe_planar_display_sh == NULL) {
    char *vert_str = BLI_string_joinN(datatoc_common_view_lib_glsl,
                                      datatoc_lightprobe_planar_display_vert_glsl);

    char *shader_str = BLI_string_joinN(datatoc_common_view_lib_glsl,
                                        datatoc_lightprobe_planar_display_frag_glsl);

    e_data.probe_planar_display_sh = DRW_shader_create(vert_str, NULL, shader_str, NULL);

    MEM_freeN(vert_str);
    MEM_freeN(shader_str);
  }
  return e_data.probe_planar_display_sh;
}

GPUShader *EEVEE_shaders_velocity_resolve_sh_get(void)
{
  if (e_data.velocity_resolve_sh == NULL) {
    char *frag_str = BLI_string_joinN(datatoc_common_uniforms_lib_glsl,
                                      datatoc_common_view_lib_glsl,
                                      datatoc_bsdf_common_lib_glsl,
                                      datatoc_effect_velocity_resolve_frag_glsl);

    e_data.velocity_resolve_sh = DRW_shader_create_fullscreen(frag_str, NULL);

    MEM_freeN(frag_str);
  }
  return e_data.velocity_resolve_sh;
}

GPUShader *EEVEE_shaders_taa_resolve_sh_get(EEVEE_EffectsFlag enabled_effects)
{
  GPUShader **sh;
  const char *define = NULL;
  if (enabled_effects & EFFECT_TAA_REPROJECT) {
    sh = &e_data.taa_resolve_reproject_sh;
    define = "#define USE_REPROJECTION\n";
  }
  else {
    sh = &e_data.taa_resolve_sh;
  }
  if (*sh == NULL) {
    char *frag_str = BLI_string_joinN(datatoc_common_uniforms_lib_glsl,
                                      datatoc_common_view_lib_glsl,
                                      datatoc_bsdf_common_lib_glsl,
                                      datatoc_effect_temporal_aa_glsl);

    *sh = DRW_shader_create_fullscreen(frag_str, define);
    MEM_freeN(frag_str);
  }

  return *sh;
}

////////////////////////TRY
GPUShader *eevee_shader_antialiasing_accumulation_get(void)
{
  if (e_data.aa_accum_sh == NULL) {
    char *frag = DRW_shader_library_create_shader_string(e_data.lib,
                                                         datatoc_workbench_effect_taa_frag_glsl);

    e_data.aa_accum_sh = DRW_shader_create_fullscreen(frag, NULL);

    MEM_freeN(frag);
  }
  return e_data.aa_accum_sh;
}

GPUShader *eevee_shader_antialiasing_get(int stage)
{
  BLI_assert(stage < 3);
  if (!e_data.smaa_sh[stage]) {
    char stage_define[32];
    BLI_snprintf(stage_define, sizeof(stage_define), "#define SMAA_STAGE %d\n", stage);

    e_data.smaa_sh[stage] = GPU_shader_create_from_arrays({
        .vert =
            (const char *[]){
                "#define SMAA_INCLUDE_VS 1\n",
                "#define SMAA_INCLUDE_PS 0\n",
                "uniform vec4 viewportMetrics;\n",
                datatoc_common_smaa_lib_glsl,
                datatoc_workbench_effect_smaa_vert_glsl,
                NULL,
            },
        .frag =
            (const char *[]){
                "#define SMAA_INCLUDE_VS 0\n",
                "#define SMAA_INCLUDE_PS 1\n",
                "uniform vec4 viewportMetrics;\n",
                datatoc_common_smaa_lib_glsl,
                datatoc_workbench_effect_smaa_frag_glsl,
                NULL,
            },
        .defs =
            (const char *[]){
                "#define SMAA_GLSL_3\n",
                "#define SMAA_RT_METRICS viewportMetrics\n",
                "#define SMAA_PRESET_HIGH\n",
                "#define SMAA_LUMA_WEIGHT float4(1.0, 1.0, 1.0, 1.0)\n",
                "#define SMAA_NO_DISCARD\n",
                stage_define,
                NULL,
            },
    });
  }
  return e_data.smaa_sh[stage];
}

void EEVEE_shaders_free(void)
{
  DRW_SHADER_FREE_SAFE(e_data.probe_default_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_filter_glossy_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_filter_diffuse_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_filter_visibility_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_grid_fill_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_planar_downsample_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_default_studiolight_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_background_studiolight_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_grid_display_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_cube_display_sh);
  DRW_SHADER_FREE_SAFE(e_data.probe_planar_display_sh);
  DRW_SHADER_FREE_SAFE(e_data.velocity_resolve_sh);
  DRW_SHADER_FREE_SAFE(e_data.taa_resolve_sh);
  DRW_SHADER_FREE_SAFE(e_data.taa_resolve_reproject_sh);

  //////////////TRY
  DRW_SHADER_FREE_SAFE(e_data.aa_accum_sh);

  for (int j = 0; j < sizeof(e_data.smaa_sh) / sizeof(void *); j++) {
    struct GPUShader **sh_array = &e_data.smaa_sh[0];
    DRW_SHADER_FREE_SAFE(sh_array[j]);
  }

  DRW_SHADER_LIB_FREE_SAFE(e_data.lib);
}

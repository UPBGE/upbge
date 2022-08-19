/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_view3d_defaults.h"

/* Struct members on own line. */
/* clang-format off */

/* -------------------------------------------------------------------- */
/** \name Scene Struct
 * \{ */

#define _DNA_DEFAULT_ImageFormatData \
  { \
    .planes = R_IMF_PLANES_RGBA, \
    .imtype = R_IMF_IMTYPE_PNG, \
    .depth = R_IMF_CHAN_DEPTH_8, \
    .quality = 90, \
    .compress = 15, \
  }

#define _DNA_DEFAULT_BakeData \
  { \
    .im_format = _DNA_DEFAULT_ImageFormatData, \
    .filepath = "//", \
    .flag = R_BAKE_CLEAR, \
    .pass_filter = R_BAKE_PASS_FILTER_ALL, \
    .width = 512, \
    .height = 512, \
    .margin = 16, \
    .margin_type = R_BAKE_ADJACENT_FACES, \
    .normal_space = R_BAKE_SPACE_TANGENT, \
    .normal_swizzle = {R_BAKE_POSX, R_BAKE_POSY, R_BAKE_POSZ}, \
  }

#define _DNA_DEFAULT_FFMpegCodecData \
  { \
    .audio_mixrate = 48000, \
    .audio_volume = 1.0f, \
    .audio_bitrate = 192, \
    .audio_channels = 2, \
  }

#define _DNA_DEFAULT_DisplaySafeAreas \
  { \
    .title = {10.0f / 100.0f, 5.0f / 100.0f}, \
    .action = {3.5f / 100.0f, 3.5f / 100.0f}, \
    .title_center = {17.5f / 100.0f, 5.0f / 100.0f}, \
    .action_center = {15.0f / 100.0f, 5.0f / 100.0f}, \
  }

#define _DNA_DEFAULT_RenderData \
  { \
    .mode = 0, \
    .cfra = 1, \
    .sfra = 1, \
    .efra = 250, \
    .frame_step = 1, \
    .xsch = 1920, \
    .ysch = 1080, \
    .xasp = 1, \
    .yasp = 1, \
    .tilex = 256, \
    .tiley = 256, \
    .size = 100, \
 \
    .im_format = _DNA_DEFAULT_ImageFormatData, \
 \
    .framapto = 100, \
    .images = 100, \
    .framelen = 1.0, \
    .blurfac = 0.5, \
    .frs_sec = 24, \
    .frs_sec_base = 1, \
 \
    /* OCIO_TODO: for forwards compatibility only, so if no tone-curve are used, \
     *            images would look in the same way as in current blender \
     * \
     *            perhaps at some point should be completely deprecated? \
     */ \
    .color_mgt_flag = R_COLOR_MANAGEMENT, \
 \
    .gauss = 1.5, \
    .dither_intensity = 1.0f, \
 \
    .bake_mode = 0, \
    .bake_margin = 16, \
    .bake_margin_type = R_BAKE_ADJACENT_FACES, \
    .bake_flag = R_BAKE_CLEAR, \
    .bake_samples = 256, \
    .bake_biasdist = 0.001f, \
 \
    /* BakeData */ \
    .bake = _DNA_DEFAULT_BakeData, \
 \
    .scemode = R_DOCOMP | R_DOSEQ | R_EXTENSION, \
 \
    .pic = "//", \
 \
    .stamp = R_STAMP_TIME | R_STAMP_FRAME | R_STAMP_DATE | R_STAMP_CAMERA | R_STAMP_SCENE | \
             R_STAMP_FILENAME | R_STAMP_RENDERTIME | R_STAMP_MEMORY, \
    .stamp_font_id = 12, \
    .fg_stamp = {0.8f, 0.8f, 0.8f, 1.0f}, \
    .bg_stamp = {0.0f, 0.0f, 0.0f, 0.25f}, \
 \
    .seq_prev_type = OB_SOLID, \
    .seq_rend_type = OB_SOLID, \
    .seq_flag = 0, \
 \
    .threads = 1, \
 \
    .simplify_subsurf = 6, \
    .simplify_particles = 1.0f, \
    .simplify_volumes = 1.0f, \
 \
    .border.xmin = 0.0f, \
    .border.ymin = 0.0f, \
    .border.xmax = 1.0f, \
    .border.ymax = 1.0f, \
 \
    .line_thickness_mode = R_LINE_THICKNESS_ABSOLUTE, \
    .unit_line_thickness = 1.0f, \
 \
    .ffcodecdata = _DNA_DEFAULT_FFMpegCodecData, \
  }

#define _DNA_DEFAULT_AudioData \
  { \
    .distance_model = 2.0f, \
    .doppler_factor = 1.0f, \
    .speed_of_sound = 343.3f, \
    .volume = 1.0f, \
    .flag = AUDIO_SYNC, \
  }

#define _DNA_DEFAULT_SceneDisplay \
  { \
    .light_direction = {M_SQRT1_3, M_SQRT1_3, M_SQRT1_3}, \
    .shadow_shift = 0.1f, \
    .shadow_focus = 0.0f, \
 \
    .matcap_ssao_distance = 0.2f, \
    .matcap_ssao_attenuation = 1.0f, \
    .matcap_ssao_samples = 16, \
 \
    .shading = _DNA_DEFAULT_View3DShading, \
 \
    .render_aa = SCE_DISPLAY_AA_SAMPLES_8, \
    .viewport_aa = SCE_DISPLAY_AA_FXAA, \
  }

#define _DNA_DEFAULT_PhysicsSettings \
  { \
    .gravity = {0.0f, 0.0f, -9.81f}, \
    .flag = PHYS_GLOBAL_GRAVITY, \
  }

#define _DNA_DEFAULT_RecastData \
  { \
    .cellsize = 0.3f, \
    .cellheight = 0.2f, \
    .agentmaxslope = M_PI_4, \
    .agentmaxclimb = 0.9f, \
    .agentheight = 2.0f, \
    .agentradius = 0.6f, \
    .edgemaxlen = 12.0f, \
    .edgemaxerror = 1.3f, \
    .regionminsize = 8.0f, \
    .regionmergesize = 20.0f, \
    .vertsperpoly = 6, \
    .detailsampledist = 6.0f, \
    .detailsamplemaxerror = 1.0f, \
    .partitioning = RC_PARTITION_WATERSHED, \
  }

#define _DNA_DEFAULT_GameData \
  { \
    .stereoflag = STEREO_NOSTEREO, \
    .stereomode = STEREO_ANAGLYPH, \
    .eyeseparation = 0.10, \
    .xplay = 1280, \
    .yplay = 720, \
    .samples_per_frame = 1, \
    .freqplay = 60, \
    .depth = 32, \
    .gravity = 9.8f, \
    .physicsEngine = WOPHY_BULLET, \
    .mode = WO_ACTIVITY_CULLING, \
    .occlusionRes = 128, \
    .ticrate = 60, \
    .maxlogicstep = 5, \
    .physubstep = 1, \
    .maxphystep = 5, \
    .timeScale = 1.0f, \
    .lineardeactthreshold = 0.8f, \
    .angulardeactthreshold = 1.0f, \
    .deactivationtime = 2.0f, \
    .erp = 0.2f, \
    .erp2 = 0.8f, \
    .cfm = 0.0f, \
    .obstacleSimulation = OBSTSIMULATION_NONE, \
    .levelHeight = 2.0f, \
    .exitkey = 218, \
    .flag = GAME_USE_UNDO, \
    .lodflag = SCE_LOD_USE_HYST, \
    .scehysteresis = 10, \
    .pythonkeys = {212, 217, 213, 116}, \
    .recastData = _DNA_DEFAULT_RecastData, \
  }

#define _DNA_DEFAULT_SceneEEVEE \
  { \
    .gi_diffuse_bounces = 3, \
    .gi_cubemap_resolution = 512, \
    .gi_visibility_resolution = 32, \
    .gi_cubemap_draw_size = 0.3f, \
    .gi_irradiance_draw_size = 0.1f, \
    .gi_irradiance_smoothing = 0.1f, \
    .gi_filter_quality = 3.0f, \
 \
    .taa_samples = 16, \
    .taa_render_samples = 64, \
 \
    .sss_samples = 7, \
    .sss_jitter_threshold = 0.3f, \
 \
    .ssr_quality = 0.25f, \
    .ssr_max_roughness = 0.5f, \
    .ssr_thickness = 0.2f, \
    .ssr_border_fade = 0.075f, \
    .ssr_firefly_fac = 10.0f, \
 \
    .volumetric_start = 0.1f, \
    .volumetric_end = 100.0f, \
    .volumetric_tile_size = 8, \
    .volumetric_samples = 64, \
    .volumetric_sample_distribution = 0.8f, \
    .volumetric_light_clamp = 0.0f, \
    .volumetric_shadow_samples = 16, \
 \
    .gtao_distance = 0.2f, \
    .gtao_factor = 1.0f, \
    .gtao_quality = 0.25f, \
 \
    .bokeh_overblur = 5.0f, \
    .bokeh_max_size = 100.0f, \
    .bokeh_threshold = 1.0f, \
    .bokeh_neighbor_max = 10.0f, \
    .bokeh_denoise_fac = 0.75f, \
 \
    .bloom_color = {1.0f, 1.0f, 1.0f}, \
    .bloom_threshold = 0.8f, \
    .bloom_knee = 0.5f, \
    .bloom_intensity = 0.05f, \
    .bloom_radius = 6.5f, \
    .bloom_clamp = 0.0f, \
 \
    .motion_blur_shutter = 0.5f, \
    .motion_blur_depth_scale = 100.0f, \
    .motion_blur_max = 32, \
    .motion_blur_steps = 1, \
 \
    .shadow_cube_size = 512, \
    .shadow_cascade_size = 1024, \
 \
    .light_cache_data = NULL, \
    .light_threshold = 0.01f, \
 \
    .overscan = 3.0f, \
 \
    .flag = SCE_EEVEE_VOLUMETRIC_LIGHTS | SCE_EEVEE_GTAO_BENT_NORMALS | \
                    SCE_EEVEE_GTAO_BOUNCE | SCE_EEVEE_TAA_REPROJECTION | \
                    SCE_EEVEE_SSR_HALF_RESOLUTION | SCE_EEVEE_SHADOW_SOFT, \
 \
    .smaa_quality = SCE_EEVEE_SMAA_PRESET_HIGH, \
    .smaa_predication_scale = 0.01, \
 \
  }

#define _DNA_DEFAULT_Scene \
  { \
    .cursor = _DNA_DEFAULT_View3DCursor, \
    .r = _DNA_DEFAULT_RenderData, \
    .audio = _DNA_DEFAULT_AudioData, \
 \
    .display = _DNA_DEFAULT_SceneDisplay, \
 \
    .physics_settings = _DNA_DEFAULT_PhysicsSettings, \
 \
    .safe_areas = _DNA_DEFAULT_DisplaySafeAreas, \
 \
    .eevee = _DNA_DEFAULT_SceneEEVEE, \
 \
    .gm = _DNA_DEFAULT_GameData, \
 \
  }

/** \} */

/* -------------------------------------------------------------------- */
/** \name ToolSettings Struct
 * \{ */

#define _DNA_DEFAULTS_CurvePaintSettings \
  { \
    .curve_type = CU_BEZIER, \
    .flag = CURVE_PAINT_FLAG_CORNERS_DETECT, \
    .error_threshold = 8, \
    .radius_max = 1.0f, \
    .corner_angle = DEG2RADF(70.0f), \
  }

#define _DNA_DEFAULTS_ImagePaintSettings \
  { \
    .paint.flags = PAINT_SHOW_BRUSH, \
    .normal_angle = 80, \
    .seam_bleed = 2, \
  }

#define _DNA_DEFAULTS_ParticleBrushData \
  { \
    .strength = 0.5f, \
    .size = 50, \
    .step = 10, \
    .count = 10, \
  }

#define _DNA_DEFAULTS_UnifiedPaintSettings \
  { \
    .size = 50, \
    .unprojected_radius = 0.29, \
    .alpha = 0.5f, \
    .weight = 0.5f, \
    .flag = UNIFIED_PAINT_SIZE | UNIFIED_PAINT_ALPHA, \
  }

#define _DNA_DEFAULTS_ParticleEditSettings \
  { \
    .flag = PE_KEEP_LENGTHS | PE_LOCK_FIRST | PE_DEFLECT_EMITTER | PE_AUTO_VELOCITY, \
    .emitterdist = 0.25f, \
    .totrekey = 5, \
    .totaddkey = 5, \
    .brushtype = PE_BRUSH_COMB, \
 \
    /* Scene init copies this to all other elements. */ \
    .brush = {_DNA_DEFAULTS_ParticleBrushData}, \
 \
    .draw_step = 2, \
    .fade_frames = 2, \
    .selectmode = SCE_SELECT_PATH, \
  }

#define _DNA_DEFAULTS_GP_Sculpt_Guide \
  { \
    .spacing = 20.0f, \
  }

#define _DNA_DEFAULTS_GP_Sculpt_Settings \
  { \
    .guide = _DNA_DEFAULTS_GP_Sculpt_Guide, \
  }

#define _DNA_DEFAULTS_MeshStatVis \
  { \
    .overhang_axis = OB_NEGZ, \
    .overhang_min = 0, \
    .overhang_max = DEG2RADF(45.0f), \
    .thickness_max = 0.1f, \
    .thickness_samples = 1, \
    .distort_min = DEG2RADF(5.0f), \
    .distort_max = DEG2RADF(45.0f), \
 \
    .sharp_min = DEG2RADF(90.0f), \
    .sharp_max = DEG2RADF(180.0f), \
  }

#define _DNA_DEFAULT_ToolSettings \
  { \
    .object_flag = SCE_OBJECT_MODE_LOCK, \
    .doublimit = 0.001, \
    .vgroup_weight = 1.0f, \
    .uvcalc_margin = 0.001f, \
    .uvcalc_flag = UVCALC_TRANSFORM_CORRECT_SLIDE, \
    .unwrapper = 1, \
    .select_thresh = 0.01f, \
 \
    .selectmode = SCE_SELECT_VERTEX, \
    .uv_selectmode = UV_SELECT_VERTEX, \
    .autokey_mode = AUTOKEY_MODE_NORMAL, \
 \
    .transform_pivot_point = V3D_AROUND_CENTER_MEDIAN, \
    .snap_mode = SCE_SNAP_MODE_INCREMENT, \
    .snap_node_mode = SCE_SNAP_MODE_GRID, \
    .snap_uv_mode = SCE_SNAP_MODE_INCREMENT, \
    .snap_flag = SCE_SNAP_TO_INCLUDE_EDITED | SCE_SNAP_TO_INCLUDE_NONEDITED, \
    .snap_transform_mode_flag = SCE_SNAP_TRANSFORM_MODE_TRANSLATE, \
    .snap_face_nearest_steps = 1, \
 \
    .curve_paint_settings = _DNA_DEFAULTS_CurvePaintSettings, \
 \
    .unified_paint_settings = _DNA_DEFAULTS_UnifiedPaintSettings, \
 \
    .statvis = _DNA_DEFAULTS_MeshStatVis, \
 \
    .proportional_size = 1.0f, \
 \
    .imapaint = _DNA_DEFAULTS_ImagePaintSettings, \
 \
    .particle = _DNA_DEFAULTS_ParticleEditSettings, \
 \
    .gp_sculpt = _DNA_DEFAULTS_GP_Sculpt_Settings, \
 \
    /* Annotations */ \
    .annotate_v3d_align = GP_PROJECT_VIEWSPACE | GP_PROJECT_CURSOR, \
    .annotate_thickness = 3, \
 \
    /* GP Stroke Placement */ \
    .gpencil_v3d_align = GP_PROJECT_VIEWSPACE, \
    .gpencil_v2d_align = GP_PROJECT_VIEWSPACE, \
 \
    /* UV painting */ \
    .uv_sculpt_settings = 0, \
    .uv_relax_method = UV_SCULPT_TOOL_RELAX_LAPLACIAN, \
  }

/* clang-format off */

/** \} */

/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_view3d_defaults.h"

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
    .exr_flag = R_IMF_EXR_FLAG_MULTIPART, \
  }

#define _DNA_DEFAULT_BakeData \
  { \
    .im_format = _DNA_DEFAULT_ImageFormatData, \
    .filepath = "//", \
    .type = R_BAKE_NORMALS, \
    .flag = R_BAKE_CLEAR, \
    .pass_filter = R_BAKE_PASS_FILTER_ALL, \
    .width = 512, \
    .height = 512, \
    .margin = 16, \
    .margin_type = R_BAKE_ADJACENT_FACES, \
    .normal_space = R_BAKE_SPACE_TANGENT, \
    .normal_swizzle = {R_BAKE_POSX, R_BAKE_POSY, R_BAKE_POSZ}, \
    .displacement_space = R_BAKE_SPACE_OBJECT, \
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
    .ppm_factor = 72.0f, \
    .ppm_base = 0.0254f, \
    .tilex = 256, \
    .tiley = 256, \
    .size = 100, \
 \
    .im_format = _DNA_DEFAULT_ImageFormatData, \
 \
    .framapto = 100, \
    .images = 100, \
    .framelen = 1.0, \
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
 \
    .motion_blur_shutter = 0.5f, \
\
    .compositor_denoise_final_quality = SCE_COMPOSITOR_DENOISE_HIGH, \
    .compositor_denoise_preview_quality = SCE_COMPOSITOR_DENOISE_BALANCED, \
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

#define _DNA_DEFAULT_RaytraceEEVEE \
  { \
    .flag = RAYTRACE_EEVEE_USE_DENOISE, \
    .denoise_stages = RAYTRACE_EEVEE_DENOISE_SPATIAL | \
                    RAYTRACE_EEVEE_DENOISE_TEMPORAL | \
                    RAYTRACE_EEVEE_DENOISE_BILATERAL, \
    .screen_trace_quality = 0.25f, \
    .screen_trace_thickness = 0.2f, \
    .trace_max_roughness = 0.5f, \
    .resolution_scale = 2, \
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
    .gi_irradiance_pool_size = 16, \
    .shadow_pool_size = 512, \
 \
    .taa_samples = 16, \
    .taa_render_samples = 64, \
 \
    .volumetric_start = 0.1f, \
    .volumetric_end = 100.0f, \
    .volumetric_tile_size = 8, \
    .volumetric_samples = 64, \
    .volumetric_sample_distribution = 0.8f, \
    .volumetric_ray_depth = 16, \
    .volumetric_light_clamp = 0.0f, \
    .volumetric_shadow_samples = 16, \
 \
    .fast_gi_bias = 0.05f, \
    .fast_gi_resolution = 2, \
    .fast_gi_step_count = 8, \
    .fast_gi_ray_count = 2, \
    .fast_gi_quality = 0.25f, \
    .fast_gi_distance = 0.0f, \
    .fast_gi_thickness_near = 0.25f, \
    .fast_gi_thickness_far = DEG2RAD(45), \
    .fast_gi_method = FAST_GI_FULL, \
 \
    .bokeh_overblur = 5.0f, \
    .bokeh_max_size = 100.0f, \
    .bokeh_threshold = 1.0f, \
    .bokeh_neighbor_max = 10.0f, \
 \
    .motion_blur_depth_scale = 100.0f, \
    .motion_blur_max = 32, \
    .motion_blur_steps = 1, \
 \
    .clamp_surface_indirect = 10.0f, \
\
    .shadow_ray_count = 1, \
    .shadow_step_count = 6, \
    .shadow_resolution_scale = 1.0f, \
 \
    .ray_tracing_method = RAYTRACE_EEVEE_METHOD_SCREEN, \
 \
    .ray_tracing_options = _DNA_DEFAULT_RaytraceEEVEE, \
 \
    .light_threshold = 0.01f, \
 \
    .overscan = 3.0f, \
 \
    .flag = SCE_EEVEE_TAA_REPROJECTION | SCE_EEVEE_SHADOW_ENABLED, \
  }

#define _DNA_DEFAULT_SceneGreasePencil \
  { \
    .smaa_threshold = 1.0f, \
    .smaa_threshold_render = 0.25f, \
    .aa_samples = 8, \
    .motion_blur_steps = 8, \
  }

#define _DNA_DEFAULT_SceneHydra \
  { \
    .export_method = SCE_HYDRA_EXPORT_HYDRA, \
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
    .grease_pencil_settings = _DNA_DEFAULT_SceneGreasePencil, \
 \
    .hydra = _DNA_DEFAULT_SceneHydra, \
    .simulation_frame_start = 1, \
    .simulation_frame_end = 250, \
 \
    .gm = _DNA_DEFAULT_GameData, \
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
    .paint = { \
      .flags = PAINT_SHOW_BRUSH, \
      .unified_paint_settings = _DNA_DEFAULTS_UnifiedPaintSettings, \
    }, \
    .normal_angle = 80, \
    .seam_bleed = 2, \
    .clone_alpha = 0.5f, \
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
    .size = 100, \
    .input_samples = 1, \
    .unprojected_size = 0.58, \
    .alpha = 0.5f, \
    .weight = 0.5f, \
    .color = {0.0f, 0.0f, 0.0f}, \
    .secondary_color = {1.0f, 1.0f, 1.0f}, \
    .rgb = {0.0f, 0.0f, 0.0f}, \
    .secondary_rgb = {1.0f, 1.0f, 1.0f}, \
    .flag = UNIFIED_PAINT_SIZE | UNIFIED_PAINT_COLOR, \
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

#define _DNA_DEFAULTS_UvSculpt \
  { \
    .size = 100, \
    .strength = 1.0f, \
    .curve_distance_falloff_preset = BRUSH_CURVE_SMOOTH, \
  }

#define _DNA_DEFAULT_ToolSettings \
  { \
    .object_flag = SCE_OBJECT_MODE_LOCK, \
    .doublimit = 0.001, \
    .vgroup_weight = 1.0f, \
 \
    .uvcalc_margin = 0.001f, \
    .uvcalc_flag = UVCALC_TRANSFORM_CORRECT_SLIDE, \
    .unwrapper = UVCALC_UNWRAP_METHOD_CONFORMAL, \
    .uvcalc_iterations = 10, \
    /* See struct member doc-string regarding this name. */ \
    .uvcalc_weight_group = "uv_importance", \
    .uvcalc_weight_factor = 1.0, \
 \
    .select_thresh = 0.01f, \
 \
    .selectmode = SCE_SELECT_VERTEX, \
    .uv_selectmode = UV_SELECT_VERT, \
    .autokey_mode = AUTOKEY_MODE_NORMAL, \
 \
    .transform_pivot_point = V3D_AROUND_CENTER_MEDIAN, \
    .snap_mode = SCE_SNAP_TO_INCREMENT, \
    .snap_node_mode = SCE_SNAP_TO_GRID, \
    .snap_uv_mode = SCE_SNAP_TO_INCREMENT, \
    .snap_anim_mode = SCE_SNAP_TO_FRAME, \
    .snap_playhead_mode = SCE_SNAP_TO_KEYS | SCE_SNAP_TO_STRIPS, \
    .snap_step_frames = 2, \
    .snap_step_seconds = 1, \
    .playhead_snap_distance = 20, \
    .snap_flag = SCE_SNAP_TO_INCLUDE_EDITED | SCE_SNAP_TO_INCLUDE_NONEDITED, \
    .snap_flag_anim = SCE_SNAP, \
    .snap_flag_playhead = 0, \
    .snap_transform_mode_flag = SCE_SNAP_TRANSFORM_MODE_TRANSLATE, \
    .snap_face_nearest_steps = 1, \
    .snap_angle_increment_3d = DEG2RADF(5.0f), \
    .snap_angle_increment_2d = DEG2RADF(5.0f), \
    .snap_angle_increment_3d_precision = DEG2RADF(1.0f), \
    .snap_angle_increment_2d_precision = DEG2RADF(1.0f), \
 \
    .snap_flag_seq = SCE_SNAP, \
    /* Weight Paint */ \
    .weightuser = OB_DRAW_GROUPUSER_ACTIVE, \
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
    .uvsculpt = _DNA_DEFAULTS_UvSculpt, \
    .uv_sculpt_settings = 0, \
 \
    /* Placement */ \
    .snap_mode_tools = SCE_SNAP_TO_GEOM,\
    .plane_axis = 2,\
\
    /* Animation */ \
    .fix_to_cam_flag = FIX_TO_CAM_FLAG_USE_LOC | FIX_TO_CAM_FLAG_USE_ROT | FIX_TO_CAM_FLAG_USE_SCALE, \
  }

#define _DNA_DEFAULT_Sculpt \
  { \
    .detail_size = 12,\
    .detail_percent = 25,\
    .constant_detail = 3.0f,\
    .automasking_start_normal_limit = 0.34906585f, /* 20 / 180 * pi. */ \
    .automasking_start_normal_falloff = 0.25f, \
    .automasking_view_normal_limit = 1.570796, /* 0.5 * pi. */ \
    .automasking_view_normal_falloff = 0.25f, \
    .automasking_boundary_edges_propagation_steps = 1, \
    .flags = SCULPT_DYNTOPO_SUBDIVIDE | SCULPT_DYNTOPO_COLLAPSE,\
    .paint = {\
      .unified_paint_settings = _DNA_DEFAULTS_UnifiedPaintSettings, \
      .symmetry_flags = PAINT_SYMMETRY_FEATHER,\
      .tile_offset = {1.0f, 1.0f, 1.0f},\
    }\
  }
/* clang-format off */

/** \} */

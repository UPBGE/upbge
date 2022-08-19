/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_hash.h"
#include "BLI_math.h"
#include "BLI_math_color_blend.h"
#include "BLI_task.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_brush.h"
#include "BKE_colorband.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"

#include "IMB_colormanagement.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_toolsystem.h"
#include "WM_types.h"

#include "ED_object.h"
#include "ED_screen.h"
#include "ED_sculpt.h"
#include "paint_intern.h"
#include "sculpt_intern.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "UI_interface.h"

#include "IMB_imbuf.h"

#include "bmesh.h"

#include <math.h>
#include <stdlib.h>

static void do_color_smooth_task_cb_exec(void *__restrict userdata,
                                         const int n,
                                         const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const Brush *brush = data->brush;
  const float bstrength = ss->cache->bstrength;

  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, &test, data->brush->falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(tls);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (!sculpt_brush_test_sq_fn(&test, vd.co)) {
      continue;
    }
    const float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                                brush,
                                                                vd.co,
                                                                sqrtf(test.dist),
                                                                vd.no,
                                                                vd.fno,
                                                                vd.mask ? *vd.mask : 0.0f,
                                                                vd.vertex,
                                                                thread_id);

    float smooth_color[4];
    SCULPT_neighbor_color_average(ss, smooth_color, vd.vertex);
    float col[4];

    SCULPT_vertex_color_get(ss, vd.vertex, col);
    blend_color_interpolate_float(col, col, smooth_color, fade);
    SCULPT_vertex_color_set(ss, vd.vertex, col);
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_paint_brush_task_cb_ex(void *__restrict userdata,
                                      const int n,
                                      const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const Brush *brush = data->brush;
  const float bstrength = fabsf(ss->cache->bstrength);

  PBVHVertexIter vd;
  PBVHColorBufferNode *color_buffer;

  SculptOrigVertData orig_data;
  SCULPT_orig_vert_data_init(&orig_data, data->ob, data->nodes[n], SCULPT_UNDO_COLOR);

  color_buffer = BKE_pbvh_node_color_buffer_get(data->nodes[n]);

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, &test, data->brush->falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(tls);

  float brush_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  copy_v3_v3(brush_color,
             ss->cache->invert ? BKE_brush_secondary_color_get(ss->scene, brush) :
                                 BKE_brush_color_get(ss->scene, brush));

  IMB_colormanagement_srgb_to_scene_linear_v3(brush_color, brush_color);

  if (brush->flag & BRUSH_USE_GRADIENT) {
    switch (brush->gradient_stroke_mode) {
      case BRUSH_GRADIENT_PRESSURE:
        BKE_colorband_evaluate(brush->gradient, ss->cache->pressure, brush_color);
        break;
      case BRUSH_GRADIENT_SPACING_REPEAT: {
        float coord = fmod(ss->cache->stroke_distance / brush->gradient_spacing, 1.0);
        BKE_colorband_evaluate(brush->gradient, coord, brush_color);
        break;
      }
      case BRUSH_GRADIENT_SPACING_CLAMP: {
        BKE_colorband_evaluate(
            brush->gradient, ss->cache->stroke_distance / brush->gradient_spacing, brush_color);
        break;
      }
    }
  }

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    SCULPT_orig_vert_data_update(&orig_data, &vd);

    bool affect_vertex = false;
    float distance_to_stroke_location = 0.0f;
    if (brush->tip_roundness < 1.0f) {
      affect_vertex = SCULPT_brush_test_cube(&test, vd.co, data->mat, brush->tip_roundness);
      distance_to_stroke_location = ss->cache->radius * test.dist;
    }
    else {
      affect_vertex = sculpt_brush_test_sq_fn(&test, vd.co);
      distance_to_stroke_location = sqrtf(test.dist);
    }

    if (!affect_vertex) {
      continue;
    }

    float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                          brush,
                                                          vd.co,
                                                          distance_to_stroke_location,
                                                          vd.no,
                                                          vd.fno,
                                                          vd.mask ? *vd.mask : 0.0f,
                                                          vd.vertex,
                                                          thread_id);

    /* Density. */
    float noise = 1.0f;
    const float density = ss->cache->paint_brush.density;
    if (density < 1.0f) {
      const float hash_noise = (float)BLI_hash_int_01(ss->cache->density_seed * 1000 * vd.index);
      if (hash_noise > density) {
        noise = density * hash_noise;
        fade = fade * noise;
      }
    }

    /* Brush paint color, brush test falloff and flow. */
    float paint_color[4];
    float wet_mix_color[4];
    float buffer_color[4];

    mul_v4_v4fl(paint_color, brush_color, fade * ss->cache->paint_brush.flow);
    mul_v4_v4fl(wet_mix_color, data->wet_mix_sampled_color, fade * ss->cache->paint_brush.flow);

    /* Interpolate with the wet_mix color for wet paint mixing. */
    blend_color_interpolate_float(
        paint_color, paint_color, wet_mix_color, ss->cache->paint_brush.wet_mix);
    blend_color_mix_float(color_buffer->color[vd.i], color_buffer->color[vd.i], paint_color);

    /* Final mix over the original color using brush alpha. */
    mul_v4_v4fl(buffer_color, color_buffer->color[vd.i], brush->alpha);

    float col[4];
    SCULPT_vertex_color_get(ss, vd.vertex, col);
    IMB_blend_color_float(col, orig_data.col, buffer_color, brush->blend);
    CLAMP4(col, 0.0f, 1.0f);
    SCULPT_vertex_color_set(ss, vd.vertex, col);
  }
  BKE_pbvh_vertex_iter_end;
}

typedef struct SampleWetPaintTLSData {
  int tot_samples;
  float color[4];
} SampleWetPaintTLSData;

static void do_sample_wet_paint_task_cb(void *__restrict userdata,
                                        const int n,
                                        const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  SampleWetPaintTLSData *swptd = tls->userdata_chunk;
  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, &test, data->brush->falloff_shape);

  test.radius *= data->brush->wet_paint_radius_factor;
  test.radius_squared = test.radius * test.radius;

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (!sculpt_brush_test_sq_fn(&test, vd.co)) {
      continue;
    }

    float col[4];
    SCULPT_vertex_color_get(ss, vd.vertex, col);

    add_v4_v4(swptd->color, col);
    swptd->tot_samples++;
  }
  BKE_pbvh_vertex_iter_end;
}

static void sample_wet_paint_reduce(const void *__restrict UNUSED(userdata),
                                    void *__restrict chunk_join,
                                    void *__restrict chunk)
{
  SampleWetPaintTLSData *join = chunk_join;
  SampleWetPaintTLSData *swptd = chunk;

  join->tot_samples += swptd->tot_samples;
  add_v4_v4(join->color, swptd->color);
}

void SCULPT_do_paint_brush(
    PaintModeSettings *paint_mode_settings, Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode)
{
  if (SCULPT_use_image_paint_brush(paint_mode_settings, ob)) {
    SCULPT_do_paint_brush_image(paint_mode_settings, sd, ob, nodes, totnode);
    return;
  }

  Brush *brush = BKE_paint_brush(&sd->paint);
  SculptSession *ss = ob->sculpt;

  if (!SCULPT_has_colors(ss)) {
    return;
  }

  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(ss->cache)) {
    if (SCULPT_stroke_is_first_brush_step(ss->cache)) {
      ss->cache->density_seed = (float)BLI_hash_int_01(ss->cache->location[0] * 1000);
    }
    return;
  }

  BKE_curvemapping_init(brush->curve);

  float area_no[3];
  float mat[4][4];
  float scale[4][4];
  float tmat[4][4];

  /* If the brush is round the tip does not need to be aligned to the surface, so this saves a
   * whole iteration over the affected nodes. */
  if (brush->tip_roundness < 1.0f) {
    SCULPT_calc_area_normal(sd, ob, nodes, totnode, area_no);

    cross_v3_v3v3(mat[0], area_no, ss->cache->grab_delta_symmetry);
    mat[0][3] = 0;
    cross_v3_v3v3(mat[1], area_no, mat[0]);
    mat[1][3] = 0;
    copy_v3_v3(mat[2], area_no);
    mat[2][3] = 0;
    copy_v3_v3(mat[3], ss->cache->location);
    mat[3][3] = 1;
    normalize_m4(mat);

    scale_m4_fl(scale, ss->cache->radius);
    mul_m4_m4m4(tmat, mat, scale);
    mul_v3_fl(tmat[1], brush->tip_scale_x);
    invert_m4_m4(mat, tmat);
    if (is_zero_m4(mat)) {
      return;
    }
  }

  /* Smooth colors mode. */
  if (ss->cache->alt_smooth) {
    SculptThreadedTaskData data = {
        .sd = sd,
        .ob = ob,
        .brush = brush,
        .nodes = nodes,
        .mat = mat,
    };

    TaskParallelSettings settings;
    BKE_pbvh_parallel_range_settings(&settings, true, totnode);
    BLI_task_parallel_range(0, totnode, &data, do_color_smooth_task_cb_exec, &settings);
    return;
  }

  /* Regular Paint mode. */

  /* Wet paint color sampling. */
  float wet_color[4] = {0.0f};
  if (ss->cache->paint_brush.wet_mix > 0.0f) {
    SculptThreadedTaskData task_data = {
        .sd = sd,
        .ob = ob,
        .nodes = nodes,
        .brush = brush,
    };

    SampleWetPaintTLSData swptd;
    swptd.tot_samples = 0;
    zero_v4(swptd.color);

    TaskParallelSettings settings_sample;
    BKE_pbvh_parallel_range_settings(&settings_sample, true, totnode);
    settings_sample.func_reduce = sample_wet_paint_reduce;
    settings_sample.userdata_chunk = &swptd;
    settings_sample.userdata_chunk_size = sizeof(SampleWetPaintTLSData);
    BLI_task_parallel_range(0, totnode, &task_data, do_sample_wet_paint_task_cb, &settings_sample);

    if (swptd.tot_samples > 0 && is_finite_v4(swptd.color)) {
      copy_v4_v4(wet_color, swptd.color);
      mul_v4_fl(wet_color, 1.0f / swptd.tot_samples);
      CLAMP4(wet_color, 0.0f, 1.0f);

      if (ss->cache->first_time) {
        copy_v4_v4(ss->cache->wet_mix_prev_color, wet_color);
      }
      blend_color_interpolate_float(wet_color,
                                    wet_color,
                                    ss->cache->wet_mix_prev_color,
                                    ss->cache->paint_brush.wet_persistence);
      copy_v4_v4(ss->cache->wet_mix_prev_color, wet_color);
      CLAMP4(ss->cache->wet_mix_prev_color, 0.0f, 1.0f);
    }
  }

  /* Threaded loop over nodes. */
  SculptThreadedTaskData data = {
      .sd = sd,
      .ob = ob,
      .brush = brush,
      .nodes = nodes,
      .wet_mix_sampled_color = wet_color,
      .mat = mat,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, do_paint_brush_task_cb_ex, &settings);
}

static void do_smear_brush_task_cb_exec(void *__restrict userdata,
                                        const int n,
                                        const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const Brush *brush = data->brush;
  const float bstrength = ss->cache->bstrength;

  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, &test, data->brush->falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(tls);

  float brush_delta[3];

  if (brush->flag & BRUSH_ANCHORED) {
    copy_v3_v3(brush_delta, ss->cache->grab_delta_symmetry);
  }
  else {
    sub_v3_v3v3(brush_delta, ss->cache->location, ss->cache->last_location);
  }

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (!sculpt_brush_test_sq_fn(&test, vd.co)) {
      continue;
    }
    const float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                                brush,
                                                                vd.co,
                                                                sqrtf(test.dist),
                                                                vd.no,
                                                                vd.fno,
                                                                vd.mask ? *vd.mask : 0.0f,
                                                                vd.vertex,
                                                                thread_id);

    float current_disp[3];
    float current_disp_norm[3];
    float interp_color[4];
    copy_v4_v4(interp_color, ss->cache->prev_colors[vd.index]);

    float no[3];
    SCULPT_vertex_normal_get(ss, vd.vertex, no);

    switch (brush->smear_deform_type) {
      case BRUSH_SMEAR_DEFORM_DRAG:
        copy_v3_v3(current_disp, brush_delta);
        break;
      case BRUSH_SMEAR_DEFORM_PINCH:
        sub_v3_v3v3(current_disp, ss->cache->location, vd.co);
        break;
      case BRUSH_SMEAR_DEFORM_EXPAND:
        sub_v3_v3v3(current_disp, vd.co, ss->cache->location);
        break;
    }

    /* Project into vertex plane. */
    madd_v3_v3fl(current_disp, no, -dot_v3v3(current_disp, no));

    normalize_v3_v3(current_disp_norm, current_disp);
    mul_v3_v3fl(current_disp, current_disp_norm, bstrength);

    float accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float totw = 0.0f;

    /*
     * NOTE: we have to do a nested iteration here to avoid
     * blocky artifacts on quad topologies.  The runtime cost
     * is not as bad as it seems due to neighbor iteration
     * in the sculpt code being cache bound; once the data is in
     * the cache iterating over it a few more times is not terribly
     * costly.
     */

    SculptVertexNeighborIter ni2;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vd.vertex, ni2) {
      const float *nco = SCULPT_vertex_co_get(ss, ni2.vertex);

      SculptVertexNeighborIter ni;
      SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, ni2.vertex, ni) {
        if (ni.index == vd.index) {
          continue;
        }

        float vertex_disp[3];
        float vertex_disp_norm[3];

        sub_v3_v3v3(vertex_disp, SCULPT_vertex_co_get(ss, ni.vertex), vd.co);

        /* Weight by how close we are to our target distance from vd.co. */
        float w = (1.0f + fabsf(len_v3(vertex_disp) / bstrength - 1.0f));

        /* TODO: use cotangents (or at least face areas) here. */
        float len = len_v3v3(SCULPT_vertex_co_get(ss, ni.vertex), nco);
        if (len > 0.0f) {
          len = bstrength / len;
        }
        else { /* Coincident point. */
          len = 1.0f;
        }

        /* Multiply weight with edge lengths (in the future this will be
         * cotangent weights or face areas). */
        w *= len;

        /* Build directional weight. */

        /* Project into vertex plane. */
        madd_v3_v3fl(vertex_disp, no, -dot_v3v3(no, vertex_disp));
        normalize_v3_v3(vertex_disp_norm, vertex_disp);

        if (dot_v3v3(current_disp_norm, vertex_disp_norm) >= 0.0f) {
          continue;
        }

        const float *neighbor_color = ss->cache->prev_colors[ni.index];
        float color_interp = -dot_v3v3(current_disp_norm, vertex_disp_norm);

        /* Square directional weight to get a somewhat sharper result. */
        w *= color_interp * color_interp;

        madd_v4_v4fl(accum, neighbor_color, w);
        totw += w;
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni2);

    if (totw != 0.0f) {
      mul_v4_fl(accum, 1.0f / totw);
    }

    blend_color_mix_float(interp_color, interp_color, accum);

    float col[4];
    SCULPT_vertex_color_get(ss, vd.vertex, col);
    blend_color_interpolate_float(col, ss->cache->prev_colors[vd.index], interp_color, fade);
    SCULPT_vertex_color_set(ss, vd.vertex, col);
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_smear_store_prev_colors_task_cb_exec(void *__restrict userdata,
                                                    const int n,
                                                    const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;

  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    SCULPT_vertex_color_get(ss, vd.vertex, ss->cache->prev_colors[vd.index]);
  }
  BKE_pbvh_vertex_iter_end;
}

void SCULPT_do_smear_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode)
{
  Brush *brush = BKE_paint_brush(&sd->paint);
  SculptSession *ss = ob->sculpt;

  if (!SCULPT_has_colors(ss) || ss->cache->bstrength == 0.0f) {
    return;
  }

  const int totvert = SCULPT_vertex_count_get(ss);

  if (!ss->cache->prev_colors) {
    ss->cache->prev_colors = MEM_callocN(sizeof(float[4]) * totvert, "prev colors");
    for (int i = 0; i < totvert; i++) {
      PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

      SCULPT_vertex_color_get(ss, vertex, ss->cache->prev_colors[i]);
    }
  }

  BKE_curvemapping_init(brush->curve);

  SculptThreadedTaskData data = {
      .sd = sd,
      .ob = ob,
      .brush = brush,
      .nodes = nodes,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);

  /* Smooth colors mode. */
  if (ss->cache->alt_smooth) {
    BLI_task_parallel_range(0, totnode, &data, do_color_smooth_task_cb_exec, &settings);
  }
  else {
    /* Smear mode. */
    BLI_task_parallel_range(0, totnode, &data, do_smear_store_prev_colors_task_cb_exec, &settings);
    BLI_task_parallel_range(0, totnode, &data, do_smear_brush_task_cb_exec, &settings);
  }
}

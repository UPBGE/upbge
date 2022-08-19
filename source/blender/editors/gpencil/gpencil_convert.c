/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 * Operator for converting Grease Pencil data to geometry.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_rand.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_anim_types.h"
#include "DNA_collection_types.h"
#include "DNA_curve_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_animsys.h"
#include "BKE_collection.h"
#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_fcurve.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_image.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_tracking.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "UI_interface.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "UI_resources.h"
#include "UI_view2d.h"

#include "ED_clip.h"
#include "ED_gpencil.h"
#include "ED_keyframing.h"
#include "ED_object.h"
#include "ED_view3d.h"

#include "gpencil_intern.h"

/* ************************************************ */
/* Grease Pencil to Data Operator */

/* defines for possible modes */
enum {
  GP_STROKECONVERT_PATH = 1,
  GP_STROKECONVERT_CURVE,
  GP_STROKECONVERT_POLY,
};

/* Defines for possible timing modes */
enum {
  GP_STROKECONVERT_TIMING_NONE = 1,
  GP_STROKECONVERT_TIMING_LINEAR = 2,
  GP_STROKECONVERT_TIMING_FULL = 3,
  GP_STROKECONVERT_TIMING_CUSTOMGAP = 4,
};

/* RNA enum define */
static const EnumPropertyItem prop_gpencil_convertmodes[] = {
    {GP_STROKECONVERT_PATH, "PATH", ICON_CURVE_PATH, "Path", "Animation path"},
    {GP_STROKECONVERT_CURVE, "CURVE", ICON_CURVE_BEZCURVE, "Bezier Curve", "Smooth Bezier curve"},
    {GP_STROKECONVERT_POLY,
     "POLY",
     ICON_MESH_DATA,
     "Polygon Curve",
     "Bezier curve with straight-line segments (vector handles)"},
    {0, NULL, 0, NULL, NULL},
};

static const EnumPropertyItem prop_gpencil_convert_timingmodes_restricted[] = {
    {GP_STROKECONVERT_TIMING_NONE, "NONE", 0, "No Timing", "Ignore timing"},
    {GP_STROKECONVERT_TIMING_LINEAR, "LINEAR", 0, "Linear", "Simple linear timing"},
    {0, NULL, 0, NULL, NULL},
};

static const EnumPropertyItem prop_gpencil_convert_timingmodes[] = {
    {GP_STROKECONVERT_TIMING_NONE, "NONE", 0, "No Timing", "Ignore timing"},
    {GP_STROKECONVERT_TIMING_LINEAR, "LINEAR", 0, "Linear", "Simple linear timing"},
    {GP_STROKECONVERT_TIMING_FULL,
     "FULL",
     0,
     "Original",
     "Use the original timing, gaps included"},
    {GP_STROKECONVERT_TIMING_CUSTOMGAP,
     "CUSTOMGAP",
     0,
     "Custom Gaps",
     "Use the original timing, but with custom gap lengths (in frames)"},
    {0, NULL, 0, NULL, NULL},
};

static const EnumPropertyItem *rna_GPConvert_mode_items(bContext *UNUSED(C),
                                                        PointerRNA *ptr,
                                                        PropertyRNA *UNUSED(prop),
                                                        bool *UNUSED(r_free))
{
  if (RNA_boolean_get(ptr, "use_timing_data")) {
    return prop_gpencil_convert_timingmodes;
  }
  return prop_gpencil_convert_timingmodes_restricted;
}

/* --- */

/* convert the coordinates from the given stroke point into 3d-coordinates
 * - assumes that the active space is the 3D-View
 */
static void gpencil_strokepoint_convertcoords(bContext *C,
                                              bGPDlayer *gpl,
                                              bGPDstroke *gps,
                                              bGPDspoint *source_pt,
                                              float p3d[3],
                                              const rctf *subrect)
{
  Scene *scene = CTX_data_scene(C);
  View3D *v3d = CTX_wm_view3d(C);
  ARegion *region = CTX_wm_region(C);
  /* TODO(sergey): This function might be called from a loop, but no tagging is happening in it,
   * so it's not that expensive to ensure evaluated depsgraph here. However, ideally all the
   * parameters are to wrapped into a context style struct and queried from Context once. */
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Object *obact = CTX_data_active_object(C);
  bGPDspoint mypt, *pt;

  float diff_mat[4][4];
  pt = &mypt;

  /* apply parent transform */
  float fpt[3];
  BKE_gpencil_layer_transform_matrix_get(depsgraph, obact, gpl, diff_mat);
  mul_v3_m4v3(fpt, diff_mat, &source_pt->x);
  copy_v3_v3(&pt->x, fpt);

  if (gps->flag & GP_STROKE_3DSPACE) {
    /* directly use 3d-coordinates */
    copy_v3_v3(p3d, &pt->x);
  }
  else {
    const float *fp = scene->cursor.location;
    float mvalf[2];

    /* get screen coordinate */
    if (gps->flag & GP_STROKE_2DSPACE) {
      View2D *v2d = &region->v2d;
      UI_view2d_view_to_region_fl(v2d, pt->x, pt->y, &mvalf[0], &mvalf[1]);
    }
    else {
      if (subrect) {
        mvalf[0] = (((float)pt->x / 100.0f) * BLI_rctf_size_x(subrect)) + subrect->xmin;
        mvalf[1] = (((float)pt->y / 100.0f) * BLI_rctf_size_y(subrect)) + subrect->ymin;
      }
      else {
        mvalf[0] = (float)pt->x / 100.0f * region->winx;
        mvalf[1] = (float)pt->y / 100.0f * region->winy;
      }
    }

    ED_view3d_win_to_3d(v3d, region, fp, mvalf, p3d);
  }
}

/* --- */

/* temp struct for gp_stroke_path_animation() */
typedef struct tGpTimingData {
  /* Data set from operator settings */
  int mode;
  int frame_range; /* Number of frames evaluated for path animation */
  int start_frame, end_frame;
  bool realtime; /* Will overwrite end_frame in case of Original or CustomGap timing... */
  float gap_duration, gap_randomness; /* To be used with CustomGap mode. */
  int seed;

  /* Data set from points, used to compute final timing FCurve */
  int points_num, cur_point;

  /* Distances */
  float *dists;
  float tot_dist;

  /* Times */
  float *times; /* NOTE: Gap times will be negative! */
  float tot_time, gap_tot_time;
  double inittime;

  /* Only used during creation of dists & times lists. */
  float offset_time;

  /* Curve bevel. */
  float bevel_depth;
  int bevel_resolution;
} tGpTimingData;

/* Init point buffers for timing data.
 * Note this assumes we only grow those arrays!
 */
static void gpencil_timing_data_set_num(tGpTimingData *gtd, const int num)
{
  float *tmp;

  BLI_assert(num > gtd->points_num);

  /* distances */
  tmp = gtd->dists;
  gtd->dists = MEM_callocN(sizeof(float) * num, __func__);
  if (tmp) {
    memcpy(gtd->dists, tmp, sizeof(float) * gtd->points_num);
    MEM_freeN(tmp);
  }

  /* times */
  tmp = gtd->times;
  gtd->times = MEM_callocN(sizeof(float) * num, __func__);
  if (tmp) {
    memcpy(gtd->times, tmp, sizeof(float) * gtd->points_num);
    MEM_freeN(tmp);
  }

  gtd->points_num = num;
}

/* add stroke point to timing buffers */
static void gpencil_timing_data_add_point(tGpTimingData *gtd,
                                          const double stroke_inittime,
                                          const float time,
                                          const float delta_dist)
{
  float delta_time = 0.0f;
  const int cur_point = gtd->cur_point;

  if (!cur_point) {
    /* Special case, first point, if time is not 0.0f we have to compensate! */
    gtd->offset_time = -time;
    gtd->times[cur_point] = 0.0f;
  }
  else if (time < 0.0f) {
    /* This is a gap, negative value! */
    gtd->times[cur_point] = -(((float)(stroke_inittime - gtd->inittime)) + time +
                              gtd->offset_time);
    delta_time = -gtd->times[cur_point] - gtd->times[cur_point - 1];

    gtd->gap_tot_time += delta_time;
  }
  else {
    gtd->times[cur_point] = (((float)(stroke_inittime - gtd->inittime)) + time + gtd->offset_time);
    delta_time = gtd->times[cur_point] - fabsf(gtd->times[cur_point - 1]);
  }

  gtd->tot_time += delta_time;
  gtd->tot_dist += delta_dist;
  gtd->dists[cur_point] = gtd->tot_dist;

  gtd->cur_point++;
}

/* In frames! Binary search for FCurve keys have a threshold of 0.01, so we can't set
 * arbitrarily close points - this is esp. important with NoGaps mode!
 */
#define MIN_TIME_DELTA 0.02f

/* Loop over next points to find the end of the stroke, and compute */
static int gpencil_find_end_of_stroke_idx(tGpTimingData *gtd,
                                          RNG *rng,
                                          const int idx,
                                          const int gaps_count,
                                          int *gaps_done_count,
                                          const float tot_gaps_time,
                                          const float delta_time,
                                          float *next_delta_time)
{
  int j;

  for (j = idx + 1; j < gtd->points_num; j++) {
    if (gtd->times[j] < 0) {
      gtd->times[j] = -gtd->times[j];
      if (gtd->mode == GP_STROKECONVERT_TIMING_CUSTOMGAP) {
        /* In this mode, gap time between this stroke and the next should be 0 currently...
         * So we have to compute its final duration!
         */
        if (gtd->gap_randomness > 0.0f) {
          /* We want gaps that are in gtd->gap_duration +/- gtd->gap_randomness range,
           * and which sum to exactly tot_gaps_time...
           */
          int rem_gaps = gaps_count - (*gaps_done_count);
          if (rem_gaps < 2) {
            /* Last gap, just give remaining time! */
            *next_delta_time = tot_gaps_time;
          }
          else {
            float delta, min, max;

            /* This code ensures that if the first gaps
             * have been shorter than average gap_duration, next gaps
             * will tend to be longer (i.e. try to recover the lateness), and vice-versa! */
            delta = delta_time - (gtd->gap_duration * (*gaps_done_count));

            /* Clamp min between [-gap_randomness, 0.0], with lower delta giving higher min */
            min = -gtd->gap_randomness - delta;
            CLAMP(min, -gtd->gap_randomness, 0.0f);

            /* Clamp max between [0.0, gap_randomness], with lower delta giving higher max */
            max = gtd->gap_randomness - delta;
            CLAMP(max, 0.0f, gtd->gap_randomness);
            *next_delta_time += gtd->gap_duration + (BLI_rng_get_float(rng) * (max - min)) + min;
          }
        }
        else {
          *next_delta_time += gtd->gap_duration;
        }
      }
      (*gaps_done_count)++;
      break;
    }
  }

  return j - 1;
}

static void gpencil_stroke_path_animation_preprocess_gaps(tGpTimingData *gtd,
                                                          RNG *rng,
                                                          int *gaps_count,
                                                          float *r_tot_gaps_time)
{
  float delta_time = 0.0f;

  for (int i = 0; i < gtd->points_num; i++) {
    if (gtd->times[i] < 0 && i) {
      (*gaps_count)++;
      gtd->times[i] = -gtd->times[i] - delta_time;
      delta_time += gtd->times[i] - gtd->times[i - 1];
      gtd->times[i] = -gtd->times[i - 1]; /* Temp marker, values *have* to be different! */
    }
    else {
      gtd->times[i] -= delta_time;
    }
  }
  gtd->tot_time -= delta_time;

  *r_tot_gaps_time = (float)(*gaps_count) * gtd->gap_duration;
  gtd->tot_time += *r_tot_gaps_time;
  if (gtd->gap_randomness > 0.0f) {
    BLI_rng_srandom(rng, gtd->seed);
  }
}

static void gpencil_stroke_path_animation_add_keyframes(ReportList *reports,
                                                        PointerRNA ptr,
                                                        PropertyRNA *prop,
                                                        Depsgraph *depsgraph,
                                                        FCurve *fcu,
                                                        Curve *cu,
                                                        tGpTimingData *gtd,
                                                        RNG *rng,
                                                        const float time_range,
                                                        const int gaps_count,
                                                        const float tot_gaps_time)
{
  /* Use actual recorded timing! */
  const float time_start = (float)gtd->start_frame;

  float last_valid_time = 0.0f;
  int end_stroke_idx = -1, start_stroke_idx = 0;
  float end_stroke_time = 0.0f;

  /* CustomGaps specific */
  float delta_time = 0.0f, next_delta_time = 0.0f;
  int gaps_done_count = 0;

  /* This is a bit tricky, as:
   * - We can't add arbitrarily close points on FCurve (in time).
   * - We *must* have all "caps" points of all strokes in FCurve, as much as possible!
   */
  for (int i = 0; i < gtd->points_num; i++) {
    /* If new stroke... */
    if (i > end_stroke_idx) {
      start_stroke_idx = i;
      delta_time = next_delta_time;
      /* find end of that new stroke */
      end_stroke_idx = gpencil_find_end_of_stroke_idx(
          gtd, rng, i, gaps_count, &gaps_done_count, tot_gaps_time, delta_time, &next_delta_time);
      /* This one should *never* be negative! */
      end_stroke_time = time_start +
                        ((gtd->times[end_stroke_idx] + delta_time) / gtd->tot_time * time_range);
    }

    /* Simple proportional stuff... */
    cu->ctime = gtd->dists[i] / gtd->tot_dist * cu->pathlen;
    float cfra = time_start + ((gtd->times[i] + delta_time) / gtd->tot_time * time_range);

    /* And now, the checks about timing... */
    if (i == start_stroke_idx) {
      /* If first point of a stroke, be sure it's enough ahead of last valid keyframe, and
       * that the end point of the stroke is far enough!
       * In case it is not, we keep the end point...
       * Note that with CustomGaps mode, this is here we set the actual gap timing!
       */
      if ((end_stroke_time - last_valid_time) > MIN_TIME_DELTA * 2) {
        if ((cfra - last_valid_time) < MIN_TIME_DELTA) {
          cfra = last_valid_time + MIN_TIME_DELTA;
        }
        const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(
            depsgraph, cfra);
        insert_keyframe_direct(reports,
                               ptr,
                               prop,
                               fcu,
                               &anim_eval_context,
                               BEZT_KEYTYPE_KEYFRAME,
                               NULL,
                               INSERTKEY_FAST);
        last_valid_time = cfra;
      }
    }
    else if (i == end_stroke_idx) {
      /* Always try to insert end point of a curve (should be safe enough, anyway...) */
      if ((cfra - last_valid_time) < MIN_TIME_DELTA) {
        cfra = last_valid_time + MIN_TIME_DELTA;
      }
      const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(depsgraph,
                                                                                        cfra);
      insert_keyframe_direct(reports,
                             ptr,
                             prop,
                             fcu,
                             &anim_eval_context,
                             BEZT_KEYTYPE_KEYFRAME,
                             NULL,
                             INSERTKEY_FAST);
      last_valid_time = cfra;
    }
    else {
      /* Else ("middle" point), we only insert it if it's far enough from last keyframe,
       * and also far enough from (not yet added!) end_stroke keyframe!
       */
      if ((cfra - last_valid_time) > MIN_TIME_DELTA && (end_stroke_time - cfra) > MIN_TIME_DELTA) {
        const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(
            depsgraph, cfra);
        insert_keyframe_direct(reports,
                               ptr,
                               prop,
                               fcu,
                               &anim_eval_context,
                               BEZT_KEYTYPE_BREAKDOWN,
                               NULL,
                               INSERTKEY_FAST);
        last_valid_time = cfra;
      }
      else if (G.debug & G_DEBUG) {
        printf(
            "\t Skipping \"middle\" point %d, too close from last added point or end point %d\n",
            i,
            end_stroke_idx);
      }
    }
  }
}

static void gpencil_stroke_path_animation(bContext *C,
                                          ReportList *reports,
                                          Curve *cu,
                                          tGpTimingData *gtd)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  bAction *act;
  FCurve *fcu;
  PointerRNA ptr;
  PropertyRNA *prop = NULL;
  int gaps_count = 0;

  if (gtd->mode == GP_STROKECONVERT_TIMING_NONE) {
    return;
  }

  /* gap_duration and gap_randomness are in frames, but we need seconds!!! */
  gtd->gap_duration = FRA2TIME(gtd->gap_duration);
  gtd->gap_randomness = FRA2TIME(gtd->gap_randomness);

  /* Enable path! */
  cu->flag |= CU_PATH;
  cu->pathlen = gtd->frame_range;

  /* Get RNA pointer to read/write path time values */
  RNA_id_pointer_create((ID *)cu, &ptr);
  prop = RNA_struct_find_property(&ptr, "eval_time");

  /* Ensure we have an F-Curve to add keyframes to */
  act = ED_id_action_ensure(bmain, (ID *)cu);
  fcu = ED_action_fcurve_ensure(bmain, act, NULL, &ptr, "eval_time", 0);

  if (gtd->mode == GP_STROKECONVERT_TIMING_LINEAR) {
    float cfra;

    /* Linear extrapolation! */
    fcu->extend = FCURVE_EXTRAPOLATE_LINEAR;

    cu->ctime = 0.0f;
    cfra = (float)gtd->start_frame;
    AnimationEvalContext anim_eval_context_start = BKE_animsys_eval_context_construct(depsgraph,
                                                                                      cfra);
    insert_keyframe_direct(reports,
                           ptr,
                           prop,
                           fcu,
                           &anim_eval_context_start,
                           BEZT_KEYTYPE_KEYFRAME,
                           NULL,
                           INSERTKEY_FAST);

    cu->ctime = cu->pathlen;
    if (gtd->realtime) {
      cfra += (float)TIME2FRA(gtd->tot_time); /* Seconds to frames */
    }
    else {
      cfra = (float)gtd->end_frame;
    }
    AnimationEvalContext anim_eval_context_end = BKE_animsys_eval_context_construct(depsgraph,
                                                                                    cfra);
    insert_keyframe_direct(reports,
                           ptr,
                           prop,
                           fcu,
                           &anim_eval_context_end,
                           BEZT_KEYTYPE_KEYFRAME,
                           NULL,
                           INSERTKEY_FAST);
  }
  else {
    /* Use actual recorded timing! */
    RNG *rng = BLI_rng_new(0);
    float time_range;

    /* CustomGaps specific */
    float tot_gaps_time = 0.0f;

    /* Pre-process gaps, in case we don't want to keep their original timing */
    if (gtd->mode == GP_STROKECONVERT_TIMING_CUSTOMGAP) {
      gpencil_stroke_path_animation_preprocess_gaps(gtd, rng, &gaps_count, &tot_gaps_time);
    }

    if (gtd->realtime) {
      time_range = (float)TIME2FRA(gtd->tot_time); /* Seconds to frames */
    }
    else {
      time_range = (float)(gtd->end_frame - gtd->start_frame);
    }

    gpencil_stroke_path_animation_add_keyframes(
        reports, ptr, prop, depsgraph, fcu, cu, gtd, rng, time_range, gaps_count, tot_gaps_time);

    BLI_rng_free(rng);
  }

  /* As we used INSERTKEY_FAST mode, we need to recompute all curve's handles now */
  BKE_fcurve_handles_recalc(fcu);

  WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME | NA_EDITED, NULL);

  /* send updates */
  DEG_id_tag_update(&cu->id, 0);
}

#undef MIN_TIME_DELTA

#define GAP_DFAC 0.01f
#define WIDTH_CORR_FAC 0.1f
#define BEZT_HANDLE_FAC 0.3f

/* convert stroke to 3d path */

/* helper */
static void gpencil_stroke_to_path_add_point(tGpTimingData *gtd,
                                             BPoint *bp,
                                             const float p[3],
                                             const float prev_p[3],
                                             const bool do_gtd,
                                             const double inittime,
                                             const float time,
                                             const float width,
                                             const float rad_fac,
                                             float minmax_weights[2])
{
  copy_v3_v3(bp->vec, p);
  bp->vec[3] = 1.0f;

  /* set settings */
  bp->f1 = SELECT;
  bp->radius = width * rad_fac;
  bp->weight = width;
  CLAMP(bp->weight, 0.0f, 1.0f);
  if (bp->weight < minmax_weights[0]) {
    minmax_weights[0] = bp->weight;
  }
  else if (bp->weight > minmax_weights[1]) {
    minmax_weights[1] = bp->weight;
  }

  /* Update timing data */
  if (do_gtd) {
    gpencil_timing_data_add_point(gtd, inittime, time, len_v3v3(prev_p, p));
  }
}

static void gpencil_stroke_to_path(bContext *C,
                                   bGPDlayer *gpl,
                                   bGPDstroke *gps,
                                   Curve *cu,
                                   rctf *subrect,
                                   Nurb **curnu,
                                   float minmax_weights[2],
                                   const float rad_fac,
                                   bool stitch,
                                   const bool add_start_point,
                                   const bool add_end_point,
                                   tGpTimingData *gtd)
{
  bGPDspoint *pt;
  Nurb *nu = (curnu) ? *curnu : NULL;
  BPoint *bp, *prev_bp = NULL;
  const bool do_gtd = (gtd->mode != GP_STROKECONVERT_TIMING_NONE);
  const int add_start_end_points = (add_start_point ? 1 : 0) + (add_end_point ? 1 : 0);
  int i, old_nbp = 0;

  /* create new 'nurb' or extend current one within the curve */
  if (nu) {
    old_nbp = nu->pntsu;

    /* If stitch, the first point of this stroke is already present in current nu.
     * Else, we have to add two additional points to make the zero-radius link between strokes.
     */
    BKE_nurb_points_add(nu, gps->totpoints + (stitch ? -1 : 2) + add_start_end_points);
  }
  else {
    nu = (Nurb *)MEM_callocN(sizeof(Nurb), "gpstroke_to_path(nurb)");

    nu->pntsu = gps->totpoints + add_start_end_points;
    nu->pntsv = 1;
    nu->orderu = 2; /* point-to-point! */
    nu->type = CU_NURBS;
    nu->flagu = CU_NURB_ENDPOINT;
    nu->resolu = cu->resolu;
    nu->resolv = cu->resolv;
    nu->knotsu = NULL;

    nu->bp = (BPoint *)MEM_callocN(sizeof(BPoint) * nu->pntsu, "bpoints");

    stitch = false; /* Security! */
  }

  if (do_gtd) {
    gpencil_timing_data_set_num(gtd, nu->pntsu);
  }

  /* If needed, make the link between both strokes with two zero-radius additional points */
  /* About "zero-radius" point interpolations:
   * - If we have at least two points in current curve (most common case), we linearly extrapolate
   *   the last segment to get the first point (p1) position and timing.
   * - If we do not have those (quite odd, but may happen), we linearly interpolate the last point
   *   with the first point of the current stroke.
   *
   * The same goes for the second point, first segment of the current stroke is "negatively"
   * extrapolated if it exists, else (if the stroke is a single point),
   * linear interpolation with last curve point.
   */
  if (curnu && !stitch && old_nbp) {
    float p1[3], p2[3], p[3], next_p[3];
    float dt1 = 0.0f, dt2 = 0.0f;

    BLI_assert(gps->prev != NULL);

    prev_bp = NULL;
    if ((old_nbp > 1) && (gps->prev->totpoints > 1)) {
      /* Only use last curve segment if previous stroke was not a single-point one! */
      prev_bp = &nu->bp[old_nbp - 2];
    }
    bp = &nu->bp[old_nbp - 1];

    /* First point */
    gpencil_strokepoint_convertcoords(C, gpl, gps, gps->points, p, subrect);
    if (prev_bp) {
      interp_v3_v3v3(p1, bp->vec, prev_bp->vec, -GAP_DFAC);
      if (do_gtd) {
        const int idx = gps->prev->totpoints - 1;
        dt1 = interpf(gps->prev->points[idx - 1].time, gps->prev->points[idx].time, -GAP_DFAC);
      }
    }
    else {
      interp_v3_v3v3(p1, bp->vec, p, GAP_DFAC);
      if (do_gtd) {
        dt1 = interpf(gps->inittime - gps->prev->inittime, 0.0f, GAP_DFAC);
      }
    }
    bp++;
    gpencil_stroke_to_path_add_point(gtd,
                                     bp,
                                     p1,
                                     (bp - 1)->vec,
                                     do_gtd,
                                     gps->prev->inittime,
                                     dt1,
                                     0.0f,
                                     rad_fac,
                                     minmax_weights);

    /* Second point */
    /* Note dt2 is always negative, which marks the gap. */
    if (gps->totpoints > 1) {
      gpencil_strokepoint_convertcoords(C, gpl, gps, gps->points + 1, next_p, subrect);
      interp_v3_v3v3(p2, p, next_p, -GAP_DFAC);
      if (do_gtd) {
        dt2 = interpf(gps->points[1].time, gps->points[0].time, -GAP_DFAC);
      }
    }
    else {
      interp_v3_v3v3(p2, p, bp->vec, GAP_DFAC);
      if (do_gtd) {
        dt2 = interpf(gps->prev->inittime - gps->inittime, 0.0f, GAP_DFAC);
      }
    }
    bp++;
    gpencil_stroke_to_path_add_point(
        gtd, bp, p2, p1, do_gtd, gps->inittime, dt2, 0.0f, rad_fac, minmax_weights);

    old_nbp += 2;
  }
  else if (add_start_point) {
    float p[3], next_p[3];
    float dt = 0.0f;

    gpencil_strokepoint_convertcoords(C, gpl, gps, gps->points, p, subrect);
    if (gps->totpoints > 1) {
      gpencil_strokepoint_convertcoords(C, gpl, gps, gps->points + 1, next_p, subrect);
      interp_v3_v3v3(p, p, next_p, -GAP_DFAC);
      if (do_gtd) {
        dt = interpf(gps->points[1].time, gps->points[0].time, -GAP_DFAC);
      }
    }
    else {
      p[0] -= GAP_DFAC; /* Rather arbitrary... */
      dt = -GAP_DFAC;   /* Rather arbitrary too! */
    }
    bp = &nu->bp[old_nbp];
    /* Note we can't give anything else than 0.0 as time here, since a negative one
     * (which would be expected value) would not work
     * (it would be *before* gtd->inittime, which is not supported currently).
     */
    gpencil_stroke_to_path_add_point(
        gtd, bp, p, p, do_gtd, gps->inittime, dt, 0.0f, rad_fac, minmax_weights);

    old_nbp++;
  }

  if (old_nbp) {
    prev_bp = &nu->bp[old_nbp - 1];
  }

  /* add points */
  for (i = (stitch) ? 1 : 0, pt = &gps->points[(stitch) ? 1 : 0], bp = &nu->bp[old_nbp];
       i < gps->totpoints;
       i++, pt++, bp++) {
    float p[3];
    float width = pt->pressure * (gps->thickness + gpl->line_change) * WIDTH_CORR_FAC;

    /* get coordinates to add at */
    gpencil_strokepoint_convertcoords(C, gpl, gps, pt, p, subrect);

    gpencil_stroke_to_path_add_point(gtd,
                                     bp,
                                     p,
                                     (prev_bp) ? prev_bp->vec : p,
                                     do_gtd,
                                     gps->inittime,
                                     pt->time,
                                     width,
                                     rad_fac,
                                     minmax_weights);

    prev_bp = bp;
  }

  if (add_end_point) {
    float p[3];
    float dt = 0.0f;

    if (gps->totpoints > 1) {
      interp_v3_v3v3(p, prev_bp->vec, (prev_bp - 1)->vec, -GAP_DFAC);
      if (do_gtd) {
        const int idx = gps->totpoints - 1;
        dt = interpf(gps->points[idx - 1].time, gps->points[idx].time, -GAP_DFAC);
      }
    }
    else {
      copy_v3_v3(p, prev_bp->vec);
      p[0] += GAP_DFAC; /* Rather arbitrary... */
      dt = GAP_DFAC;    /* Rather arbitrary too! */
    }
    /* Note bp has already been incremented in main loop above, so it points to the right place. */
    gpencil_stroke_to_path_add_point(
        gtd, bp, p, prev_bp->vec, do_gtd, gps->inittime, dt, 0.0f, rad_fac, minmax_weights);
  }

  /* add nurb to curve */
  if (!curnu || !*curnu) {
    BLI_addtail(&cu->nurb, nu);
  }
  if (curnu) {
    *curnu = nu;
  }

  BKE_nurb_knot_calc_u(nu);
}

/* convert stroke to 3d bezier */

/* helper */
static void gpencil_stroke_to_bezier_add_point(tGpTimingData *gtd,
                                               BezTriple *bezt,
                                               const float p[3],
                                               const float h1[3],
                                               const float h2[3],
                                               const float prev_p[3],
                                               const bool do_gtd,
                                               const double inittime,
                                               const float time,
                                               const float width,
                                               const float rad_fac,
                                               float minmax_weights[2])
{
  copy_v3_v3(bezt->vec[0], h1);
  copy_v3_v3(bezt->vec[1], p);
  copy_v3_v3(bezt->vec[2], h2);

  /* set settings */
  bezt->h1 = bezt->h2 = HD_FREE;
  bezt->f1 = bezt->f2 = bezt->f3 = SELECT;
  bezt->radius = width * rad_fac;
  bezt->weight = width;
  CLAMP(bezt->weight, 0.0f, 1.0f);
  if (bezt->weight < minmax_weights[0]) {
    minmax_weights[0] = bezt->weight;
  }
  else if (bezt->weight > minmax_weights[1]) {
    minmax_weights[1] = bezt->weight;
  }

  /* Update timing data */
  if (do_gtd) {
    gpencil_timing_data_add_point(gtd, inittime, time, len_v3v3(prev_p, p));
  }
}

static void gpencil_stroke_to_bezier(bContext *C,
                                     bGPDlayer *gpl,
                                     bGPDstroke *gps,
                                     Curve *cu,
                                     rctf *subrect,
                                     Nurb **curnu,
                                     float minmax_weights[2],
                                     const float rad_fac,
                                     bool stitch,
                                     const bool add_start_point,
                                     const bool add_end_point,
                                     tGpTimingData *gtd)
{
  bGPDspoint *pt;
  Nurb *nu = (curnu) ? *curnu : NULL;
  BezTriple *bezt, *prev_bezt = NULL;
  int i, tot, old_nbezt = 0;
  const int add_start_end_points = (add_start_point ? 1 : 0) + (add_end_point ? 1 : 0);
  float p3d_cur[3], p3d_prev[3], p3d_next[3], h1[3], h2[3];
  const bool do_gtd = (gtd->mode != GP_STROKECONVERT_TIMING_NONE);

  /* create new 'nurb' or extend current one within the curve */
  if (nu) {
    old_nbezt = nu->pntsu;
    /* If we do stitch, first point of current stroke is assumed the same as last point of
     * previous stroke, so no need to add it.
     * If no stitch, we want to add two additional points to make a "zero-radius"
     * link between both strokes.
     */
    BKE_nurb_bezierPoints_add(nu, gps->totpoints + ((stitch) ? -1 : 2) + add_start_end_points);
  }
  else {
    nu = (Nurb *)MEM_callocN(sizeof(Nurb), "gpstroke_to_bezier(nurb)");

    nu->pntsu = gps->totpoints + add_start_end_points;
    nu->resolu = 12;
    nu->resolv = 12;
    nu->type = CU_BEZIER;
    nu->bezt = (BezTriple *)MEM_callocN(sizeof(BezTriple) * nu->pntsu, "bezts");

    stitch = false; /* Security! */
  }

  if (do_gtd) {
    gpencil_timing_data_set_num(gtd, nu->pntsu);
  }

  tot = gps->totpoints;

  /* get initial coordinates */
  pt = gps->points;
  if (tot) {
    gpencil_strokepoint_convertcoords(C, gpl, gps, pt, (stitch) ? p3d_prev : p3d_cur, subrect);
    if (tot > 1) {
      gpencil_strokepoint_convertcoords(
          C, gpl, gps, pt + 1, (stitch) ? p3d_cur : p3d_next, subrect);
    }
    if (stitch && tot > 2) {
      gpencil_strokepoint_convertcoords(C, gpl, gps, pt + 2, p3d_next, subrect);
    }
  }

  /* If needed, make the link between both strokes with two zero-radius additional points */
  if (curnu && old_nbezt) {
    BLI_assert(gps->prev != NULL);

    /* Update last point's second handle */
    if (stitch) {
      bezt = &nu->bezt[old_nbezt - 1];
      interp_v3_v3v3(h2, bezt->vec[1], p3d_cur, BEZT_HANDLE_FAC);
      copy_v3_v3(bezt->vec[2], h2);
      pt++;
    }

    /* Create "link points" */
    /* About "zero-radius" point interpolations:
     * - If we have at least two points in current curve (most common case),
     *   we linearly extrapolate the last segment to get the first point (p1) position and timing.
     * - If we do not have those (quite odd, but may happen),
     *   we linearly interpolate the last point with the first point of the current stroke.
     *
     * The same goes for the second point,
     * first segment of the current stroke is "negatively" extrapolated
     * if it exists, else (if the stroke is a single point),
     * linear interpolation with last curve point.
     */
    else {
      float p1[3], p2[3];
      float dt1 = 0.0f, dt2 = 0.0f;

      prev_bezt = NULL;
      if ((old_nbezt > 1) && (gps->prev->totpoints > 1)) {
        /* Only use last curve segment if previous stroke was not a single-point one! */
        prev_bezt = &nu->bezt[old_nbezt - 2];
      }
      bezt = &nu->bezt[old_nbezt - 1];

      /* First point */
      if (prev_bezt) {
        interp_v3_v3v3(p1, prev_bezt->vec[1], bezt->vec[1], 1.0f + GAP_DFAC);
        if (do_gtd) {
          const int idx = gps->prev->totpoints - 1;
          dt1 = interpf(gps->prev->points[idx - 1].time, gps->prev->points[idx].time, -GAP_DFAC);
        }
      }
      else {
        interp_v3_v3v3(p1, bezt->vec[1], p3d_cur, GAP_DFAC);
        if (do_gtd) {
          dt1 = interpf(gps->inittime - gps->prev->inittime, 0.0f, GAP_DFAC);
        }
      }

      /* Second point */
      /* Note dt2 is always negative, which marks the gap. */
      if (tot > 1) {
        interp_v3_v3v3(p2, p3d_cur, p3d_next, -GAP_DFAC);
        if (do_gtd) {
          dt2 = interpf(gps->points[1].time, gps->points[0].time, -GAP_DFAC);
        }
      }
      else {
        interp_v3_v3v3(p2, p3d_cur, bezt->vec[1], GAP_DFAC);
        if (do_gtd) {
          dt2 = interpf(gps->prev->inittime - gps->inittime, 0.0f, GAP_DFAC);
        }
      }

      /* Second handle of last point of previous stroke. */
      interp_v3_v3v3(h2, bezt->vec[1], p1, BEZT_HANDLE_FAC);
      copy_v3_v3(bezt->vec[2], h2);

      /* First point */
      interp_v3_v3v3(h1, p1, bezt->vec[1], BEZT_HANDLE_FAC);
      interp_v3_v3v3(h2, p1, p2, BEZT_HANDLE_FAC);
      bezt++;
      gpencil_stroke_to_bezier_add_point(gtd,
                                         bezt,
                                         p1,
                                         h1,
                                         h2,
                                         (bezt - 1)->vec[1],
                                         do_gtd,
                                         gps->prev->inittime,
                                         dt1,
                                         0.0f,
                                         rad_fac,
                                         minmax_weights);

      /* Second point */
      interp_v3_v3v3(h1, p2, p1, BEZT_HANDLE_FAC);
      interp_v3_v3v3(h2, p2, p3d_cur, BEZT_HANDLE_FAC);
      bezt++;
      gpencil_stroke_to_bezier_add_point(
          gtd, bezt, p2, h1, h2, p1, do_gtd, gps->inittime, dt2, 0.0f, rad_fac, minmax_weights);

      old_nbezt += 2;
      copy_v3_v3(p3d_prev, p2);
    }
  }
  else if (add_start_point) {
    float p[3];
    float dt = 0.0f;

    if (gps->totpoints > 1) {
      interp_v3_v3v3(p, p3d_cur, p3d_next, -GAP_DFAC);
      if (do_gtd) {
        dt = interpf(gps->points[1].time, gps->points[0].time, -GAP_DFAC);
      }
    }
    else {
      copy_v3_v3(p, p3d_cur);
      p[0] -= GAP_DFAC; /* Rather arbitrary... */
      dt = -GAP_DFAC;   /* Rather arbitrary too! */
    }
    interp_v3_v3v3(h1, p, p3d_cur, -BEZT_HANDLE_FAC);
    interp_v3_v3v3(h2, p, p3d_cur, BEZT_HANDLE_FAC);
    bezt = &nu->bezt[old_nbezt];
    gpencil_stroke_to_bezier_add_point(
        gtd, bezt, p, h1, h2, p, do_gtd, gps->inittime, dt, 0.0f, rad_fac, minmax_weights);

    old_nbezt++;
    copy_v3_v3(p3d_prev, p);
  }

  if (old_nbezt) {
    prev_bezt = &nu->bezt[old_nbezt - 1];
  }

  /* add points */
  for (i = stitch ? 1 : 0, bezt = &nu->bezt[old_nbezt]; i < tot; i++, pt++, bezt++) {
    float width = pt->pressure * (gps->thickness + gpl->line_change) * WIDTH_CORR_FAC;

    if (i || old_nbezt) {
      interp_v3_v3v3(h1, p3d_cur, p3d_prev, BEZT_HANDLE_FAC);
    }
    else {
      interp_v3_v3v3(h1, p3d_cur, p3d_next, -BEZT_HANDLE_FAC);
    }

    if (i < tot - 1) {
      interp_v3_v3v3(h2, p3d_cur, p3d_next, BEZT_HANDLE_FAC);
    }
    else {
      interp_v3_v3v3(h2, p3d_cur, p3d_prev, -BEZT_HANDLE_FAC);
    }

    gpencil_stroke_to_bezier_add_point(gtd,
                                       bezt,
                                       p3d_cur,
                                       h1,
                                       h2,
                                       prev_bezt ? prev_bezt->vec[1] : p3d_cur,
                                       do_gtd,
                                       gps->inittime,
                                       pt->time,
                                       width,
                                       rad_fac,
                                       minmax_weights);

    /* Shift coord vectors. */
    copy_v3_v3(p3d_prev, p3d_cur);
    copy_v3_v3(p3d_cur, p3d_next);

    if (i + 2 < tot) {
      gpencil_strokepoint_convertcoords(C, gpl, gps, pt + 2, p3d_next, subrect);
    }

    prev_bezt = bezt;
  }

  if (add_end_point) {
    float p[3];
    float dt = 0.0f;

    if (gps->totpoints > 1) {
      interp_v3_v3v3(p, prev_bezt->vec[1], (prev_bezt - 1)->vec[1], -GAP_DFAC);
      if (do_gtd) {
        const int idx = gps->totpoints - 1;
        dt = interpf(gps->points[idx - 1].time, gps->points[idx].time, -GAP_DFAC);
      }
    }
    else {
      copy_v3_v3(p, prev_bezt->vec[1]);
      p[0] += GAP_DFAC; /* Rather arbitrary... */
      dt = GAP_DFAC;    /* Rather arbitrary too! */
    }

    /* Second handle of last point of this stroke. */
    interp_v3_v3v3(h2, prev_bezt->vec[1], p, BEZT_HANDLE_FAC);
    copy_v3_v3(prev_bezt->vec[2], h2);

    /* The end point */
    interp_v3_v3v3(h1, p, prev_bezt->vec[1], BEZT_HANDLE_FAC);
    interp_v3_v3v3(h2, p, prev_bezt->vec[1], -BEZT_HANDLE_FAC);
    /* Note bezt has already been incremented in main loop above,
     * so it points to the right place. */
    gpencil_stroke_to_bezier_add_point(gtd,
                                       bezt,
                                       p,
                                       h1,
                                       h2,
                                       prev_bezt->vec[1],
                                       do_gtd,
                                       gps->inittime,
                                       dt,
                                       0.0f,
                                       rad_fac,
                                       minmax_weights);
  }

  /* must calculate handles or else we crash */
  BKE_nurb_handles_calc(nu);

  if (!curnu || !*curnu) {
    BLI_addtail(&cu->nurb, nu);
  }
  if (curnu) {
    *curnu = nu;
  }
}

#undef GAP_DFAC
#undef WIDTH_CORR_FAC
#undef BEZT_HANDLE_FAC

static void gpencil_stroke_finalize_curve_endpoints(Curve *cu)
{
  /* start */
  Nurb *nu = cu->nurb.first;
  int i = 0;
  if (nu->bezt) {
    BezTriple *bezt = nu->bezt;
    if (bezt) {
      bezt[i].weight = bezt[i].radius = 0.0f;
    }
  }
  else if (nu->bp) {
    BPoint *bp = nu->bp;
    if (bp) {
      bp[i].weight = bp[i].radius = 0.0f;
    }
  }

  /* end */
  nu = cu->nurb.last;
  i = nu->pntsu - 1;
  if (nu->bezt) {
    BezTriple *bezt = nu->bezt;
    if (bezt) {
      bezt[i].weight = bezt[i].radius = 0.0f;
    }
  }
  else if (nu->bp) {
    BPoint *bp = nu->bp;
    if (bp) {
      bp[i].weight = bp[i].radius = 0.0f;
    }
  }
}

static void gpencil_stroke_norm_curve_weights(Curve *cu, const float minmax_weights[2])
{
  const float delta = minmax_weights[0];

  /* when delta == minmax_weights[0] == minmax_weights[1], we get div by zero T35686. */
  float fac;
  if (IS_EQF(delta, minmax_weights[1])) {
    fac = 1.0f;
  }
  else {
    fac = 1.0f / (minmax_weights[1] - delta);
  }

  LISTBASE_FOREACH (Nurb *, nu, &cu->nurb) {
    if (nu->bezt) {
      BezTriple *bezt = nu->bezt;
      for (int i = 0; i < nu->pntsu; i++, bezt++) {
        bezt->weight = (bezt->weight - delta) * fac;
      }
    }
    else if (nu->bp) {
      BPoint *bp = nu->bp;
      for (int i = 0; i < nu->pntsu; i++, bp++) {
        bp->weight = (bp->weight - delta) * fac;
      }
    }
  }
}

static int gpencil_camera_view_subrect(bContext *C, rctf *subrect)
{
  View3D *v3d = CTX_wm_view3d(C);
  ARegion *region = CTX_wm_region(C);

  if (v3d) {
    RegionView3D *rv3d = region->regiondata;

    /* for camera view set the subrect */
    if (rv3d->persp == RV3D_CAMOB) {
      Scene *scene = CTX_data_scene(C);
      Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
      ED_view3d_calc_camera_border(scene, depsgraph, region, v3d, rv3d, subrect, true);
      return 1;
    }
  }

  return 0;
}

/* convert a given grease-pencil layer to a 3d-curve representation
 * (using current view if appropriate) */
static void gpencil_layer_to_curve(bContext *C,
                                   ReportList *reports,
                                   bGPdata *gpd,
                                   bGPDlayer *gpl,
                                   const int mode,
                                   const bool norm_weights,
                                   const float rad_fac,
                                   const bool link_strokes,
                                   tGpTimingData *gtd)
{
  struct Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Collection *collection = CTX_data_collection(C);
  Scene *scene = CTX_data_scene(C);

  bGPDframe *gpf = BKE_gpencil_layer_frame_get(gpl, scene->r.cfra, GP_GETFRAME_USE_PREV);
  bGPDstroke *prev_gps = NULL;
  Object *ob;
  Curve *cu;
  Nurb *nu = NULL;
  Base *base_new = NULL;
  float minmax_weights[2] = {1.0f, 0.0f};

  /* camera framing */
  rctf subrect, *subrect_ptr = NULL;

  /* error checking */
  if (ELEM(NULL, gpd, gpl, gpf)) {
    return;
  }

  /* only convert if there are any strokes on this layer's frame to convert */
  if (BLI_listbase_is_empty(&gpf->strokes)) {
    return;
  }

  /* initialize camera framing */
  if (gpencil_camera_view_subrect(C, &subrect)) {
    subrect_ptr = &subrect;
  }

  /* init the curve object (remove rotation and get curve data from it)
   * - must clear transforms set on object, as those skew our results
   */
  ob = BKE_object_add_only_object(bmain, OB_CURVES_LEGACY, gpl->info);
  cu = ob->data = BKE_curve_add(bmain, gpl->info, OB_CURVES_LEGACY);
  BKE_collection_object_add(bmain, collection, ob);
  base_new = BKE_view_layer_base_find(view_layer, ob);
  DEG_relations_tag_update(bmain); /* added object */

  cu->flag |= CU_3D;
  cu->bevresol = gtd->bevel_resolution;
  cu->bevel_radius = gtd->bevel_depth;

  gtd->inittime = ((bGPDstroke *)gpf->strokes.first)->inittime;

  /* add points to curve */
  LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
    if (gps->totpoints < 1) {
      continue;
    }
    const bool add_start_point = (link_strokes && !(prev_gps));
    const bool add_end_point = (link_strokes && !(gps->next));

    /* Detect new strokes created because of GP_STROKE_BUFFER_MAX reached,
     * and stitch them to previous one. */
    bool stitch = false;
    if (prev_gps) {
      bGPDspoint *pt1 = &prev_gps->points[prev_gps->totpoints - 1];
      bGPDspoint *pt2 = &gps->points[0];

      if ((pt1->x == pt2->x) && (pt1->y == pt2->y)) {
        stitch = true;
      }
    }

    /* Decide whether we connect this stroke to previous one */
    if (!(stitch || link_strokes)) {
      nu = NULL;
    }

    switch (mode) {
      case GP_STROKECONVERT_PATH:
        gpencil_stroke_to_path(C,
                               gpl,
                               gps,
                               cu,
                               subrect_ptr,
                               &nu,
                               minmax_weights,
                               rad_fac,
                               stitch,
                               add_start_point,
                               add_end_point,
                               gtd);
        break;
      case GP_STROKECONVERT_CURVE:
      case GP_STROKECONVERT_POLY: /* convert after */
        gpencil_stroke_to_bezier(C,
                                 gpl,
                                 gps,
                                 cu,
                                 subrect_ptr,
                                 &nu,
                                 minmax_weights,
                                 rad_fac,
                                 stitch,
                                 add_start_point,
                                 add_end_point,
                                 gtd);
        break;
      default:
        BLI_assert_msg(0, "invalid mode");
        break;
    }
    prev_gps = gps;
  }

  /* If link_strokes, be sure first and last points have a zero weight/size! */
  if (link_strokes) {
    gpencil_stroke_finalize_curve_endpoints(cu);
  }

  /* Update curve's weights, if needed */
  if (norm_weights && ((minmax_weights[0] > 0.0f) || (minmax_weights[1] < 1.0f))) {
    gpencil_stroke_norm_curve_weights(cu, minmax_weights);
  }

  /* Create the path animation, if needed */
  gpencil_stroke_path_animation(C, reports, cu, gtd);

  if (mode == GP_STROKECONVERT_POLY) {
    for (nu = cu->nurb.first; nu; nu = nu->next) {
      BKE_nurb_type_convert(nu, CU_POLY, false, NULL);
    }
  }

  ED_object_base_select(base_new, BA_SELECT);

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
}

/* --- */

/* Check a GP layer has valid timing data! Else, most timing options are hidden in the operator.
 * op may be NULL.
 */
static bool gpencil_convert_check_has_valid_timing(bContext *C, bGPDlayer *gpl, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);

  bGPDframe *gpf = NULL;
  bGPDstroke *gps = NULL;
  bGPDspoint *pt;
  double base_time, cur_time, prev_time = -1.0;
  int i;
  bool valid = true;

  if (!gpl || !(gpf = BKE_gpencil_layer_frame_get(gpl, scene->r.cfra, GP_GETFRAME_USE_PREV)) ||
      !(gps = gpf->strokes.first)) {
    return false;
  }

  do {
    base_time = cur_time = gps->inittime;
    if (cur_time <= prev_time) {
      valid = false;
      break;
    }

    prev_time = cur_time;
    for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
      cur_time = base_time + (double)pt->time;
      /* First point of a stroke should have the same time as stroke's inittime,
       * so it's the only case where equality is allowed!
       */
      if ((i && cur_time <= prev_time) || (cur_time < prev_time)) {
        valid = false;
        break;
      }
      prev_time = cur_time;
    }

    if (!valid) {
      break;
    }
  } while ((gps = gps->next));

  if (op) {
    RNA_boolean_set(op->ptr, "use_timing_data", valid);
  }
  return valid;
}

/* Check end_frame is always > start frame! */
static void gpencil_convert_set_end_frame(struct Main *UNUSED(main),
                                          struct Scene *UNUSED(scene),
                                          struct PointerRNA *ptr)
{
  int start_frame = RNA_int_get(ptr, "start_frame");
  int end_frame = RNA_int_get(ptr, "end_frame");

  if (end_frame <= start_frame) {
    RNA_int_set(ptr, "end_frame", start_frame + 1);
  }
}

static bool gpencil_convert_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);

  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
    return false;
  }

  bGPdata *gpd = (bGPdata *)ob->data;
  bGPDlayer *gpl = NULL;
  bGPDframe *gpf = NULL;
  ScrArea *area = CTX_wm_area(C);

  /* only if the current view is 3D View, if there's valid data (i.e. at least one stroke!),
   * and if we are not in edit mode!
   */
  return ((area && area->spacetype == SPACE_VIEW3D) && (gpl = BKE_gpencil_layer_active_get(gpd)) &&
          (gpf = BKE_gpencil_layer_frame_get(gpl, scene->r.cfra, GP_GETFRAME_USE_PREV)) &&
          (gpf->strokes.first) && (!GPENCIL_ANY_EDIT_MODE(gpd)));
}

static int gpencil_convert_layer_exec(bContext *C, wmOperator *op)
{
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "use_timing_data");
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = (bGPdata *)ob->data;

  bGPDlayer *gpl = BKE_gpencil_layer_active_get(gpd);
  Scene *scene = CTX_data_scene(C);
  const int mode = RNA_enum_get(op->ptr, "type");
  const bool norm_weights = RNA_boolean_get(op->ptr, "use_normalize_weights");
  const float rad_fac = RNA_float_get(op->ptr, "radius_multiplier");
  const bool link_strokes = RNA_boolean_get(op->ptr, "use_link_strokes");
  bool valid_timing;
  tGpTimingData gtd;

  /* check if there's data to work with */
  if (gpd == NULL) {
    BKE_report(op->reports, RPT_ERROR, "No Grease Pencil data to work on");
    return OPERATOR_CANCELLED;
  }

  if (!RNA_property_is_set(op->ptr, prop) && !gpencil_convert_check_has_valid_timing(C, gpl, op)) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Current Grease Pencil strokes have no valid timing data, most timing options will "
               "be hidden!");
  }
  valid_timing = RNA_property_boolean_get(op->ptr, prop);

  gtd.mode = RNA_enum_get(op->ptr, "timing_mode");
  /* Check for illegal timing mode! */
  if (!valid_timing &&
      !ELEM(gtd.mode, GP_STROKECONVERT_TIMING_NONE, GP_STROKECONVERT_TIMING_LINEAR)) {
    gtd.mode = GP_STROKECONVERT_TIMING_LINEAR;
    RNA_enum_set(op->ptr, "timing_mode", gtd.mode);
  }
  if (!link_strokes) {
    gtd.mode = GP_STROKECONVERT_TIMING_NONE;
  }

  /* grab all relevant settings */
  gtd.frame_range = RNA_int_get(op->ptr, "frame_range");
  gtd.start_frame = RNA_int_get(op->ptr, "start_frame");
  gtd.bevel_depth = RNA_float_get(op->ptr, "bevel_depth");
  gtd.bevel_resolution = RNA_int_get(op->ptr, "bevel_resolution");
  gtd.realtime = valid_timing ? RNA_boolean_get(op->ptr, "use_realtime") : false;
  gtd.end_frame = RNA_int_get(op->ptr, "end_frame");
  gtd.gap_duration = RNA_float_get(op->ptr, "gap_duration");
  gtd.gap_randomness = RNA_float_get(op->ptr, "gap_randomness");
  gtd.gap_randomness = min_ff(gtd.gap_randomness, gtd.gap_duration);
  gtd.seed = RNA_int_get(op->ptr, "seed");
  gtd.points_num = gtd.cur_point = 0;
  gtd.dists = gtd.times = NULL;
  gtd.tot_dist = gtd.tot_time = gtd.gap_tot_time = 0.0f;
  gtd.inittime = 0.0;
  gtd.offset_time = 0.0f;

  /* perform conversion */
  gpencil_layer_to_curve(
      C, op->reports, gpd, gpl, mode, norm_weights, rad_fac, link_strokes, &gtd);

  /* free temp memory */
  MEM_SAFE_FREE(gtd.dists);
  MEM_SAFE_FREE(gtd.times);

  /* notifiers */
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_OBJECT | NA_ADDED, NULL);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);

  /* done */
  return OPERATOR_FINISHED;
}

static bool gpencil_convert_poll_property(const bContext *UNUSED(C),
                                          wmOperator *op,
                                          const PropertyRNA *prop)
{
  PointerRNA *ptr = op->ptr;
  const char *prop_id = RNA_property_identifier(prop);
  const bool link_strokes = RNA_boolean_get(ptr, "use_link_strokes");
  int timing_mode = RNA_enum_get(ptr, "timing_mode");
  bool realtime = RNA_boolean_get(ptr, "use_realtime");
  float gap_duration = RNA_float_get(ptr, "gap_duration");
  float gap_randomness = RNA_float_get(ptr, "gap_randomness");
  const bool valid_timing = RNA_boolean_get(ptr, "use_timing_data");

  /* Always show those props */
  if (STR_ELEM(prop_id,
               "type",
               "use_normalize_weights",
               "radius_multiplier",
               "use_link_strokes",
               "bevel_depth",
               "bevel_resolution")) {
    return true;
  }

  /* Never show this prop */
  if (STREQ(prop_id, "use_timing_data")) {
    return false;
  }

  if (link_strokes) {
    /* Only show when link_stroke is true */
    if (STREQ(prop_id, "timing_mode")) {
      return true;
    }

    if (timing_mode != GP_STROKECONVERT_TIMING_NONE) {
      /* Only show when link_stroke is true and stroke timing is enabled */
      if (STR_ELEM(prop_id, "frame_range", "start_frame")) {
        return true;
      }

      /* Only show if we have valid timing data! */
      if (valid_timing && STREQ(prop_id, "use_realtime")) {
        return true;
      }

      /* Only show if realtime or valid_timing is false! */
      if ((!realtime || !valid_timing) && STREQ(prop_id, "end_frame")) {
        return true;
      }

      if (valid_timing && timing_mode == GP_STROKECONVERT_TIMING_CUSTOMGAP) {
        /* Only show for custom gaps! */
        if (STREQ(prop_id, "gap_duration")) {
          return true;
        }

        /* Only show randomness for non-null custom gaps! */
        if (STREQ(prop_id, "gap_randomness") && (gap_duration > 0.0f)) {
          return true;
        }

        /* Only show seed for randomize action! */
        if (STREQ(prop_id, "seed") && (gap_duration > 0.0f) && (gap_randomness > 0.0f)) {
          return true;
        }
      }
    }
  }

  /* Else, hidden! */
  return false;
}

void GPENCIL_OT_convert(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Convert Grease Pencil";
  ot->idname = "GPENCIL_OT_convert";
  ot->description = "Convert the active Grease Pencil layer to a new Curve Object";

  /* callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = gpencil_convert_layer_exec;
  ot->poll = gpencil_convert_poll;
  ot->poll_property = gpencil_convert_poll_property;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(
      ot->srna, "type", prop_gpencil_convertmodes, 0, "Type", "Which type of curve to convert to");

  RNA_def_float_distance(
      ot->srna, "bevel_depth", 0.0f, 0.0f, 1000.0f, "Bevel Depth", "", 0.0f, 10.0f);
  RNA_def_int(ot->srna,
              "bevel_resolution",
              0,
              0,
              32,
              "Bevel Resolution",
              "Bevel resolution when depth is non-zero",
              0,
              32);

  RNA_def_boolean(ot->srna,
                  "use_normalize_weights",
                  true,
                  "Normalize Weight",
                  "Normalize weight (set from stroke width)");
  RNA_def_float(ot->srna,
                "radius_multiplier",
                1.0f,
                0.0f,
                1000.0f,
                "Radius Factor",
                "Multiplier for the points' radii (set from stroke width)",
                0.0f,
                10.0f);
  RNA_def_boolean(ot->srna,
                  "use_link_strokes",
                  false,
                  "Link Strokes",
                  "Whether to link strokes with zero-radius sections of curves");

  prop = RNA_def_enum(ot->srna,
                      "timing_mode",
                      prop_gpencil_convert_timingmodes,
                      GP_STROKECONVERT_TIMING_FULL,
                      "Timing Mode",
                      "How to use timing data stored in strokes");
  RNA_def_enum_funcs(prop, rna_GPConvert_mode_items);

  RNA_def_int(ot->srna,
              "frame_range",
              100,
              1,
              10000,
              "Frame Range",
              "The duration of evaluation of the path control curve",
              1,
              1000);
  RNA_def_int(ot->srna,
              "start_frame",
              1,
              1,
              100000,
              "Start Frame",
              "The start frame of the path control curve",
              1,
              100000);
  RNA_def_boolean(ot->srna,
                  "use_realtime",
                  false,
                  "Realtime",
                  "Whether the path control curve reproduces the drawing in realtime, starting "
                  "from Start Frame");
  prop = RNA_def_int(ot->srna,
                     "end_frame",
                     250,
                     1,
                     100000,
                     "End Frame",
                     "The end frame of the path control curve (if Realtime is not set)",
                     1,
                     100000);
  RNA_def_property_update_runtime(prop, gpencil_convert_set_end_frame);

  RNA_def_float(ot->srna,
                "gap_duration",
                0.0f,
                0.0f,
                10000.0f,
                "Gap Duration",
                "Custom Gap mode: (Average) length of gaps, in frames "
                "(Note: Realtime value, will be scaled if Realtime is not set)",
                0.0f,
                1000.0f);
  RNA_def_float(ot->srna,
                "gap_randomness",
                0.0f,
                0.0f,
                10000.0f,
                "Gap Randomness",
                "Custom Gap mode: Number of frames that gap lengths can vary",
                0.0f,
                1000.0f);
  RNA_def_int(ot->srna,
              "seed",
              0,
              0,
              1000,
              "Random Seed",
              "Custom Gap mode: Random generator seed",
              0,
              100);

  /* NOTE: Internal use, this one will always be hidden by UI code... */
  prop = RNA_def_boolean(
      ot->srna,
      "use_timing_data",
      false,
      "Has Valid Timing",
      "Whether the converted Grease Pencil layer has valid timing data (internal use)");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* Generate Grease Pencil from Image. */
static bool image_to_gpencil_poll(bContext *C)
{
  SpaceLink *sl = CTX_wm_space_data(C);
  if ((sl != NULL) && (sl->spacetype == SPACE_IMAGE)) {
    SpaceImage *sima = CTX_wm_space_image(C);
    Image *image = sima->image;
    ImageUser iuser = sima->iuser;
    return BKE_image_has_ibuf(image, &iuser);
  }

  return false;
}

static int image_to_gpencil_exec(bContext *C, wmOperator *op)
{
  const float size = RNA_float_get(op->ptr, "size");
  const bool is_mask = RNA_boolean_get(op->ptr, "mask");

  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  bool done = false;

  if (sima->image == NULL) {
    return OPERATOR_CANCELLED;
  }

  /* Create Object. */
  const float *cur = scene->cursor.location;
  ushort local_view_bits = 0;
  Object *ob = ED_gpencil_add_object(C, cur, local_view_bits);
  DEG_relations_tag_update(bmain); /* added object */

  /* Create material slot. */
  Material *ma = BKE_gpencil_object_material_new(bmain, ob, "Image Material", NULL);
  MaterialGPencilStyle *gp_style = ma->gp_style;
  gp_style->mode = GP_MATERIAL_MODE_SQUARE;

  /* Add layer and frame. */
  bGPdata *gpd = (bGPdata *)ob->data;
  bGPDlayer *gpl = BKE_gpencil_layer_addnew(gpd, "Image Layer", true, false);
  bGPDframe *gpf = BKE_gpencil_frame_addnew(gpl, scene->r.cfra);
  done = BKE_gpencil_from_image(sima, gpd, gpf, size, is_mask);

  if (done) {
    /* Delete any selected point. */
    LISTBASE_FOREACH_MUTABLE (bGPDstroke *, gps, &gpf->strokes) {
      BKE_gpencil_stroke_delete_tagged_points(
          gpd, gpf, gps, gps->next, GP_SPOINT_SELECT, false, false, 0);
    }

    BKE_reportf(op->reports, RPT_INFO, "Object created");
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_image_to_grease_pencil(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Generate Grease Pencil Object using image as source";
  ot->idname = "GPENCIL_OT_image_to_grease_pencil";
  ot->description = "Generate a Grease Pencil Object using Image as source";

  /* api callbacks */
  ot->exec = image_to_gpencil_exec;
  ot->poll = image_to_gpencil_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_float(ot->srna,
                           "size",
                           0.005f,
                           0.0001f,
                           10.0f,
                           "Point Size",
                           "Size used for grease pencil points",
                           0.001f,
                           1.0f);
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "mask",
                         false,
                         "Generate Mask",
                         "Create an inverted image for masking using alpha channel");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

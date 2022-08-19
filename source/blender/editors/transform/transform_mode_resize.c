/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edtransform
 */

#include <stdlib.h>

#include "BLI_math.h"
#include "BLI_task.h"

#include "BKE_context.h"
#include "BKE_image.h"
#include "BKE_unit.h"

#include "ED_screen.h"

#include "UI_interface.h"

#include "transform.h"
#include "transform_constraints.h"
#include "transform_convert.h"
#include "transform_mode.h"
#include "transform_snap.h"

/* -------------------------------------------------------------------- */
/** \name Transform (Resize) Element
 * \{ */

struct ElemResizeData {
  const TransInfo *t;
  const TransDataContainer *tc;
  float mat[3][3];
};

static void element_resize_fn(void *__restrict iter_data_v,
                              const int iter,
                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  struct ElemResizeData *data = iter_data_v;
  TransData *td = &data->tc->data[iter];
  if (td->flag & TD_SKIP) {
    return;
  }
  ElementResize(data->t, data->tc, td, data->mat);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform (Resize)
 * \{ */

static float ResizeBetween(TransInfo *t, const float p1[3], const float p2[3])
{
  float d1[3], d2[3], len_d1;

  sub_v3_v3v3(d1, p1, t->center_global);
  sub_v3_v3v3(d2, p2, t->center_global);

  if (t->con.applyRot != NULL && (t->con.mode & CON_APPLY)) {
    mul_m3_v3(t->con.pmtx, d1);
    mul_m3_v3(t->con.pmtx, d2);
  }

  project_v3_v3v3(d1, d1, d2);

  len_d1 = len_v3(d1);

  /* Use 'invalid' dist when `center == p1` (after projecting),
   * in this case scale will _never_ move the point in relation to the center,
   * so it makes no sense to take it into account when scaling. see: T46503 */
  return len_d1 != 0.0f ? len_v3(d2) / len_d1 : TRANSFORM_DIST_INVALID;
}

static void ApplySnapResize(TransInfo *t, float vec[3])
{
  float point[3];
  getSnapPoint(t, point);

  float dist = ResizeBetween(t, t->tsnap.snapTarget, point);
  if (dist != TRANSFORM_DIST_INVALID) {
    copy_v3_fl(vec, dist);
  }
}

/**
 * Find the correction for the scaling factor when "Constrain to Bounds" is active.
 * \param numerator: How far the UV boundary (unit square) is from the origin of the scale.
 * \param denominator: How far the AABB is from the origin of the scale.
 * \param scale: Scale parameter to update.
 */
static void constrain_scale_to_boundary(const float numerator,
                                        const float denominator,
                                        float *scale)
{
  if (denominator == 0.0f) {
    /* The origin of the scale is on the edge of the boundary. */
    if (numerator < 0.0f) {
      /* Negative scale will wrap around and put us outside the boundary. */
      *scale = 0.0f; /* Hold at the boundary instead. */
    }
    return; /* Nothing else we can do without more info. */
  }

  const float correction = numerator / denominator;
  if (correction < 0.0f || !isfinite(correction)) {
    /* TODO: Correction is negative or invalid, but we lack context to fix `*scale`. */
    return;
  }

  if (denominator < 0.0f) {
    /* Scale origin is outside boundary, only make scale bigger. */
    if (*scale < correction) {
      *scale = correction;
    }
    return;
  }

  /* Scale origin is inside boundary, the "regular" case, limit maximum scale. */
  if (*scale > correction) {
    *scale = correction;
  }
}

static bool clip_uv_transform_resize(TransInfo *t, float vec[2])
{

  /* Stores the coordinates of the closest UDIM tile.
   * Also acts as an offset to the tile from the origin of UV space. */
  float base_offset[2] = {0.0f, 0.0f};

  /* If tiled image then constrain to correct/closest UDIM tile, else 0-1 UV space. */
  const SpaceImage *sima = t->area->spacedata.first;
  BKE_image_find_nearest_tile_with_offset(sima->image, t->center_global, base_offset);

  /* Assume no change is required. */
  float scale = 1.0f;

  /* Are we scaling U and V together, or just one axis? */
  const bool adjust_u = !(t->con.mode & CON_AXIS1);
  const bool adjust_v = !(t->con.mode & CON_AXIS0);
  const bool use_local_center = transdata_check_local_center(t, t->around);
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    for (TransData *td = tc->data; td < tc->data + tc->data_len; td++) {

      /* Get scale origin. */
      const float *scale_origin = use_local_center ? td->center : t->center_global;

      /* Alias td->loc as min and max just in case we need to optimize later. */
      const float *min = td->loc;
      const float *max = td->loc;

      if (adjust_u) {
        /* Update U against the left border. */
        constrain_scale_to_boundary(
            scale_origin[0] - base_offset[0], scale_origin[0] - min[0], &scale);

        /* Now the right border, negated, because `-1.0 / -1.0 = 1.0` */
        constrain_scale_to_boundary(
            base_offset[0] + t->aspect[0] - scale_origin[0], max[0] - scale_origin[0], &scale);
      }

      /* Do the same for the V co-ordinate. */
      if (adjust_v) {
        constrain_scale_to_boundary(
            scale_origin[1] - base_offset[1], scale_origin[1] - min[1], &scale);

        constrain_scale_to_boundary(
            base_offset[1] + t->aspect[1] - scale_origin[1], max[1] - scale_origin[1], &scale);
      }
    }
  }
  vec[0] *= scale;
  vec[1] *= scale;
  return scale != 1.0f;
}

static void applyResize(TransInfo *t, const int UNUSED(mval[2]))
{
  float mat[3][3];
  int i;
  char str[UI_MAX_DRAW_STR];

  if (t->flag & T_INPUT_IS_VALUES_FINAL) {
    copy_v3_v3(t->values_final, t->values);
  }
  else {
    float ratio = t->values[0];

    copy_v3_fl(t->values_final, ratio);
    add_v3_v3(t->values_final, t->values_modal_offset);

    transform_snap_increment(t, t->values_final);

    if (applyNumInput(&t->num, t->values_final)) {
      constraintNumInput(t, t->values_final);
    }

    applySnappingAsGroup(t, t->values_final);
  }

  size_to_mat3(mat, t->values_final);
  if (t->con.mode & CON_APPLY) {
    t->con.applySize(t, NULL, NULL, mat);

    /* Only so we have re-usable value with redo. */
    float pvec[3] = {0.0f, 0.0f, 0.0f};
    int j = 0;
    for (i = 0; i < 3; i++) {
      if (!(t->con.mode & (CON_AXIS0 << i))) {
        t->values_final[i] = 1.0f;
      }
      else {
        pvec[j++] = t->values_final[i];
      }
    }
    headerResize(t, pvec, str, sizeof(str));
  }
  else {
    headerResize(t, t->values_final, str, sizeof(str));
  }

  copy_m3_m3(t->mat, mat); /* used in gizmo */

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {

    if (tc->data_len < TRANSDATA_THREAD_LIMIT) {
      TransData *td = tc->data;
      for (i = 0; i < tc->data_len; i++, td++) {
        if (td->flag & TD_SKIP) {
          continue;
        }

        ElementResize(t, tc, td, mat);
      }
    }
    else {
      struct ElemResizeData data = {
          .t = t,
          .tc = tc,
      };
      copy_m3_m3(data.mat, mat);

      TaskParallelSettings settings;
      BLI_parallel_range_settings_defaults(&settings);
      BLI_task_parallel_range(0, tc->data_len, &data, element_resize_fn, &settings);
    }
  }

  /* Evil hack - redo resize if clipping needed. */
  if (t->flag & T_CLIP_UV && clip_uv_transform_resize(t, t->values_final)) {
    size_to_mat3(mat, t->values_final);

    if (t->con.mode & CON_APPLY) {
      t->con.applySize(t, NULL, NULL, mat);
    }

    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      TransData *td = tc->data;
      for (i = 0; i < tc->data_len; i++, td++) {
        ElementResize(t, tc, td, mat);
      }

      /* In proportional edit it can happen that */
      /* vertices in the radius of the brush end */
      /* outside the clipping area               */
      /* XXX HACK - dg */
      if (t->flag & T_PROP_EDIT) {
        clipUVData(t);
      }
    }
  }

  recalcData(t);

  ED_area_status_text(t->area, str);
}

void initResize(TransInfo *t, float mouse_dir_constraint[3])
{
  t->mode = TFM_RESIZE;
  t->transform = applyResize;
  t->tsnap.applySnap = ApplySnapResize;
  t->tsnap.distance = ResizeBetween;

  if (is_zero_v3(mouse_dir_constraint)) {
    initMouseInputMode(t, &t->mouse, INPUT_SPRING_FLIP);
  }
  else {
    int mval_start[2], mval_end[2];
    float mval_dir[3], t_mval[2];
    float viewmat[3][3];

    copy_m3_m4(viewmat, t->viewmat);
    mul_v3_m3v3(mval_dir, viewmat, mouse_dir_constraint);
    normalize_v2(mval_dir);
    if (is_zero_v2(mval_dir)) {
      /* The screen space direction is orthogonal to the view.
       * Fall back to constraining on the Y axis. */
      mval_dir[0] = 0;
      mval_dir[1] = 1;
    }

    mval_start[0] = t->center2d[0];
    mval_start[1] = t->center2d[1];

    t_mval[0] = t->mval[0] - mval_start[0];
    t_mval[1] = t->mval[1] - mval_start[1];
    project_v2_v2v2(mval_dir, t_mval, mval_dir);

    mval_end[0] = t->center2d[0] + mval_dir[0];
    mval_end[1] = t->center2d[1] + mval_dir[1];

    setCustomPoints(t, &t->mouse, mval_end, mval_start);

    initMouseInputMode(t, &t->mouse, INPUT_CUSTOM_RATIO);
  }

  t->flag |= T_NULL_ONE;
  t->num.val_flag[0] |= NUM_NULL_ONE;
  t->num.val_flag[1] |= NUM_NULL_ONE;
  t->num.val_flag[2] |= NUM_NULL_ONE;
  t->num.flag |= NUM_AFFECT_ALL;
  if ((t->flag & T_EDIT) == 0) {
#ifdef USE_NUM_NO_ZERO
    t->num.val_flag[0] |= NUM_NO_ZERO;
    t->num.val_flag[1] |= NUM_NO_ZERO;
    t->num.val_flag[2] |= NUM_NO_ZERO;
#endif
  }

  t->idx_max = 2;
  t->num.idx_max = 2;
  t->snap[0] = 0.1f;
  t->snap[1] = t->snap[0] * 0.1f;

  copy_v3_fl(t->num.val_inc, t->snap[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE;
  t->num.unit_type[1] = B_UNIT_NONE;
  t->num.unit_type[2] = B_UNIT_NONE;

  transform_mode_default_modal_orientation_set(t, V3D_ORIENT_GLOBAL);
}

/** \} */

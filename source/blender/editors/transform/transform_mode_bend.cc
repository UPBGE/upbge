/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <cstdlib>

#include "MEM_guardedalloc.h"

#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"
#include "BLI_task.hh"

#include "BKE_unit.hh"

#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLT_translation.hh"

#include "UI_interface_types.hh"

#include "transform.hh"
#include "transform_convert.hh"
#include "transform_snap.hh"

#include "transform_mode.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Transform (Bend) Custom Data
 * \{ */

/**
 * Custom data, stored in #TransInfo.custom.mode.data
 */
struct BendCustomData {
  /* All values are in global space. */
  float warp_sta[3];
  float warp_end[3];

  float warp_nor[3];
  float warp_tan[3];

  /* For applying the mouse distance. */
  float warp_init_dist;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform (Bend) Element
 * \{ */

static void transdata_elem_bend(const TransInfo *t,
                                const TransDataContainer *tc,
                                TransData *td,
                                TransDataExtension *td_ext,
                                float angle,
                                const BendCustomData *bend_data,
                                const float warp_sta_local[3],
                                const float /*warp_end_local*/[3],
                                const float warp_end_radius_local[3],
                                const float pivot_local[3],

                                bool is_clamp)
{
  if (UNLIKELY(angle == 0.0f)) {
    copy_v3_v3(td->loc, td->iloc);
    return;
  }

  float vec[3];
  float mat[3][3];
  float delta[3];
  float fac, fac_scaled;

  copy_v3_v3(vec, td->iloc);
  mul_m3_v3(td->mtx, vec);

  fac = line_point_factor_v3(vec, warp_sta_local, warp_end_radius_local);
  if (is_clamp) {
    CLAMP(fac, 0.0f, 1.0f);
  }

  if (t->options & CTX_GPENCIL_STROKES) {
    /* Grease pencil multi-frame falloff. */
    float *gp_falloff = static_cast<float *>(td->extra);
    if (gp_falloff != nullptr) {
      fac_scaled = fac * td->factor * *gp_falloff;
    }
    else {
      fac_scaled = fac * td->factor;
    }
  }
  else {
    fac_scaled = fac * td->factor;
  }

  axis_angle_normalized_to_mat3(mat, bend_data->warp_nor, angle * fac_scaled);
  interp_v3_v3v3(delta, warp_sta_local, warp_end_radius_local, fac_scaled);
  sub_v3_v3(delta, warp_sta_local);

  /* Delta is subtracted, rotation adds back this offset. */
  sub_v3_v3(vec, delta);

  sub_v3_v3(vec, pivot_local);
  mul_m3_v3(mat, vec);
  add_v3_v3(vec, pivot_local);

  mul_m3_v3(td->smtx, vec);

  /* Rotation. */
  if ((t->flag & T_POINTS) == 0) {
    ElementRotation(t, tc, td, td_ext, mat, V3D_AROUND_LOCAL_ORIGINS);
  }

  /* Location. */
  copy_v3_v3(td->loc, vec);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform (Bend)
 * \{ */

static eRedrawFlag handleEventBend(TransInfo * /*t*/, const wmEvent *event)
{
  eRedrawFlag status = TREDRAW_NOTHING;

  if (event->type == MIDDLEMOUSE && event->val == KM_PRESS) {
    status = TREDRAW_HARD;
  }

  return status;
}

static void Bend(TransInfo *t)
{
  float pivot_global[3];
  float warp_end_radius_global[3];
  char str[UI_MAX_DRAW_STR];
  const BendCustomData *bend_data = static_cast<const BendCustomData *>(t->custom.mode.data);
  const bool is_clamp = (t->flag & T_ALT_TRANSFORM) == 0;

  union {
    struct {
      float angle, scale;
    };
    float vector[2];
  } values;

  /* Amount of radians for bend. */
  copy_v2_v2(values.vector, t->values);

#if 0
  snapGrid(t, angle_rad);
#else
  /* Hrmf, snapping radius is using 'angle' steps, need to convert to something else
   * this isn't essential but nicer to give reasonable snapping values for radius. */
  if (t->tsnap.mode & SCE_SNAP_TO_INCREMENT) {
    const float radius_snap = 0.1f;
    const float snap_hack = (t->increment[0] * bend_data->warp_init_dist) / radius_snap;
    values.scale *= snap_hack;
    transform_snap_increment(t, values.vector);
    values.scale /= snap_hack;
  }
#endif

  if (applyNumInput(&t->num, values.vector)) {
    values.scale = values.scale / bend_data->warp_init_dist;
  }

  copy_v2_v2(t->values_final, values.vector);

  /* Header print for NumInput. */
  if (hasNumInput(&t->num)) {
    char c[NUM_STR_REP_LEN * 2];

    outputNumInput(&(t->num), c, t->scene->unit);

    SNPRINTF_UTF8(str,
                  IFACE_("Bend Angle: %s, Radius: %s, Alt: Clamp %s"),
                  &c[0],
                  &c[NUM_STR_REP_LEN],
                  WM_bool_as_string(is_clamp));
  }
  else {
    /* Default header print. */
    SNPRINTF_UTF8(str,
                  IFACE_("Bend Angle: %.3f, Radius: %.4f, Alt: Clamp %s"),
                  RAD2DEGF(values.angle),
                  values.scale * bend_data->warp_init_dist,
                  WM_bool_as_string(is_clamp));
  }

  values.angle *= -1.0f;
  values.scale *= bend_data->warp_init_dist;

  /* Calculate `data->warp_end` from `data->warp_end_init`. */
  copy_v3_v3(warp_end_radius_global, bend_data->warp_end);
  dist_ensure_v3_v3fl(warp_end_radius_global, bend_data->warp_sta, values.scale);
  /* Done. */

  /* Calculate pivot. */
  copy_v3_v3(pivot_global, bend_data->warp_sta);
  if (values.angle > 0.0f) {
    madd_v3_v3fl(pivot_global,
                 bend_data->warp_tan,
                 -values.scale * shell_angle_to_dist(float(M_PI_2) - values.angle));
  }
  else {
    madd_v3_v3fl(pivot_global,
                 bend_data->warp_tan,
                 +values.scale * shell_angle_to_dist(float(M_PI_2) + values.angle));
  }

  /* TODO(@ideasman42): xform, compensate object center. */
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {

    float warp_sta_local[3];
    float warp_end_local[3];
    float warp_end_radius_local[3];
    float pivot_local[3];

    if (tc->use_local_mat) {
      sub_v3_v3v3(warp_sta_local, bend_data->warp_sta, tc->mat[3]);
      sub_v3_v3v3(warp_end_local, bend_data->warp_end, tc->mat[3]);
      sub_v3_v3v3(warp_end_radius_local, warp_end_radius_global, tc->mat[3]);
      sub_v3_v3v3(pivot_local, pivot_global, tc->mat[3]);
    }
    else {
      copy_v3_v3(warp_sta_local, bend_data->warp_sta);
      copy_v3_v3(warp_end_local, bend_data->warp_end);
      copy_v3_v3(warp_end_radius_local, warp_end_radius_global);
      copy_v3_v3(pivot_local, pivot_global);
    }

    threading::parallel_for(IndexRange(tc->data_len), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        TransData *td = &tc->data[i];
        TransDataExtension *td_ext = tc->data_ext ? &tc->data_ext[i] : nullptr;
        if (td->flag & TD_SKIP) {
          continue;
        }
        transdata_elem_bend(t,
                            tc,
                            td,
                            td_ext,
                            values.angle,
                            bend_data,
                            warp_sta_local,
                            warp_end_local,
                            warp_end_radius_local,
                            pivot_local,
                            is_clamp);
      }
    });
  }

  recalc_data(t);

  ED_area_status_text(t->area, str);
}

static void initBend(TransInfo *t, wmOperator * /*op*/)
{
  const float *curs;
  float tvec[3];
  BendCustomData *data;

  t->mode = TFM_BEND;

  initMouseInputMode(t, &t->mouse, INPUT_ANGLE_SPRING);

  t->idx_max = 1;
  t->num.idx_max = 1;
  initSnapAngleIncrements(t);

  copy_v3_fl(t->num.val_inc, t->increment[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_use_radians = (t->scene->unit.system_rotation == USER_UNIT_ROT_RADIANS);
  t->num.unit_type[0] = B_UNIT_ROTATION;
  t->num.unit_type[1] = B_UNIT_LENGTH;

  // copy_v3_v3(t->center, ED_view3d_cursor3d_get(t->scene, t->view));
  if ((t->flag & T_OVERRIDE_CENTER) == 0) {
    calculateCenterCursor(t, t->center_global);
  }
  calculateCenterLocal(t, t->center_global);

  data = MEM_callocN<BendCustomData>(__func__);

  curs = t->scene->cursor.location;
  copy_v3_v3(data->warp_sta, curs);
  ED_view3d_win_to_3d(
      (View3D *)t->area->spacedata.first, t->region, curs, t->mval, data->warp_end);

  copy_v3_v3(data->warp_nor, t->viewinv[2]);
  normalize_v3(data->warp_nor);

  /* Tangent. */
  sub_v3_v3v3(tvec, data->warp_end, data->warp_sta);
  cross_v3_v3v3(data->warp_tan, tvec, data->warp_nor);
  normalize_v3(data->warp_tan);

  data->warp_init_dist = len_v3v3(data->warp_end, data->warp_sta);

  t->custom.mode.data = data;
  t->custom.mode.use_free = true;
}

/** \} */

TransModeInfo TransMode_bend = {
    /*flags*/ T_NO_CONSTRAINT,
    /*init_fn*/ initBend,
    /*transform_fn*/ Bend,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ handleEventBend,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

}  // namespace blender::ed::transform

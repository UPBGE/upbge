/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <cstdlib>

#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"

#include "BKE_nla.hh"
#include "BKE_unit.hh"

#include "ED_screen.hh"

#include "BLT_translation.hh"

#include "UI_interface_types.hh"

#include "transform.hh"
#include "transform_convert.hh"
#include "transform_snap.hh"

#include "transform_mode.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Transform (Animation Time Scale)
 * \{ */

static void timescale_snap_apply_fn(TransInfo *t, float vec[3])
{
  float point[3];
  getSnapPoint(t, point);
  const float fac = (point[0] - t->center_global[0]) /
                    (t->tsnap.snap_source[0] - t->center_global[0]);
  vec[0] = fac;
}

static void headerTimeScale(TransInfo *t, char str[UI_MAX_DRAW_STR])
{
  char tvec[NUM_STR_REP_LEN * 3];

  if (hasNumInput(&t->num)) {
    outputNumInput(&(t->num), tvec, t->scene->unit);
  }
  else {
    BLI_snprintf_utf8(&tvec[0], NUM_STR_REP_LEN, "%.4f", t->values_final[0]);
  }

  BLI_snprintf_utf8(str, UI_MAX_DRAW_STR, IFACE_("ScaleX: %s"), &tvec[0]);
}

static void applyTimeScaleValue(TransInfo *t, float value)
{
  Scene *scene = t->scene;

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData *td = tc->data;
    TransData2D *td2d = tc->data_2d;
    for (int i = 0; i < tc->data_len; i++, td++, td2d++) {
      /* It is assumed that td->extra is a pointer to the AnimData,
       * whose active action is where this keyframe comes from
       * (this is only valid when not in NLA). */
      AnimData *adt = static_cast<AnimData *>((t->spacetype != SPACE_NLA) ? td->extra : nullptr);
      float startx = scene->r.cfra;
      float fac = value;

      /* Take proportional editing into account. */
      fac = ((fac - 1.0f) * td->factor) + 1;

      /* Check if any need to apply nla-mapping. */
      if (adt) {
        startx = BKE_nla_tweakedit_remap(adt, startx, NLATIME_CONVERT_UNMAP);
      }

      /* Now, calculate the new value. */
      td->loc[0] = ((td->iloc[0] - startx) * fac) + startx;
    }
  }
}

static void applyTimeScale(TransInfo *t)
{
  char str[UI_MAX_DRAW_STR];

  /* Handle numeric-input stuff. */
  t->vec[0] = t->values[0];
  applyNumInput(&t->num, &t->vec[0]);

  transform_snap_mixed_apply(t, &t->vec[0]);

  t->values_final[0] = t->vec[0];
  headerTimeScale(t, str);

  applyTimeScaleValue(t, t->values_final[0]);

  recalc_data(t);

  ED_area_status_text(t->area, str);
}

static void timescale_transform_matrix_fn(TransInfo *t, float mat_xform[4][4])
{
  const float i_loc = mat_xform[3][0];
  const float startx = t->center_global[0];
  const float fac = t->values_final[0];
  const float loc = ((i_loc - startx) * fac) + startx;
  mat_xform[3][0] = loc;
}

static void initTimeScale(TransInfo *t, wmOperator * /*op*/)
{
  float center[2];

  /* This tool is only really available in the Action Editor
   * AND NLA Editor (for strip scaling). */
  if (ELEM(t->spacetype, SPACE_ACTION, SPACE_NLA) == 0) {
    t->state = TRANS_CANCEL;
  }

  t->mode = TFM_TIME_SCALE;

  /* Recalculate center2d to use scene->r.cfra and mouse Y, since that's
   * what is used in time scale. */
  if ((t->flag & T_OVERRIDE_CENTER) == 0) {
    t->center_global[0] = t->scene->r.cfra;
    projectFloatView(t, t->center_global, center);
    center[1] = t->mouse.imval[1];
  }

  /* Force a reinitialize with the center2d used here. */
  initMouseInput(t, &t->mouse, center, t->mouse.imval, false);

  initMouseInputMode(t, &t->mouse, INPUT_SPRING_FLIP);

  t->num.val_flag[0] |= NUM_NULL_ONE;

  /* Numeric-input has max of (n-1). */
  t->idx_max = 0;
  t->num.flag = 0;
  t->num.idx_max = t->idx_max;

  /* Initialize snap like for everything else. */
  t->increment[0] = 1.0f;
  t->increment_precision = 1.0f;

  copy_v3_fl(t->num.val_inc, t->increment[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE;
}

/** \} */

TransModeInfo TransMode_timescale = {
    /*flags*/ T_NULL_ONE,
    /*init_fn*/ initTimeScale,
    /*transform_fn*/ applyTimeScale,
    /*transform_matrix_fn*/ timescale_transform_matrix_fn,
    /*handle_event_fn*/ nullptr,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ timescale_snap_apply_fn,
    /*draw_fn*/ nullptr,
};

}  // namespace blender::ed::transform

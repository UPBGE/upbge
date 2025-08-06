/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <cstdlib>

#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"

#include "BKE_unit.hh"

#include "ED_screen.hh"

#include "UI_interface_types.hh"

#include "BLT_translation.hh"

#include "transform.hh"
#include "transform_constraints.hh"
#include "transform_convert.hh"
#include "transform_snap.hh"

#include "transform_mode.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Transform (EditBone B-Bone width scaling)
 * \{ */

static void headerBoneSize(TransInfo *t, const float vec[3], char str[UI_MAX_DRAW_STR])
{
  char tvec[NUM_STR_REP_LEN * 3];
  if (hasNumInput(&t->num)) {
    outputNumInput(&(t->num), tvec, t->scene->unit);
  }
  else {
    BLI_snprintf_utf8(&tvec[0], NUM_STR_REP_LEN, "%.4f", vec[0]);
    BLI_snprintf_utf8(&tvec[NUM_STR_REP_LEN], NUM_STR_REP_LEN, "%.4f", vec[1]);
    BLI_snprintf_utf8(&tvec[NUM_STR_REP_LEN * 2], NUM_STR_REP_LEN, "%.4f", vec[2]);
  }

  /* Hmm... perhaps the y-axis values don't need to be shown? */
  if (t->con.mode & CON_APPLY) {
    if (t->num.idx_max == 0) {
      BLI_snprintf_utf8(
          str, UI_MAX_DRAW_STR, IFACE_("ScaleB: %s%s %s"), &tvec[0], t->con.text, t->proptext);
    }
    else {
      BLI_snprintf_utf8(str,
                        UI_MAX_DRAW_STR,
                        IFACE_("ScaleB: %s : %s : %s%s %s"),
                        &tvec[0],
                        &tvec[NUM_STR_REP_LEN],
                        &tvec[NUM_STR_REP_LEN * 2],
                        t->con.text,
                        t->proptext);
    }
  }
  else {
    BLI_snprintf_utf8(str,
                      UI_MAX_DRAW_STR,
                      IFACE_("ScaleB X: %s  Y: %s  Z: %s%s %s"),
                      &tvec[0],
                      &tvec[NUM_STR_REP_LEN],
                      &tvec[NUM_STR_REP_LEN * 2],
                      t->con.text,
                      t->proptext);
  }
}

static void ElementBoneSize(TransInfo *t,
                            TransDataContainer *tc,
                            TransData *td,
                            const float mat[3][3])
{
  float tmat[3][3], smat[3][3], oldy;
  float sizemat[3][3];

  mul_m3_m3m3(smat, mat, td->mtx);
  mul_m3_m3m3(tmat, td->smtx, smat);

  if (t->con.applySize) {
    t->con.applySize(t, tc, td, tmat);
  }

  /* We've tucked the scale in loc. */
  oldy = td->iloc[1];
  size_to_mat3(sizemat, td->iloc);
  mul_m3_m3m3(tmat, tmat, sizemat);
  mat3_to_size(td->loc, tmat);
  td->loc[1] = oldy;
}

static void applyBoneSize(TransInfo *t)
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
  }

  size_to_mat3(mat, t->values_final);

  if (t->con.applySize) {
    t->con.applySize(t, nullptr, nullptr, mat);
    for (i = 0; i < 3; i++) {
      if (!(t->con.mode & (CON_AXIS0 << i))) {
        t->values_final[i] = 1.0f;
      }
    }
  }

  copy_m3_m3(t->mat, mat); /* Used in gizmo. */

  headerBoneSize(t, t->values_final, str);

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData *td = tc->data;
    for (i = 0; i < tc->data_len; i++, td++) {
      if (td->flag & TD_SKIP) {
        continue;
      }

      ElementBoneSize(t, tc, td, mat);
    }
  }

  recalc_data(t);

  ED_area_status_text(t->area, str);
}

static void initBoneSize(TransInfo *t, wmOperator * /*op*/)
{
  t->mode = TFM_BONESIZE;

  initMouseInputMode(t, &t->mouse, INPUT_SPRING_FLIP);

  t->idx_max = 2;
  t->num.idx_max = 2;
  t->num.val_flag[0] |= NUM_NULL_ONE;
  t->num.val_flag[1] |= NUM_NULL_ONE;
  t->num.val_flag[2] |= NUM_NULL_ONE;
  t->num.flag |= NUM_AFFECT_ALL;
  t->increment = float3(0.1f);
  t->increment_precision = 0.1f;

  copy_v3_fl(t->num.val_inc, t->increment[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE;
  t->num.unit_type[1] = B_UNIT_NONE;
  t->num.unit_type[2] = B_UNIT_NONE;
}

/** \} */

TransModeInfo TransMode_bboneresize = {
    /*flags*/ 0,
    /*init_fn*/ initBoneSize,
    /*transform_fn*/ applyBoneSize,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ nullptr,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

}  // namespace blender::ed::transform

/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <cstdlib>

#include "BLI_math_bits.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"

#include "ED_screen.hh"

#include "BLT_translation.hh"

#include "UI_interface_types.hh"

#include "transform.hh"
#include "transform_convert.hh"

#include "transform_mode.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Transform (Mirror)
 * \{ */

/**
 * Mirrors an object by negating the scale of the object on the mirror axis, reflecting the
 * location and adjusting the rotation.
 *
 * \param axis: Either the axis to mirror on (0 = x, 1 = y, 2 = z) in transform space or -1 for no
 * axis mirror.
 * \param flip: If true, a mirror on all axis will be performed additionally (point
 * reflection).
 */
static void ElementMirror(TransInfo *t, TransDataContainer *tc, int td_index, int axis, bool flip)
{
  TransData *td = &tc->data[td_index];

  if ((t->flag & T_V3D_ALIGN) == 0 && tc->data_ext) {
    TransDataExtension *td_ext = &tc->data_ext[td_index];

    /* Size checked needed since the 3D cursor only uses rotation fields. */
    if (td_ext->scale) {
      float fscale[] = {1.0, 1.0, 1.0};

      if (axis >= 0) {
        fscale[axis] = -fscale[axis];
      }
      if (flip) {
        negate_v3(fscale);
      }

      protectedScaleBits(td->protectflag, fscale);

      mul_v3_v3v3(td_ext->scale, td_ext->iscale, fscale);

      constraintScaleLim(t, tc, td_index);
    }

    float rmat[3][3];
    if (axis >= 0) {
      float imat[3][3];
      mul_m3_m3m3(rmat, t->spacemtx_inv, td->axismtx);
      rmat[axis][0] = -rmat[axis][0];
      rmat[axis][1] = -rmat[axis][1];
      rmat[axis][2] = -rmat[axis][2];
      rmat[0][axis] = -rmat[0][axis];
      rmat[1][axis] = -rmat[1][axis];
      rmat[2][axis] = -rmat[2][axis];
      invert_m3_m3(imat, td->axismtx);
      mul_m3_m3m3(rmat, rmat, imat);
      mul_m3_m3m3(rmat, t->spacemtx, rmat);

      ElementRotation_ex(t, tc, td, td_ext, rmat, td->center);

      if (td_ext->rotAngle) {
        *td_ext->rotAngle = -td_ext->irotAngle;
      }
    }
    else {
      unit_m3(rmat);
      ElementRotation_ex(t, tc, td, td_ext, rmat, td->center);

      if (td_ext->rotAngle) {
        *td_ext->rotAngle = td_ext->irotAngle;
      }
    }
  }

  if ((td->flag & TD_NO_LOC) == 0) {
    float center[3], vec[3];

    /* Local constraint shouldn't alter center. */
    if (transdata_check_local_center(t, t->around)) {
      copy_v3_v3(center, td->center);
    }
    else if (t->options & CTX_MOVIECLIP) {
      if (td->flag & TD_INDIVIDUAL_SCALE) {
        copy_v3_v3(center, td->center);
      }
      else {
        copy_v3_v3(center, tc->center_local);
      }
    }
    else {
      copy_v3_v3(center, tc->center_local);
    }

    /* For individual element center, Editmode need to use iloc. */
    if (t->flag & T_POINTS) {
      sub_v3_v3v3(vec, td->iloc, center);
    }
    else {
      sub_v3_v3v3(vec, td->center, center);
    }

    if (axis >= 0) {
      /* Always do the mirror in global space. */
      if (t->flag & T_EDIT) {
        mul_m3_v3(td->mtx, vec);
      }
      reflect_v3_v3v3(vec, vec, t->spacemtx[axis]);
      if (t->flag & T_EDIT) {
        mul_m3_v3(td->smtx, vec);
      }
    }
    if (flip) {
      negate_v3(vec);
    }

    add_v3_v3(vec, center);
    if (t->flag & T_POINTS) {
      sub_v3_v3(vec, td->iloc);
    }
    else {
      sub_v3_v3(vec, td->center);
    }

    if (t->options & (CTX_OBJECT | CTX_POSE_BONE)) {
      mul_m3_v3(td->smtx, vec);
    }

    protectedTransBits(td->protectflag, vec);
    if (td->loc) {
      add_v3_v3v3(td->loc, td->iloc, vec);
    }

    constraintTransLim(t, tc, td);
  }
}

static void applyMirror(TransInfo *t)
{
  int i;
  char str[UI_MAX_DRAW_STR];
  copy_v3_v3(t->values_final, t->values);

  /* OPTIMIZATION:
   * This still recalculates transformation on mouse move
   * while it should only recalculate on constraint change. */

  /* If an axis has been selected. */
  if (t->con.mode & CON_APPLY) {
    /* #special_axis is either the constraint plane normal or the constraint axis.
     * Assuming that CON_AXIS0 < CON_AXIS1 < CON_AXIS2 and CON_AXIS2 is CON_AXIS0 << 2 */
    BLI_assert(CON_AXIS2 == CON_AXIS0 << 2);
    int axis_bitmap = (t->con.mode & (CON_AXIS0 | CON_AXIS1 | CON_AXIS2)) / CON_AXIS0;
    int special_axis_bitmap = 0;
    int special_axis = -1;
    int bitmap_len = count_bits_i(axis_bitmap);
    if (LIKELY(!ELEM(bitmap_len, 0, 3))) {
      special_axis_bitmap = (bitmap_len == 2) ? ~axis_bitmap : axis_bitmap;
      special_axis = bitscan_forward_i(special_axis_bitmap);
    }

    SNPRINTF_UTF8(str, IFACE_("Mirror%s"), t->con.text);

    if (t->options & CTX_SEQUENCER_IMAGE) {
      if (axis_bitmap == 1) {
        t->values_final[0] = -1;
        t->values_final[1] = 1;
      }
      if (axis_bitmap == 2) {
        t->values_final[0] = 1;
        t->values_final[1] = -1;
      }
    }

    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      TransData *td = tc->data;
      for (i = 0; i < tc->data_len; i++, td++) {
        if (td->flag & TD_SKIP) {
          continue;
        }

        ElementMirror(t, tc, i, special_axis, bitmap_len >= 2);
      }
    }

    recalc_data(t);

    ED_area_status_text(t->area, str);
  }
  else {
    if (t->options & CTX_SEQUENCER_IMAGE) {
      t->values_final[0] = 1.0f;
      t->values_final[1] = 1.0f;
    }
    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      TransData *td = tc->data;
      for (i = 0; i < tc->data_len; i++, td++) {
        if (td->flag & TD_SKIP) {
          continue;
        }

        ElementMirror(t, tc, i, -1, false);
      }
    }

    recalc_data(t);

    if (t->flag & T_2D_EDIT) {
      ED_area_status_text(t->area, IFACE_("Select a mirror axis (X, Y)"));
    }
    else {
      ED_area_status_text(t->area, IFACE_("Select a mirror axis (X, Y, Z)"));
    }
  }
}

static void initMirror(TransInfo *t, wmOperator * /*op*/)
{
  initMouseInputMode(t, &t->mouse, INPUT_NONE);
}

/** \} */

TransModeInfo TransMode_mirror = {
    /*flags*/ T_NULL_ONE,
    /*init_fn*/ initMirror,
    /*transform_fn*/ applyMirror,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ nullptr,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

}  // namespace blender::ed::transform

/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <cstdlib>

#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"

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
/** \name Transform (Bone Envelope)
 * \{ */

static void applyBoneEnvelope(TransInfo *t)
{
  float ratio;
  int i;
  char str[UI_MAX_DRAW_STR];

  ratio = t->values[0] + t->values_modal_offset[0];

  transform_snap_increment(t, &ratio);

  applyNumInput(&t->num, &ratio);

  t->values_final[0] = ratio;

  /* Header print for NumInput. */
  if (hasNumInput(&t->num)) {
    char c[NUM_STR_REP_LEN];

    outputNumInput(&(t->num), c, t->scene->unit);
    SNPRINTF_UTF8(str, IFACE_("Envelope: %s"), c);
  }
  else {
    SNPRINTF_UTF8(str, IFACE_("Envelope: %3f"), ratio);
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData *td = tc->data;
    for (i = 0; i < tc->data_len; i++, td++) {
      if (td->flag & TD_SKIP) {
        continue;
      }

      if (td->val) {
        /* If the old/original value was 0.0f, then just use ratio. */
        if (td->ival) {
          *td->val = td->ival * ratio;
        }
        else {
          *td->val = ratio;
        }
      }
    }
  }

  recalc_data(t);

  ED_area_status_text(t->area, str);
}

static void initBoneEnvelope(TransInfo *t, wmOperator * /*op*/)
{
  initMouseInputMode(t, &t->mouse, INPUT_SPRING);

  t->idx_max = 0;
  t->num.idx_max = 0;
  t->increment[0] = 0.1f;
  t->increment_precision = 0.1f;

  copy_v3_fl(t->num.val_inc, t->increment[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE;
}

/** \} */

TransModeInfo TransMode_boneenvelope = {
    /*flags*/ T_NO_CONSTRAINT | T_NO_PROJECT,
    /*init_fn*/ initBoneEnvelope,
    /*transform_fn*/ applyBoneEnvelope,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ nullptr,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

}  // namespace blender::ed::transform

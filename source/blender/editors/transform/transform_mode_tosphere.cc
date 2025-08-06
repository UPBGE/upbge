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
#include "BLI_task.hh"

#include "MEM_guardedalloc.h"

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
/** \name To Sphere Utilities
 * \{ */

struct ToSphereInfo {
  float prop_size_prev;
  float radius;
};

/** Calculate average radius. */
static void to_sphere_radius_update(TransInfo *t)
{
  ToSphereInfo *data = static_cast<ToSphereInfo *>(t->custom.mode.data);
  float radius = 0.0f;
  float vec[3];

  const bool is_local_center = transdata_check_local_center(t, t->around);
  const bool is_data_space = (t->options & CTX_POSE_BONE) != 0;

  if (t->flag & T_PROP_EDIT_ALL) {
    int factor_accum = 0.0f;
    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      TransData *td = tc->data;
      for (int i = 0; i < tc->data_len; i++, td++) {
        if (td->factor == 0.0f) {
          continue;
        }
        const float *center = is_local_center ? td->center : tc->center_local;
        if (is_data_space) {
          copy_v3_v3(vec, td->center);
        }
        else {
          copy_v3_v3(vec, td->iloc);
        }

        sub_v3_v3(vec, center);
        radius += td->factor * len_v3(vec);
        factor_accum += td->factor;
      }
    }
    if (factor_accum != 0.0f) {
      radius /= factor_accum;
    }
  }
  else {
    FOREACH_TRANS_DATA_CONTAINER (t, tc) {
      TransData *td = tc->data;
      for (int i = 0; i < tc->data_len; i++, td++) {
        const float *center = is_local_center ? td->center : tc->center_local;
        if (is_data_space) {
          copy_v3_v3(vec, td->center);
        }
        else {
          copy_v3_v3(vec, td->iloc);
        }

        sub_v3_v3(vec, center);
        radius += len_v3(vec);
      }
    }
    radius /= float(t->data_len_all);
  }

  data->prop_size_prev = t->prop_size;
  data->radius = radius;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform (ToSphere) Element
 * \{ */

static void transdata_elem_to_sphere(const TransInfo * /*t*/,
                                     const TransDataContainer *tc,
                                     TransData *td,
                                     const float ratio,
                                     const ToSphereInfo *to_sphere_info,
                                     const bool is_local_center,
                                     const bool is_data_space)
{
  float vec[3];
  const float *center = is_local_center ? td->center : tc->center_local;
  if (is_data_space) {
    copy_v3_v3(vec, td->center);
  }
  else {
    copy_v3_v3(vec, td->iloc);
  }

  sub_v3_v3(vec, center);
  const float radius = normalize_v3(vec);
  const float tratio = ratio * td->factor;
  mul_v3_fl(vec, radius * (1.0f - tratio) + to_sphere_info->radius * tratio);
  add_v3_v3(vec, center);

  if (is_data_space) {
    sub_v3_v3(vec, td->center);
    mul_m3_v3(td->smtx, vec);
    add_v3_v3(vec, td->iloc);
  }

  copy_v3_v3(td->loc, vec);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform (ToSphere)
 * \{ */

static void applyToSphere(TransInfo *t)
{
  const bool is_local_center = transdata_check_local_center(t, t->around);
  const bool is_data_space = (t->options & CTX_POSE_BONE) != 0;

  float ratio;
  char str[UI_MAX_DRAW_STR];

  ratio = t->values[0] + t->values_modal_offset[0];

  transform_snap_increment(t, &ratio);

  applyNumInput(&t->num, &ratio);

  CLAMP(ratio, 0.0f, 1.0f);

  t->values_final[0] = ratio;

  /* Header print for NumInput. */
  if (hasNumInput(&t->num)) {
    char c[NUM_STR_REP_LEN];

    outputNumInput(&(t->num), c, t->scene->unit);

    SNPRINTF_UTF8(str, IFACE_("To Sphere: %s %s"), c, t->proptext);
  }
  else {
    /* Default header print. */
    SNPRINTF_UTF8(str, IFACE_("To Sphere: %.4f %s"), ratio, t->proptext);
  }

  const ToSphereInfo *to_sphere_info = static_cast<const ToSphereInfo *>(t->custom.mode.data);
  if (to_sphere_info->prop_size_prev != t->prop_size) {
    to_sphere_radius_update(t);
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    threading::parallel_for(IndexRange(tc->data_len), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        TransData *td = &tc->data[i];
        if (td->flag & TD_SKIP) {
          continue;
        }
        transdata_elem_to_sphere(t, tc, td, ratio, to_sphere_info, is_local_center, is_data_space);
      }
    });
  }

  recalc_data(t);

  ED_area_status_text(t->area, str);
}

static void initToSphere(TransInfo *t, wmOperator * /*op*/)
{
  t->mode = TFM_TOSPHERE;

  initMouseInputMode(t, &t->mouse, INPUT_HORIZONTAL_RATIO);

  t->idx_max = 0;
  t->num.idx_max = 0;
  t->increment[0] = 0.1f;
  t->increment_precision = 0.1f;

  copy_v3_fl(t->num.val_inc, t->increment[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE;

  t->num.val_flag[0] |= NUM_NULL_ONE | NUM_NO_NEGATIVE;

  ToSphereInfo *data = MEM_callocN<ToSphereInfo>(__func__);
  t->custom.mode.data = data;
  t->custom.mode.use_free = true;

  to_sphere_radius_update(t);
}

/** \} */

TransModeInfo TransMode_tosphere = {
    /*flags*/ T_NO_CONSTRAINT,
    /*init_fn*/ initToSphere,
    /*transform_fn*/ applyToSphere,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ nullptr,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

}  // namespace blender::ed::transform

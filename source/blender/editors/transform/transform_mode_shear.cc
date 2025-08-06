/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"
#include "BLI_task.hh"

#include "BKE_unit.hh"

#include "ED_screen.hh"

#include "UI_interface.hh"

#include "BLT_translation.hh"

#include "transform.hh"
#include "transform_convert.hh"
#include "transform_snap.hh"

#include "transform_mode.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Transform (Shear) Element
 * \{ */

static void transdata_elem_shear(const TransInfo *t,
                                 const TransDataContainer *tc,
                                 TransData *td,
                                 const float mat_final[3][3],
                                 const bool is_local_center)
{
  float tmat[3][3];
  const float *center;
  if (t->flag & T_EDIT) {
    mul_m3_series(tmat, td->smtx, mat_final, td->mtx);
  }
  else {
    copy_m3_m3(tmat, mat_final);
  }

  if (is_local_center) {
    center = td->center;
  }
  else {
    center = tc->center_local;
  }

  float vec[3];
  sub_v3_v3v3(vec, td->iloc, center);
  mul_m3_v3(tmat, vec);
  add_v3_v3(vec, center);
  sub_v3_v3(vec, td->iloc);

  if (t->options & CTX_GPENCIL_STROKES) {
    /* Grease pencil multi-frame falloff. */
    float *gp_falloff = static_cast<float *>(td->extra);
    if (gp_falloff != nullptr) {
      mul_v3_fl(vec, td->factor * *gp_falloff);
    }
    else {
      mul_v3_fl(vec, td->factor);
    }
  }
  else {
    mul_v3_fl(vec, td->factor);
  }

  add_v3_v3v3(td->loc, td->iloc, vec);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform (Shear)
 * \{ */

struct ShearCustomData {
  bool update_status_bar;
  wmOperator *op;
};

static void initShear_mouseInputMode(TransInfo *t)
{
  float dir[3];
  bool dir_flip = false;
  copy_v3_v3(dir, t->spacemtx[t->orient_axis_ortho]);

  /* Needed for axis aligned view gizmo. */
  if (t->orient[t->orient_curr].type == V3D_ORIENT_VIEW) {
    if (t->orient_axis_ortho == 0) {
      if (t->center2d[1] > t->mouse.imval[1]) {
        dir_flip = !dir_flip;
      }
    }
    else if (t->orient_axis_ortho == 1) {
      if (t->center2d[0] > t->mouse.imval[0]) {
        dir_flip = !dir_flip;
      }
    }
  }

  /* Without this, half the gizmo handles move in the opposite direction. */
  if ((t->orient_axis_ortho + 1) % 3 != t->orient_axis) {
    dir_flip = !dir_flip;
  }

  if (dir_flip) {
    negate_v3(dir);
  }

  mul_mat3_m4_v3(t->viewmat, dir);
  if (normalize_v2(dir) == 0.0f) {
    dir[0] = 1.0f;
  }
  setCustomPointsFromDirection(t, &t->mouse, dir);

  initMouseInputMode(t, &t->mouse, INPUT_CUSTOM_RATIO);
}

static eRedrawFlag handleEventShear(TransInfo *t, const wmEvent *event)
{
  eRedrawFlag status = TREDRAW_NOTHING;

  if (event->type == MIDDLEMOUSE && event->val == KM_PRESS) {
    /* Use custom.mode.data pointer to signal Shear direction. */
    do {
      t->orient_axis_ortho = (t->orient_axis_ortho + 1) % 3;
    } while (t->orient_axis_ortho == t->orient_axis);

    initShear_mouseInputMode(t);

    status = TREDRAW_HARD;
  }
  else if (event->type == EVT_XKEY && event->val == KM_PRESS) {
    t->orient_axis_ortho = (t->orient_axis + 1) % 3;
    initShear_mouseInputMode(t);

    status = TREDRAW_HARD;
  }
  else if (event->type == EVT_YKEY && event->val == KM_PRESS) {
    t->orient_axis_ortho = (t->orient_axis + 2) % 3;
    initShear_mouseInputMode(t);

    status = TREDRAW_HARD;
  }

  ShearCustomData *custom_data = static_cast<ShearCustomData *>(t->custom.mode.data);
  bool is_event_handled = (event->type != MOUSEMOVE) && (status || t->redraw);
  custom_data->update_status_bar = custom_data->update_status_bar || is_event_handled;

  return status;
}

static void apply_shear_value(TransInfo *t, const float value)
{
  float smat[3][3];
  unit_m3(smat);
  smat[1][0] = value;

  float axismat_inv[3][3];
  copy_v3_v3(axismat_inv[0], t->spacemtx[t->orient_axis_ortho]);
  copy_v3_v3(axismat_inv[2], t->spacemtx[t->orient_axis]);
  cross_v3_v3v3(axismat_inv[1], axismat_inv[0], axismat_inv[2]);
  float axismat[3][3];
  invert_m3_m3(axismat, axismat_inv);

  float mat_final[3][3];
  mul_m3_series(mat_final, axismat_inv, smat, axismat);

  const bool is_local_center = transdata_check_local_center(t, t->around);

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    threading::parallel_for(IndexRange(tc->data_len), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        TransData *td = &tc->data[i];
        if (td->flag & TD_SKIP) {
          continue;
        }
        transdata_elem_shear(t, tc, td, mat_final, is_local_center);
      }
    });
  }
}

static bool uv_shear_in_clip_bounds_test(const TransInfo *t, const float value)
{
  const int axis = t->orient_axis_ortho;
  if (axis < 0 || 1 < axis) {
    return true; /* Non standard axis, nothing to do. */
  }
  const float *center = t->center_global;
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    TransData *td = tc->data;
    for (int i = 0; i < tc->data_len; i++, td++) {
      if (td->flag & TD_SKIP) {
        continue;
      }
      if (td->factor < 1.0f) {
        continue; /* Proportional edit, will get picked up in next phase. */
      }

      float uv[2];
      sub_v2_v2v2(uv, td->iloc, center);
      uv[axis] = uv[axis] + value * uv[1 - axis] * (2 * axis - 1);
      add_v2_v2(uv, center);
      /* TODO: UDIM support. */
      if (uv[axis] < 0.0f || 1.0f < uv[axis]) {
        return false;
      }
    }
  }
  return true;
}

static bool clip_uv_transform_shear(const TransInfo *t, float *vec, float *vec_inside_bounds)
{
  float value = vec[0];
  if (uv_shear_in_clip_bounds_test(t, value)) {
    vec_inside_bounds[0] = value; /* Store for next iteration. */
    return false;                 /* Nothing to do. */
  }
  float value_inside_bounds = vec_inside_bounds[0];
  if (!uv_shear_in_clip_bounds_test(t, value_inside_bounds)) {
    return false; /* No known way to fix, may as well shear anyway. */
  }
  const int max_i = 32; /* Limit iteration, mainly for debugging. */
  for (int i = 0; i < max_i; i++) {
    /* Binary search. */
    const float value_mid = (value_inside_bounds + value) / 2.0f;
    if (ELEM(value_mid, value_inside_bounds, value)) {
      break; /* Float precision reached. */
    }
    if (uv_shear_in_clip_bounds_test(t, value_mid)) {
      value_inside_bounds = value_mid;
    }
    else {
      value = value_mid;
    }
  }

  vec_inside_bounds[0] = value_inside_bounds; /* Store for next iteration. */
  vec[0] = value_inside_bounds;               /* Update shear value. */
  return true;
}

static void apply_shear(TransInfo *t)
{
  float value = t->values[0] + t->values_modal_offset[0];
  transform_snap_increment(t, &value);
  applyNumInput(&t->num, &value);
  t->values_final[0] = value;

  apply_shear_value(t, value);

  if (t->flag & T_CLIP_UV) {
    if (clip_uv_transform_shear(t, t->values_final, t->values_inside_constraints)) {
      apply_shear_value(t, t->values_final[0]);
    }

    /* Not ideal, see #clipUVData code-comment. */
    if (t->flag & T_PROP_EDIT) {
      clipUVData(t);
    }
  }

  recalc_data(t);

  char str[UI_MAX_DRAW_STR];
  /* Header print for NumInput. */
  if (hasNumInput(&t->num)) {
    char c[NUM_STR_REP_LEN];
    outputNumInput(&(t->num), c, t->scene->unit);
    SNPRINTF_UTF8(str, IFACE_("Shear: %s %s"), c, t->proptext);
  }
  else {
    /* Default header print. */
    SNPRINTF_UTF8(str, IFACE_("Shear: %.3f %s"), value, t->proptext);
  }

  ED_area_status_text(t->area, str);

  ShearCustomData *custom_data = static_cast<ShearCustomData *>(t->custom.mode.data);
  if (custom_data->op && custom_data->update_status_bar) {
    custom_data->update_status_bar = false;

    WorkspaceStatus status(t->context);

    status.opmodal(IFACE_("Confirm"), custom_data->op->type, TFM_MODAL_CONFIRM);
    status.opmodal(IFACE_("Cancel"), custom_data->op->type, TFM_MODAL_CANCEL);

    status.item_bool({}, t->orient_axis_ortho == (t->orient_axis + 1) % 3, ICON_EVENT_X);
    status.item_bool({}, t->orient_axis_ortho == (t->orient_axis + 2) % 3, ICON_EVENT_Y);
    status.item(IFACE_("Shear Axis"), ICON_NONE);
    status.item(IFACE_("Swap Axes"), ICON_MOUSE_MMB);

    status.opmodal(
        IFACE_("Snap"), custom_data->op->type, TFM_MODAL_SNAP_TOGGLE, t->modifiers & MOD_SNAP);
    status.opmodal(IFACE_("Snap Invert"),
                   custom_data->op->type,
                   TFM_MODAL_SNAP_INV_ON,
                   t->modifiers & MOD_SNAP_INVERT);
    status.opmodal(IFACE_("Precision"),
                   custom_data->op->type,
                   TFM_MODAL_PRECISION,
                   t->modifiers & MOD_PRECISION);

    if (t->proptext[0]) {
      status.opmodal({}, custom_data->op->type, TFM_MODAL_PROPSIZE_UP);
      status.opmodal(IFACE_("Proportional Size"), custom_data->op->type, TFM_MODAL_PROPSIZE_DOWN);
    }
  }
}

static void initShear(TransInfo *t, wmOperator *op)
{
  t->mode = TFM_SHEAR;

  if (t->orient_axis == t->orient_axis_ortho) {
    t->orient_axis = 2;
    t->orient_axis_ortho = 1;
  }

  initShear_mouseInputMode(t);

  t->idx_max = 0;
  t->num.idx_max = 0;
  t->increment[0] = 0.1f;
  t->increment_precision = 0.1f;

  copy_v3_fl(t->num.val_inc, t->increment[0]);
  t->num.unit_sys = t->scene->unit.system;
  t->num.unit_type[0] = B_UNIT_NONE; /* Don't think we have any unit here? */

  ShearCustomData *custom_data = static_cast<ShearCustomData *>(
      MEM_callocN(sizeof(*custom_data), __func__));
  t->custom.mode.data = custom_data;
  t->custom.mode.free_cb = [](TransInfo *, TransDataContainer *, TransCustomData *custom_data) {
    MEM_freeN(custom_data->data);
    custom_data->data = nullptr;
  };

  custom_data->update_status_bar = true;
  custom_data->op = op;

  transform_mode_default_modal_orientation_set(t, V3D_ORIENT_VIEW);
}

/** \} */

TransModeInfo TransMode_shear = {
    /*flags*/ T_NO_CONSTRAINT,
    /*init_fn*/ initShear,
    /*transform_fn*/ apply_shear,
    /*transform_matrix_fn*/ nullptr,
    /*handle_event_fn*/ handleEventShear,
    /*snap_distance_fn*/ nullptr,
    /*snap_apply_fn*/ nullptr,
    /*draw_fn*/ nullptr,
};

}  // namespace blender::ed::transform

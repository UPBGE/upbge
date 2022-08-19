/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BLI_blenlib.h"
#include "BLI_dial_2d.h"
#include "BLI_math.h"

#include "BKE_context.h"

#include "WM_api.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "DEG_depsgraph_query.h"

#include "ED_screen.h"

#include "view3d_intern.h"
#include "view3d_navigate.h" /* own include */

/* -------------------------------------------------------------------- */
/** \name View Roll Operator
 * \{ */

/**
 * \param use_axis_view: When true, keep axis-aligned orthographic views
 * (when rotating in 90 degree increments). While this may seem obscure some NDOF
 * devices have key shortcuts to do this (see #NDOF_BUTTON_ROLL_CW & #NDOF_BUTTON_ROLL_CCW).
 */
static void view_roll_angle(ARegion *region,
                            float quat[4],
                            const float orig_quat[4],
                            const float dvec[3],
                            float angle,
                            bool use_axis_view)
{
  RegionView3D *rv3d = region->regiondata;
  float quat_mul[4];

  /* camera axis */
  axis_angle_normalized_to_quat(quat_mul, dvec, angle);

  mul_qt_qtqt(quat, orig_quat, quat_mul);

  /* avoid precision loss over time */
  normalize_qt(quat);

  if (use_axis_view && RV3D_VIEW_IS_AXIS(rv3d->view) && (fabsf(angle) == (float)M_PI_2)) {
    ED_view3d_quat_to_axis_view_and_reset_quat(quat, 0.01f, &rv3d->view, &rv3d->view_axis_roll);
  }
  else {
    rv3d->view = RV3D_VIEW_USER;
  }
}

static void viewroll_apply(ViewOpsData *vod, int x, int y)
{
  float angle = BLI_dial_angle(vod->init.dial, (const float[2]){x, y});

  if (angle != 0.0f) {
    view_roll_angle(
        vod->region, vod->rv3d->viewquat, vod->init.quat, vod->init.mousevec, angle, false);
  }

  if (vod->use_dyn_ofs) {
    view3d_orbit_apply_dyn_ofs(
        vod->rv3d->ofs, vod->init.ofs, vod->init.quat, vod->rv3d->viewquat, vod->dyn_ofs);
  }

  if (RV3D_LOCK_FLAGS(vod->rv3d) & RV3D_BOXVIEW) {
    view3d_boxview_sync(vod->area, vod->region);
  }

  ED_view3d_camera_lock_sync(vod->depsgraph, vod->v3d, vod->rv3d);

  ED_region_tag_redraw(vod->region);
}

static int viewroll_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ViewOpsData *vod = op->customdata;
  short event_code = VIEW_PASS;
  bool use_autokey = false;
  int ret = OPERATOR_RUNNING_MODAL;

  /* execute the events */
  if (event->type == MOUSEMOVE) {
    event_code = VIEW_APPLY;
  }
  else if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case VIEW_MODAL_CONFIRM:
        event_code = VIEW_CONFIRM;
        break;
      case VIEWROT_MODAL_SWITCH_MOVE:
        WM_operator_name_call(C, "VIEW3D_OT_move", WM_OP_INVOKE_DEFAULT, NULL, event);
        event_code = VIEW_CONFIRM;
        break;
      case VIEWROT_MODAL_SWITCH_ROTATE:
        WM_operator_name_call(C, "VIEW3D_OT_rotate", WM_OP_INVOKE_DEFAULT, NULL, event);
        event_code = VIEW_CONFIRM;
        break;
    }
  }
  else if (ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE)) {
    /* Note this does not remove auto-keys on locked cameras. */
    copy_qt_qt(vod->rv3d->viewquat, vod->init.quat);
    ED_view3d_camera_lock_sync(vod->depsgraph, vod->v3d, vod->rv3d);
    viewops_data_free(C, op->customdata);
    op->customdata = NULL;
    return OPERATOR_CANCELLED;
  }
  else if (event->type == vod->init.event_type && event->val == KM_RELEASE) {
    event_code = VIEW_CONFIRM;
  }

  if (event_code == VIEW_APPLY) {
    viewroll_apply(vod, event->xy[0], event->xy[1]);
    if (ED_screen_animation_playing(CTX_wm_manager(C))) {
      use_autokey = true;
    }
  }
  else if (event_code == VIEW_CONFIRM) {
    use_autokey = true;
    ret = OPERATOR_FINISHED;
  }

  if (use_autokey) {
    ED_view3d_camera_lock_autokey(vod->v3d, vod->rv3d, C, true, false);
  }

  if (ret & OPERATOR_FINISHED) {
    viewops_data_free(C, op->customdata);
    op->customdata = NULL;
  }

  return ret;
}

enum {
  V3D_VIEW_STEPLEFT = 1,
  V3D_VIEW_STEPRIGHT,
};

static const EnumPropertyItem prop_view_roll_items[] = {
    {0, "ANGLE", 0, "Roll Angle", "Roll the view using an angle value"},
    {V3D_VIEW_STEPLEFT, "LEFT", 0, "Roll Left", "Roll the view around to the left"},
    {V3D_VIEW_STEPRIGHT, "RIGHT", 0, "Roll Right", "Roll the view around to the right"},
    {0, NULL, 0, NULL, NULL},
};

static int viewroll_exec(bContext *C, wmOperator *op)
{
  View3D *v3d;
  RegionView3D *rv3d;
  ARegion *region;

  if (op->customdata) {
    ViewOpsData *vod = op->customdata;
    region = vod->region;
    v3d = vod->v3d;
  }
  else {
    ED_view3d_context_user_region(C, &v3d, &region);
  }

  rv3d = region->regiondata;

  const bool is_camera_lock = ED_view3d_camera_lock_check(v3d, rv3d);
  if ((rv3d->persp != RV3D_CAMOB) || is_camera_lock) {
    if (is_camera_lock) {
      const Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
      ED_view3d_camera_lock_init(depsgraph, v3d, rv3d);
    }

    ED_view3d_smooth_view_force_finish(C, v3d, region);

    int type = RNA_enum_get(op->ptr, "type");
    float angle = (type == 0) ? RNA_float_get(op->ptr, "angle") : DEG2RADF(U.pad_rot_angle);
    float mousevec[3];
    float quat_new[4];

    const int smooth_viewtx = WM_operator_smooth_viewtx_get(op);

    if (type == V3D_VIEW_STEPLEFT) {
      angle = -angle;
    }

    normalize_v3_v3(mousevec, rv3d->viewinv[2]);
    negate_v3(mousevec);
    view_roll_angle(region, quat_new, rv3d->viewquat, mousevec, angle, true);

    const float *dyn_ofs_pt = NULL;
    float dyn_ofs[3];
    if (U.uiflag & USER_ORBIT_SELECTION) {
      if (view3d_orbit_calc_center(C, dyn_ofs)) {
        negate_v3(dyn_ofs);
        dyn_ofs_pt = dyn_ofs;
      }
    }

    ED_view3d_smooth_view(C,
                          v3d,
                          region,
                          smooth_viewtx,
                          &(const V3D_SmoothParams){
                              .quat = quat_new,
                              .dyn_ofs = dyn_ofs_pt,
                              /* Group as successive roll may run by holding a key. */
                              .undo_str = op->type->name,
                              .undo_grouped = true,
                          });

    viewops_data_free(C, op->customdata);
    op->customdata = NULL;
    return OPERATOR_FINISHED;
  }

  viewops_data_free(C, op->customdata);
  op->customdata = NULL;
  return OPERATOR_CANCELLED;
}

static int viewroll_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ViewOpsData *vod;

  bool use_angle = RNA_enum_get(op->ptr, "type") != 0;

  if (use_angle || RNA_struct_property_is_set(op->ptr, "angle")) {
    viewroll_exec(C, op);
  }
  else {
    /* makes op->customdata */
    vod = op->customdata = viewops_data_create(C, event, viewops_flag_from_prefs());
    vod->init.dial = BLI_dial_init((const float[2]){BLI_rcti_cent_x(&vod->region->winrct),
                                                    BLI_rcti_cent_y(&vod->region->winrct)},
                                   FLT_EPSILON);

    ED_view3d_smooth_view_force_finish(C, vod->v3d, vod->region);

    /* overwrite the mouse vector with the view direction */
    normalize_v3_v3(vod->init.mousevec, vod->rv3d->viewinv[2]);
    negate_v3(vod->init.mousevec);

    if (event->type == MOUSEROTATE) {
      vod->init.event_xy[0] = vod->prev.event_xy[0] = event->xy[0];
      viewroll_apply(vod, event->prev_xy[0], event->prev_xy[1]);

      viewops_data_free(C, op->customdata);
      op->customdata = NULL;
      return OPERATOR_FINISHED;
    }

    /* add temp handler */
    WM_event_add_modal_handler(C, op);
    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_FINISHED;
}

static void viewroll_cancel(bContext *C, wmOperator *op)
{
  viewops_data_free(C, op->customdata);
  op->customdata = NULL;
}

void VIEW3D_OT_view_roll(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "View Roll";
  ot->description = "Roll the view";
  ot->idname = "VIEW3D_OT_view_roll";

  /* api callbacks */
  ot->invoke = viewroll_invoke;
  ot->exec = viewroll_exec;
  ot->modal = viewroll_modal;
  ot->poll = ED_operator_rv3d_user_region_poll;
  ot->cancel = viewroll_cancel;

  /* flags */
  ot->flag = 0;

  /* properties */
  ot->prop = prop = RNA_def_float(
      ot->srna, "angle", 0, -FLT_MAX, FLT_MAX, "Roll", "", -FLT_MAX, FLT_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_enum(ot->srna,
                      "type",
                      prop_view_roll_items,
                      0,
                      "Roll Angle Source",
                      "How roll angle is calculated");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

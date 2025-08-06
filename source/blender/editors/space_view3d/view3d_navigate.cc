/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_curve_types.h"

#include "BLI_dial_2d.h"
#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_vfont.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_screen.hh"
#include "ED_transform.hh"

#include "WM_api.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "view3d_intern.hh"

#include "view3d_navigate.hh" /* own include */

/* Prototypes. */
static const ViewOpsType *view3d_navigation_type_from_idname(const char *idname);

static eViewOpsFlag viewops_flag_from_prefs()
{
  const bool use_select = (U.uiflag & USER_ORBIT_SELECTION) != 0;
  const bool use_depth = (U.uiflag & USER_DEPTH_NAVIGATE) != 0;
  const bool use_zoom_to_mouse = (U.uiflag & USER_ZOOM_TO_MOUSEPOS) != 0;

  /**
   * If the mode requires it, always set the #VIEWOPS_FLAG_PERSP_ENSURE.
   * The function #ED_view3d_persp_ensure already handles the checking of the preferences.
   * And even with the option disabled, in some modes, it is still necessary to exit the camera
   * view.
   *
   * \code{.c}
   * const bool use_auto_persp = (U.uiflag & USER_AUTOPERSP) != 0;
   * if (use_auto_persp) {
   *  flag |= VIEWOPS_FLAG_PERSP_ENSURE;
   * }
   * \endcode
   */
  enum eViewOpsFlag flag = VIEWOPS_FLAG_INIT_ZFAC | VIEWOPS_FLAG_PERSP_ENSURE;

  if (use_select) {
    flag |= VIEWOPS_FLAG_ORBIT_SELECT;
  }
  if (use_depth) {
    flag |= VIEWOPS_FLAG_DEPTH_NAVIGATE;
  }
  if (use_zoom_to_mouse) {
    flag |= VIEWOPS_FLAG_ZOOM_TO_MOUSE;
  }

  return flag;
}

/* -------------------------------------------------------------------- */
/** \name ViewOpsData definition
 * \{ */

void ViewOpsData::init_context(bContext *C)
{
  /* Store data. */
  this->depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  this->scene = CTX_data_scene(C);
  this->area = CTX_wm_area(C);
  this->region = CTX_wm_region(C);
  this->v3d = static_cast<View3D *>(this->area->spacedata.first);
  this->rv3d = static_cast<RegionView3D *>(this->region->regiondata);
}

void ViewOpsData::state_backup()
{
  copy_v3_v3(this->init.ofs, rv3d->ofs);
  copy_v2_v2(this->init.ofs_lock, rv3d->ofs_lock);
  this->init.camdx = rv3d->camdx;
  this->init.camdy = rv3d->camdy;
  this->init.camzoom = rv3d->camzoom;
  this->init.dist = rv3d->dist;
  copy_qt_qt(this->init.quat, rv3d->viewquat);

  this->init.persp = rv3d->persp;
  this->init.view = rv3d->view;
  this->init.view_axis_roll = rv3d->view_axis_roll;
}

void ViewOpsData::state_restore()
{
  /* DOLLY, MOVE, ROTATE and ZOOM. */
  {
    /* For Move this only changes when offset is not locked. */
    /* For Rotate this only changes when rotating around objects or last-brush. */
    /* For Zoom this only changes when zooming to mouse position. */
    /* Note this does not remove auto-keys on locked cameras. */
    copy_v3_v3(this->rv3d->ofs, this->init.ofs);
  }

  /* MOVE and ZOOM. */
  {
    /* For Move this only changes when offset is not locked. */
    /* For Zoom this only changes when zooming to mouse position in camera view. */
    this->rv3d->camdx = this->init.camdx;
    this->rv3d->camdy = this->init.camdy;
  }

  /* MOVE. */
  {
    if ((this->rv3d->persp == RV3D_CAMOB) && !ED_view3d_camera_lock_check(this->v3d, this->rv3d)) {
      // this->rv3d->camdx = this->init.camdx;
      // this->rv3d->camdy = this->init.camdy;
    }
    else if (ED_view3d_offset_lock_check(this->v3d, this->rv3d)) {
      copy_v2_v2(this->rv3d->ofs_lock, this->init.ofs_lock);
    }
    else {
      // copy_v3_v3(vod->rv3d->ofs, vod->init.ofs);
      if (RV3D_LOCK_FLAGS(this->rv3d) & RV3D_BOXVIEW) {
        view3d_boxview_sync(this->area, this->region);
      }
    }
  }

  /* ZOOM. */
  {
    this->rv3d->camzoom = this->init.camzoom;
  }

  /* ROTATE and ZOOM. */
  {
    /* For Rotate this only changes when orbiting from a camera view.
     * In this case the `dist` is calculated based on the camera relative to the `ofs`. */

    /* Note this does not remove auto-keys on locked cameras. */
    this->rv3d->dist = this->init.dist;
  }

  /* ROLL and ROTATE. */
  {
    /* Note this does not remove auto-keys on locked cameras. */
    copy_qt_qt(this->rv3d->viewquat, this->init.quat);
  }

  /* ROTATE. */
  {
    this->rv3d->persp = this->init.persp;
    this->rv3d->view = this->init.view;
    this->rv3d->view_axis_roll = this->init.view_axis_roll;
  }

  /* NOTE: there is no need to restore "last" values (as set by #ED_view3d_lastview_store). */

  ED_view3d_camera_lock_sync(this->depsgraph, this->v3d, this->rv3d);
}

static eViewOpsFlag navigate_pivot_get(bContext *C,
                                       Depsgraph *depsgraph,
                                       ARegion *region,
                                       View3D *v3d,
                                       const wmEvent *event,
                                       eViewOpsFlag viewops_flag,
                                       const float dyn_ofs_override[3],
                                       float r_pivot[3])
{
  if ((viewops_flag & VIEWOPS_FLAG_ORBIT_SELECT) && view3d_orbit_calc_center(C, r_pivot)) {
    return VIEWOPS_FLAG_ORBIT_SELECT;
  }

  wmWindow *win = CTX_wm_window(C);

  if (!(viewops_flag & VIEWOPS_FLAG_DEPTH_NAVIGATE)) {
    ED_view3d_autodist_last_clear(win);

    /* Uses the `lastofs` in #view3d_orbit_calc_center. */
    BLI_assert(viewops_flag & VIEWOPS_FLAG_ORBIT_SELECT);
    if (v3d->runtime.flag & V3D_RUNTIME_OFS_LAST_CENTER_IS_VALID) {
      return VIEWOPS_FLAG_ORBIT_SELECT;
    }
    /* No valid pivot, don't use any dynamic offset. */
    return VIEWOPS_FLAG_NONE;
  }

  if (dyn_ofs_override) {
    ED_view3d_win_to_3d_int(v3d, region, dyn_ofs_override, event->mval, r_pivot);
    return VIEWOPS_FLAG_DEPTH_NAVIGATE;
  }

  const bool use_depth_last = ED_view3d_autodist_last_check(win, event);

  if (use_depth_last) {
    ED_view3d_autodist_last_get(win, r_pivot);
  }
  else {
    float fallback_depth_pt[3];
    negate_v3_v3(fallback_depth_pt, static_cast<RegionView3D *>(region->regiondata)->ofs);

    if (!ED_view3d_has_depth_buffer_updated(depsgraph, v3d)) {
      ED_view3d_depth_override(
          depsgraph, region, v3d, nullptr, V3D_DEPTH_NO_GPENCIL, true, nullptr);
    }

    const bool is_set = ED_view3d_autodist(region, v3d, event->mval, r_pivot, fallback_depth_pt);

    ED_view3d_autodist_last_set(win, event, r_pivot, is_set);
  }

  return VIEWOPS_FLAG_DEPTH_NAVIGATE;
}

void ViewOpsData::init_navigation(bContext *C,
                                  const wmEvent *event,
                                  const ViewOpsType *nav_type,
                                  const float dyn_ofs_override[3],
                                  const bool use_cursor_init)
{
  using namespace blender;
  this->nav_type = nav_type;
  eViewOpsFlag viewops_flag = nav_type->flag & viewops_flag_from_prefs();
  constexpr eViewOpsFlag viewops_flag_dynamic_ofs = VIEWOPS_FLAG_DEPTH_NAVIGATE |
                                                    VIEWOPS_FLAG_ORBIT_SELECT;

  if (!use_cursor_init) {
    viewops_flag &= ~(VIEWOPS_FLAG_DEPTH_NAVIGATE | VIEWOPS_FLAG_ZOOM_TO_MOUSE);
  }

  bool calc_rv3d_dist = true;
#ifdef WITH_INPUT_NDOF
  if (ELEM(nav_type,
           &ViewOpsType_ndof_orbit,
           &ViewOpsType_ndof_orbit_zoom,
           &ViewOpsType_ndof_pan,
           &ViewOpsType_ndof_all))
  {
    calc_rv3d_dist = false;

    /* When using "Free" NDOF navigation, ignore "Orbit Around Selected" preference.
     * Logically it doesn't make sense to use the selection as a pivot when the first-person
     * navigation pivots from the view-point. This also interferes with zoom-speed,
     * causing zoom-speed scale based on the distance to the selection center, see: #115253. */
    if (U.ndof_navigation_mode == NDOF_NAVIGATION_MODE_FLY) {
      viewops_flag &= ~VIEWOPS_FLAG_ORBIT_SELECT;
    }
  }
#endif

  /* Set the view from the camera, if view locking is enabled.
   * we may want to make this optional but for now its needed always. */
  ED_view3d_camera_lock_init_ex(depsgraph, v3d, rv3d, calc_rv3d_dist);

  this->state_backup();

  if (viewops_flag & VIEWOPS_FLAG_PERSP_ENSURE) {
    if (ED_view3d_persp_ensure(depsgraph, this->v3d, this->region)) {
      /* If we're switching from camera view to the perspective one,
       * need to tag viewport update, so camera view and borders are properly updated. */
      ED_region_tag_redraw(this->region);
    }
  }

  if (viewops_flag & viewops_flag_dynamic_ofs) {
    float pivot_new[3];
    eViewOpsFlag pivot_type = navigate_pivot_get(
        C, depsgraph, region, v3d, event, viewops_flag, dyn_ofs_override, pivot_new);

    viewops_flag &= ~viewops_flag_dynamic_ofs;
    viewops_flag |= pivot_type;

    /* It's possible no offset can be found, see: #111098. */
    if (viewops_flag & viewops_flag_dynamic_ofs) {
      negate_v3_v3(this->dyn_ofs, pivot_new);
      this->use_dyn_ofs = true;

      if (pivot_type == VIEWOPS_FLAG_DEPTH_NAVIGATE) {
        /* Ensure we'll always be able to zoom into the new pivot point and panning won't go bad
         * when dist is zero. Therefore, set a new #RegionView3D::ofs and #RegionView3D::dist so
         * that the dist value becomes the distance from the new pivot point. */

        if (rv3d->is_persp) {
          float my_origin[3]; /* Original #RegionView3D.ofs. */
          float my_pivot[3];  /* View pivot. */
          float dvec[3];

          negate_v3_v3(my_origin, rv3d->ofs); /* ofs is flipped */

          /* remove dist value */
          float3 upvec;
          upvec[0] = upvec[1] = 0;
          upvec[2] = rv3d->dist;
          float3x3 mat = float3x3(float4x4(rv3d->viewinv));

          upvec = math::transform_point(mat, upvec);
          add_v3_v3v3(my_pivot, my_origin, upvec);

          /* find a new ofs value that is along the view axis
           * (rather than the mouse location) */
          float lambda = closest_to_line_v3(dvec, pivot_new, my_pivot, my_origin);

          negate_v3_v3(rv3d->ofs, dvec);
          rv3d->dist = len_v3v3(my_pivot, dvec);

          if (lambda < 0.0f) {
            /* The distance is actually negative. */
            rv3d->dist *= -1;
          }
        }
        else {
          const float mval_region_mid[2] = {float(region->winx) / 2.0f,
                                            float(region->winy) / 2.0f};
          ED_view3d_win_to_3d(v3d, region, pivot_new, mval_region_mid, rv3d->ofs);
          negate_v3(rv3d->ofs);
        }
      }

      /* Reinitialize `this->init.dist` and `this->init.ofs` as these values may have changed
       * when #ED_view3d_persp_ensure was called or when the operator uses `Auto Depth`.
       *
       * XXX: The initial state captured by #ViewOpsData::state_backup is being modified here.
       * This causes the state not to be fully restored when canceling a navigation operation. */
      this->init.dist = rv3d->dist;
      copy_v3_v3(this->init.ofs, rv3d->ofs);
    }
  }

  if (viewops_flag & VIEWOPS_FLAG_INIT_ZFAC) {
    float tvec[3];
    negate_v3_v3(tvec, rv3d->ofs);
    this->init.zfac = ED_view3d_calc_zfac(rv3d, tvec);
  }

  this->init.persp_with_auto_persp_applied = rv3d->persp;

  if (event) {
    this->init.event_type = event->type;
    copy_v2_v2_int(this->init.event_xy, event->xy);
    copy_v2_v2_int(this->prev.event_xy, event->xy);

    if (use_cursor_init) {
      zero_v2_int(this->init.event_xy_offset);
    }
    else {
      /* Simulate the event starting in the middle of the region. */
      this->init.event_xy_offset[0] = BLI_rcti_cent_x(&this->region->winrct) - event->xy[0];
      this->init.event_xy_offset[1] = BLI_rcti_cent_y(&this->region->winrct) - event->xy[1];
    }

    /* For dolly */
    const float mval[2] = {float(event->mval[0]), float(event->mval[1])};
    ED_view3d_win_to_vector(region, mval, this->init.mousevec);

    {
      int2 event_xy_offset = int2(event->xy) + this->init.event_xy_offset;

      /* For rotation with trackball rotation. */
      calctrackballvec(&region->winrct, event_xy_offset, this->init.trackvec);
    }
  }

  copy_qt_qt(this->curr.viewquat, rv3d->viewquat);

  this->reverse = 1.0f;
  if (rv3d->persmat[2][1] < 0.0f) {
    this->reverse = -1.0f;
  }

  this->viewops_flag = viewops_flag;

  /* Default. */
  this->use_dyn_ofs_ortho_correction = false;

  rv3d->rflag |= RV3D_NAVIGATING;
}

void ViewOpsData::end_navigation(bContext *C)
{
  this->rv3d->rflag &= ~RV3D_NAVIGATING;

  if (this->timer) {
    WM_event_timer_remove(CTX_wm_manager(C), this->timer->win, this->timer);
  }

  if (this->init.dial) {
    BLI_dial_free(this->init.dial);
    this->init.dial = nullptr;
  }

  /* Need to redraw because drawing code uses RV3D_NAVIGATING to draw
   * faster while navigation operator runs. */
  ED_region_tag_redraw(this->region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic Operator Callback Utils
 * \{ */

/* Used for navigation utility in operators. */
struct ViewOpsData_Utility : ViewOpsData {
  /* To track only the navigation #wmKeyMapItem items and allow changes to them, an internal
   * #wmKeyMap is created with their copy. */
  ListBase keymap_items;

  /* Used by #ED_view3d_navigation_do. */
  bool is_modal_event = false;

  ViewOpsData_Utility(bContext *C, const wmKeyMapItem *kmi_merge = nullptr)
      : ViewOpsData(), keymap_items()
  {
    this->init_context(C);

    wmKeyMap *keymap = WM_keymap_find_all(
        CTX_wm_manager(C), "3D View", SPACE_VIEW3D, RGN_TYPE_WINDOW);

    WM_keyconfig_update_suppress_begin();

    wmKeyMap keymap_tmp = {};

    LISTBASE_FOREACH (wmKeyMapItem *, kmi, &keymap->items) {
      if (!STRPREFIX(kmi->idname, "VIEW3D")) {
        continue;
      }
      if (kmi->flag & KMI_INACTIVE) {
        continue;
      }
      if (view3d_navigation_type_from_idname(kmi->idname) == nullptr) {
        continue;
      }

      wmKeyMapItem *kmi_cpy = WM_keymap_add_item_copy(&keymap_tmp, kmi);
      if (kmi_merge) {
        if (kmi_merge->shift == KM_MOD_HELD ||
            ELEM(kmi_merge->type, EVT_RIGHTSHIFTKEY, EVT_LEFTSHIFTKEY))
        {
          kmi_cpy->shift = KM_MOD_HELD;
        }
        if (kmi_merge->ctrl == KM_MOD_HELD ||
            ELEM(kmi_merge->type, EVT_LEFTCTRLKEY, EVT_RIGHTCTRLKEY))
        {
          kmi_cpy->ctrl = KM_MOD_HELD;
        }
        if (kmi_merge->alt == KM_MOD_HELD ||
            ELEM(kmi_merge->type, EVT_LEFTALTKEY, EVT_RIGHTALTKEY))
        {
          kmi_cpy->alt = KM_MOD_HELD;
        }
        if (kmi_merge->oskey == KM_MOD_HELD || ELEM(kmi_merge->type, EVT_OSKEY)) {
          kmi_cpy->oskey = KM_MOD_HELD;
        }
        if (kmi_merge->hyper == KM_MOD_HELD || ELEM(kmi_merge->type, EVT_HYPER)) {
          kmi_cpy->hyper = KM_MOD_HELD;
        }
        if (!ISKEYMODIFIER(kmi_merge->type)) {
          kmi_cpy->keymodifier = kmi_merge->type;
        }
      }
    }

    /* Weak, but only the keymap items from the #wmKeyMap struct are needed here. */
    this->keymap_items = keymap_tmp.items;

    WM_keyconfig_update_suppress_end();
  }

  ~ViewOpsData_Utility()
  {
    /* Weak, but rebuild the struct #wmKeyMap to clear the keymap items. */
    WM_keyconfig_update_suppress_begin();

    wmKeyMap keymap_tmp = {};
    keymap_tmp.items = this->keymap_items;
    WM_keymap_clear(&keymap_tmp);

    WM_keyconfig_update_suppress_end();
  }

  MEM_CXX_CLASS_ALLOC_FUNCS("ViewOpsData_Utility")
};

static bool view3d_navigation_poll_impl(bContext *C, const char viewlock)
{
  if (!ED_operator_region_view3d_active(C)) {
    return false;
  }

  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  return !(RV3D_LOCK_FLAGS(rv3d) & viewlock);
}

static eV3D_OpEvent view3d_navigate_event(ViewOpsData *vod, const wmEvent *event)
{
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case VIEW_MODAL_CANCEL:
        return VIEW_CANCEL;
      case VIEW_MODAL_CONFIRM:
        return VIEW_CONFIRM;
      case VIEWROT_MODAL_AXIS_SNAP_ENABLE:
        vod->axis_snap = true;
        return VIEW_APPLY;
      case VIEWROT_MODAL_AXIS_SNAP_DISABLE:
        vod->rv3d->persp = vod->init.persp_with_auto_persp_applied;
        vod->axis_snap = false;
        return VIEW_APPLY;
      case VIEWROT_MODAL_SWITCH_ZOOM:
      case VIEWROT_MODAL_SWITCH_MOVE:
      case VIEWROT_MODAL_SWITCH_ROTATE: {
        const ViewOpsType *nav_type_new = (event->val == VIEWROT_MODAL_SWITCH_ZOOM) ?
                                              &ViewOpsType_zoom :
                                          (event->val == VIEWROT_MODAL_SWITCH_MOVE) ?
                                              &ViewOpsType_move :
                                              &ViewOpsType_rotate;
        if (nav_type_new == vod->nav_type) {
          break;
        }
        vod->nav_type = nav_type_new;
        return VIEW_APPLY;
      }
    }
  }
  else {
    if (event->type == TIMER && event->customdata == vod->timer) {
      /* Zoom uses timer for continuous zoom. */
      return VIEW_APPLY;
    }
    if (event->type == MOUSEMOVE) {
      return VIEW_APPLY;
    }
    if (event->type == vod->init.event_type && event->val == KM_RELEASE) {
      return VIEW_CONFIRM;
    }
    if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
      return VIEW_CANCEL;
    }
  }

  return VIEW_PASS;
}

static wmOperatorStatus view3d_navigation_invoke_generic(bContext *C,
                                                         ViewOpsData *vod,
                                                         const wmEvent *event,
                                                         PointerRNA *ptr,
                                                         const ViewOpsType *nav_type,
                                                         const float dyn_ofs_override[3])
{
  if (!nav_type->init_fn) {
    return OPERATOR_CANCELLED;
  }

  bool use_cursor_init = false;
  if (PropertyRNA *prop = RNA_struct_find_property(ptr, "use_cursor_init")) {
    use_cursor_init = RNA_property_boolean_get(ptr, prop);
  }

  vod->init_navigation(C, event, nav_type, dyn_ofs_override, use_cursor_init);
  ED_view3d_smooth_view_force_finish(C, vod->v3d, vod->region);

  return nav_type->init_fn(C, vod, event, ptr);
}

wmOperatorStatus view3d_navigate_invoke_impl(bContext *C,
                                             wmOperator *op,
                                             const wmEvent *event,
                                             const ViewOpsType *nav_type)
{
  ViewOpsData *vod = new ViewOpsData();
  vod->init_context(C);
  wmOperatorStatus ret = view3d_navigation_invoke_generic(
      C, vod, event, op->ptr, nav_type, nullptr);
  op->customdata = (void *)vod;

  if (ret == OPERATOR_RUNNING_MODAL) {
    WM_event_add_modal_handler(C, op);
    return OPERATOR_RUNNING_MODAL;
  }

  viewops_data_free(C, vod);
  op->customdata = nullptr;
  return ret;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic Callbacks
 * \{ */

bool view3d_location_poll(bContext *C)
{
  return view3d_navigation_poll_impl(C, RV3D_LOCK_LOCATION);
}

bool view3d_rotation_poll(bContext *C)
{
  return view3d_navigation_poll_impl(C, RV3D_LOCK_ROTATION);
}

bool view3d_zoom_or_dolly_poll(bContext *C)
{
  return view3d_navigation_poll_impl(C, RV3D_LOCK_ZOOM_AND_DOLLY);
}

bool view3d_zoom_or_dolly_or_rotation_poll(bContext *C)
{
  /* This combination of flags is needed for the dolly operator,
   * see code-comments there for details. */
  return view3d_navigation_poll_impl(C, RV3D_LOCK_ZOOM_AND_DOLLY | RV3D_LOCK_ROTATION);
}

wmOperatorStatus view3d_navigate_modal_fn(bContext *C, wmOperator *op, const wmEvent *event)
{
  ViewOpsData *vod = static_cast<ViewOpsData *>(op->customdata);

  const ViewOpsType *nav_type_prev = vod->nav_type;
  const eV3D_OpEvent event_code = view3d_navigate_event(vod, event);
  if (nav_type_prev != vod->nav_type) {
    wmOperatorType *ot_new = WM_operatortype_find(vod->nav_type->idname, false);
    WM_operator_type_set(op, ot_new);
    vod->end_navigation(C);
    return view3d_navigation_invoke_generic(C, vod, event, op->ptr, vod->nav_type, nullptr);
  }

  wmOperatorStatus ret = vod->nav_type->apply_fn(C, vod, event_code, event->xy);

  if ((ret & OPERATOR_RUNNING_MODAL) == 0) {
    if (ret & OPERATOR_FINISHED) {
      ED_view3d_camera_lock_undo_push(op->type->name, vod->v3d, vod->rv3d, C);
    }
    viewops_data_free(C, vod);
    op->customdata = nullptr;
  }

  return ret;
}

void view3d_navigate_cancel_fn(bContext *C, wmOperator *op)
{
  viewops_data_free(C, static_cast<ViewOpsData *>(op->customdata));
  op->customdata = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic View Operator Properties
 * \{ */

void view3d_operator_properties_common(wmOperatorType *ot, const enum eV3D_OpPropFlag flag)
{
  if (flag & V3D_OP_PROP_MOUSE_CO) {
    PropertyRNA *prop;
    prop = RNA_def_int(ot->srna, "mx", 0, 0, INT_MAX, "Region Position X", "", 0, INT_MAX);
    RNA_def_property_flag(prop, PROP_HIDDEN);
    prop = RNA_def_int(ot->srna, "my", 0, 0, INT_MAX, "Region Position Y", "", 0, INT_MAX);
    RNA_def_property_flag(prop, PROP_HIDDEN);
  }
  if (flag & V3D_OP_PROP_DELTA) {
    RNA_def_int(ot->srna, "delta", 0, INT_MIN, INT_MAX, "Delta", "", INT_MIN, INT_MAX);
  }
  if (flag & V3D_OP_PROP_USE_ALL_REGIONS) {
    PropertyRNA *prop;
    prop = RNA_def_boolean(
        ot->srna, "use_all_regions", false, "All Regions", "View selected for all regions");
    RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  }
  if (flag & V3D_OP_PROP_USE_MOUSE_INIT) {
    WM_operator_properties_use_cursor_init(ot);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic View Operator Custom-Data
 * \{ */

void calctrackballvec(const rcti *rect, const int event_xy[2], float r_dir[3])
{
  const float radius = V3D_OP_TRACKBALLSIZE;
  const float t = radius / float(M_SQRT2);
  const float size[2] = {float(BLI_rcti_size_x(rect)), float(BLI_rcti_size_y(rect))};
  /* Aspect correct so dragging in a non-square view doesn't squash the direction.
   * So diagonal motion rotates the same direction the cursor is moving. */
  const float size_min = min_ff(size[0], size[1]);
  const float aspect[2] = {size_min / size[0], size_min / size[1]};

  /* Normalize x and y. */
  r_dir[0] = (event_xy[0] - BLI_rcti_cent_x(rect)) / ((size[0] * aspect[0]) / 2.0);
  r_dir[1] = (event_xy[1] - BLI_rcti_cent_y(rect)) / ((size[1] * aspect[1]) / 2.0);
  const float d = len_v2(r_dir);
  if (d < t) {
    /* Inside sphere. */
    r_dir[2] = sqrtf(square_f(radius) - square_f(d));
  }
  else {
    /* On hyperbola. */
    r_dir[2] = square_f(t) / d;
  }
}

void view3d_orbit_apply_dyn_ofs(float r_ofs[3],
                                const float ofs_old[3],
                                const float viewquat_old[4],
                                const float viewquat_new[4],
                                const float dyn_ofs[3])
{
  float q[4];
  invert_qt_qt_normalized(q, viewquat_old);
  mul_qt_qtqt(q, q, viewquat_new);

  invert_qt_normalized(q);

  sub_v3_v3v3(r_ofs, ofs_old, dyn_ofs);
  mul_qt_v3(q, r_ofs);
  add_v3_v3(r_ofs, dyn_ofs);
}

static void view3d_orbit_apply_dyn_ofs_ortho_correction(float ofs[3],
                                                        const float viewquat_old[4],
                                                        const float viewquat_new[4],
                                                        const float dyn_ofs[3])
{
  /* NOTE(@ideasman42): While orbiting in orthographic mode the "depth" of the offset
   * (position along the views Z-axis) is only noticeable when the view contents is clipped.
   * The likelihood of clipping depends on the clipping range & size of the scene.
   * In practice some users might not run into this, however using dynamic-offset in
   * orthographic views can cause the depth of the offset to drift while navigating the view,
   * causing unexpected clipping that seems like a bug from the user perspective, see: #104385.
   *
   * Imagine a camera is focused on a distant object. Now imagine a closer object in front of
   * the camera is used as a pivot, the camera is rotated to view it from the side (~90d rotation).
   * The outcome is the camera is now focused on a distant region to the left/right.
   * The new focal point is unlikely to point to anything useful (unless by accident).
   * Instead of a focal point - the `rv3d->ofs` is being manipulated in this case.
   *
   * Resolve by moving #RegionView3D::ofs so it is depth-aligned to `dyn_ofs`,
   * this is interpolated by the amount of rotation so minor rotations don't cause
   * the view-clipping to suddenly jump.
   *
   * Perspective Views
   * =================
   *
   * This logic could also be applied to perspective views because the issue of the `ofs`
   * being a location which isn't useful exists there too, however the problem where this location
   * impacts the clipping does *not* exist, as the clipping range starts from the view-point
   * (`ofs` + `dist` along the view Z-axis) unlike orthographic views which center around `ofs`.
   * Nevertheless there will be cases when having `ofs` and a large `dist` pointing nowhere doesn't
   * give ideal behavior (zooming may jump in larger than expected steps and panning the view may
   * move too much in relation to nearby objects - for example). So it's worth investigating but
   * should be done with extra care as changing `ofs` in perspective view also requires changing
   * the `dist` which could cause unexpected results if the calculated `dist` happens to be small.
   * So disable this workaround in perspective view unless there are clear benefits to enabling. */

  float q_inv[4];

  float view_z_init[3] = {0.0f, 0.0f, 1.0f};
  invert_qt_qt_normalized(q_inv, viewquat_old);
  mul_qt_v3(q_inv, view_z_init);

  float view_z_curr[3] = {0.0f, 0.0f, 1.0f};
  invert_qt_qt_normalized(q_inv, viewquat_new);
  mul_qt_v3(q_inv, view_z_curr);

  const float angle_cos = max_ff(0.0f, dot_v3v3(view_z_init, view_z_curr));
  /* 1.0 or more means no rotation, there is nothing to do in that case. */
  if (LIKELY(angle_cos < 1.0f)) {
    const float dot_ofs_curr = dot_v3v3(view_z_curr, ofs);
    const float dot_ofs_next = dot_v3v3(view_z_curr, dyn_ofs);
    const float ofs_delta = dot_ofs_next - dot_ofs_curr;
    if (LIKELY(ofs_delta != 0.0f)) {
      /* Calculate a factor where 0.0 represents no rotation and 1.0 represents 90d or more.
       * NOTE: Without applying the factor, the distances immediately changes
       * (useful for testing), but not good for the users experience as minor rotations
       * should not immediately adjust the depth. */
      const float factor = acosf(angle_cos) / M_PI_2;
      madd_v3_v3fl(ofs, view_z_curr, ofs_delta * factor);
    }
  }
}

void viewrotate_apply_dyn_ofs(ViewOpsData *vod, const float viewquat_new[4])
{
  if (vod->use_dyn_ofs) {
    RegionView3D *rv3d = vod->rv3d;
    view3d_orbit_apply_dyn_ofs(
        rv3d->ofs, vod->init.ofs, vod->init.quat, viewquat_new, vod->dyn_ofs);

    if (vod->use_dyn_ofs_ortho_correction) {
      view3d_orbit_apply_dyn_ofs_ortho_correction(
          rv3d->ofs, vod->init.quat, viewquat_new, vod->dyn_ofs);
    }
  }
}

bool view3d_orbit_calc_center(bContext *C, float r_dyn_ofs[3])
{
  using namespace blender;
  float3 ofs = float3(0);
  bool is_set = false;

  const Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Paint *paint = BKE_paint_get_active_from_context(C);
  ViewLayer *view_layer_eval = DEG_get_evaluated_view_layer(depsgraph);
  View3D *v3d = CTX_wm_view3d(C);
  BKE_view_layer_synced_ensure(scene_eval, view_layer_eval);
  Object *ob_act_eval = BKE_view_layer_active_object_get(view_layer_eval);
  Object *ob_act = DEG_get_original(ob_act_eval);

  if (v3d->runtime.flag & V3D_RUNTIME_OFS_LAST_CENTER_IS_VALID) {
    ofs = -float3(v3d->runtime.ofs_last_center);
  }

  if (ob_act && (ob_act->mode & OB_MODE_ALL_PAINT) &&
      /* with weight-paint + pose-mode, fall through to using calculateTransformCenter */
      ((ob_act->mode & OB_MODE_WEIGHT_PAINT) && BKE_object_pose_armature_get(ob_act)) == 0)
  {
    BKE_paint_stroke_get_average(paint, ob_act_eval, ofs);
    is_set = true;
  }
  else if (ob_act && ELEM(ob_act->mode,
                          OB_MODE_SCULPT_CURVES,
                          OB_MODE_PAINT_GREASE_PENCIL,
                          OB_MODE_SCULPT_GREASE_PENCIL,
                          OB_MODE_VERTEX_GREASE_PENCIL,
                          OB_MODE_WEIGHT_GREASE_PENCIL))
  {
    BKE_paint_stroke_get_average(paint, ob_act_eval, ofs);
    is_set = true;
  }
  else if (ob_act && (ob_act->mode & OB_MODE_EDIT) && (ob_act->type == OB_FONT)) {
    Curve *cu = static_cast<Curve *>(ob_act_eval->data);
    EditFont *ef = cu->editfont;

    ofs = float3(0);
    for (int i = 0; i < 4; i++) {
      ofs += ef->textcurs[i];
    }
    ofs *= 0.25f;

    ofs = math::transform_point(ob_act_eval->object_to_world(), ofs);

    is_set = true;
  }
  else if (ob_act == nullptr || ob_act->mode == OB_MODE_OBJECT) {
    /* Object mode uses bounding-box centers. */
    int total = 0;
    float3 select_center(0);

    zero_v3(select_center);
    LISTBASE_FOREACH (const Base *, base_eval, BKE_view_layer_object_bases_get(view_layer_eval)) {
      if (BASE_SELECTED(v3d, base_eval)) {
        /* Use the bounding-box if we can. */
        const Object *ob_eval = base_eval->object;

        if (const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(ob_eval)) {
          const float3 center = math::midpoint(bounds->min, bounds->max);
          select_center += math::transform_point(ob_eval->object_to_world(), center);
        }
        else {
          add_v3_v3(select_center, ob_eval->object_to_world().location());
        }
        total++;
      }
    }
    if (total) {
      mul_v3_fl(select_center, 1.0f / float(total));
      copy_v3_v3(ofs, select_center);
      is_set = true;
    }
  }
  else {
    /* If there's no selection, `ofs` is unmodified, the last offset will be used if set.
     * Otherwise the value of `ofs` is zero and should not be used. */
    is_set = blender::ed::transform::calc_pivot_pos(C, V3D_AROUND_CENTER_MEDIAN, ofs);
  }

  if (is_set) {
    v3d->runtime.flag |= V3D_RUNTIME_OFS_LAST_CENTER_IS_VALID;
    negate_v3_v3(v3d->runtime.ofs_last_center, ofs);
  }

  copy_v3_v3(r_dyn_ofs, ofs);

  return is_set;
}

ViewOpsData *viewops_data_create(bContext *C,
                                 const wmEvent *event,
                                 const ViewOpsType *nav_type,
                                 const bool use_cursor_init)
{
  ViewOpsData *vod = new ViewOpsData();
  vod->init_context(C);
  vod->init_navigation(C, event, nav_type, nullptr, use_cursor_init);
  return vod;
}

void viewops_data_free(bContext *C, ViewOpsData *vod)
{
  if (!vod) {
    return;
  }
  vod->end_navigation(C);
  delete vod;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic View Operator Utilities
 * \{ */

void axis_set_view(bContext *C,
                   View3D *v3d,
                   ARegion *region,
                   const float quat_[4],
                   char view,
                   char view_axis_roll,
                   int perspo,
                   const float *align_to_quat,
                   const int smooth_viewtx)
{
  /* no nullptr check is needed, poll checks */
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

  float quat[4];
  const short orig_persp = rv3d->persp;
  const char orig_view = rv3d->view;
  const char orig_view_axis_roll = rv3d->view_axis_roll;

  normalize_qt_qt(quat, quat_);

  if (align_to_quat) {
    mul_qt_qtqt(quat, quat, align_to_quat);
    rv3d->view = view = RV3D_VIEW_USER;
    rv3d->view_axis_roll = RV3D_VIEW_AXIS_ROLL_0;
  }
  else {
    rv3d->view = view;
    rv3d->view_axis_roll = view_axis_roll;
  }

  /* Redrawing when changes are detected is needed because the current view
   * orientation may be a "User" view that matches the axis exactly.
   * In this case smooth-view exits early as no view transition is needed.
   * However, changing the view must redraw the region as it changes the
   * viewport name & grid drawing. */
  if ((rv3d->view != orig_view) || (rv3d->view_axis_roll != orig_view_axis_roll)) {
    ED_region_tag_redraw(region);
  }

  if (RV3D_LOCK_FLAGS(rv3d) & RV3D_LOCK_ROTATION) {
    return;
  }

  if (U.uiflag & USER_AUTOPERSP) {
    rv3d->persp = RV3D_VIEW_IS_AXIS(view) ? RV3D_ORTHO : perspo;
  }
  else if (rv3d->persp == RV3D_CAMOB) {
    rv3d->persp = perspo;
  }
  if (rv3d->persp != orig_persp) {
    ED_region_tag_redraw(region);
  }

  if (rv3d->persp == RV3D_CAMOB && v3d->camera) {
    /* to camera */
    V3D_SmoothParams sview = {nullptr};
    sview.camera_old = v3d->camera;
    sview.ofs = rv3d->ofs;
    sview.quat = quat;
    /* No undo because this switches to/from camera. */
    sview.undo_str = nullptr;

    ED_view3d_smooth_view(C, v3d, region, smooth_viewtx, &sview);
  }
  else if (orig_persp == RV3D_CAMOB && v3d->camera) {
    /* from camera */
    float ofs[3], dist;

    copy_v3_v3(ofs, rv3d->ofs);
    dist = rv3d->dist;

    /* so we animate _from_ the camera location */
    Object *camera_eval = DEG_get_evaluated(CTX_data_ensure_evaluated_depsgraph(C), v3d->camera);
    ED_view3d_from_object(camera_eval, rv3d->ofs, nullptr, &rv3d->dist, nullptr);

    V3D_SmoothParams sview = {nullptr};
    sview.camera_old = camera_eval;
    sview.ofs = ofs;
    sview.quat = quat;
    sview.dist = &dist;
    /* No undo because this switches to/from camera. */
    sview.undo_str = nullptr;

    ED_view3d_smooth_view(C, v3d, region, smooth_viewtx, &sview);
  }
  else {
    /* rotate around selection */
    const float *dyn_ofs_pt = nullptr;
    float dyn_ofs[3];

    if (U.uiflag & USER_ORBIT_SELECTION) {
      if (view3d_orbit_calc_center(C, dyn_ofs)) {
        negate_v3(dyn_ofs);
        dyn_ofs_pt = dyn_ofs;
      }
    }

    /* no camera involved */
    V3D_SmoothParams sview = {nullptr};
    sview.quat = quat;
    sview.dyn_ofs = dyn_ofs_pt;
    /* No undo because this switches to/from camera. */
    sview.undo_str = nullptr;

    ED_view3d_smooth_view(C, v3d, region, smooth_viewtx, &sview);
  }
}

void viewmove_apply(ViewOpsData *vod, int x, int y)
{
  const float event_ofs[2] = {
      float(vod->prev.event_xy[0] - x),
      float(vod->prev.event_xy[1] - y),
  };

  if ((vod->rv3d->persp == RV3D_CAMOB) && !ED_view3d_camera_lock_check(vod->v3d, vod->rv3d)) {
    ED_view3d_camera_view_pan(vod->region, event_ofs);
  }
  else if (ED_view3d_offset_lock_check(vod->v3d, vod->rv3d)) {
    vod->rv3d->ofs_lock[0] -= (event_ofs[0] * 2.0f) / float(vod->region->winx);
    vod->rv3d->ofs_lock[1] -= (event_ofs[1] * 2.0f) / float(vod->region->winy);
  }
  else {
    float dvec[3];

    ED_view3d_win_to_delta(vod->region, event_ofs, vod->init.zfac, dvec, true);

    sub_v3_v3(vod->rv3d->ofs, dvec);

    if (RV3D_LOCK_FLAGS(vod->rv3d) & RV3D_BOXVIEW) {
      view3d_boxview_sync(vod->area, vod->region);
    }
  }

  vod->prev.event_xy[0] = x;
  vod->prev.event_xy[1] = y;

  ED_view3d_camera_lock_sync(vod->depsgraph, vod->v3d, vod->rv3d);

  ED_region_tag_redraw(vod->region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Navigation Utilities
 * \{ */

/* Detect the navigation operation, by the name of the navigation operator (obtained by
 * `wmKeyMapItem::idname`) */
static const ViewOpsType *view3d_navigation_type_from_idname(const char *idname)
{
  const blender::Array<const ViewOpsType *> nav_types = {
      &ViewOpsType_zoom,
      &ViewOpsType_rotate,
      &ViewOpsType_move,
      &ViewOpsType_pan,
//    &ViewOpsType_orbit,
//    &ViewOpsType_roll,
//    &ViewOpsType_dolly,
#ifdef WITH_INPUT_NDOF
      &ViewOpsType_ndof_orbit,
      &ViewOpsType_ndof_orbit_zoom,
      &ViewOpsType_ndof_pan,
      &ViewOpsType_ndof_all,
#endif
  };

  const char *op_name = idname + sizeof("VIEW3D_OT_");
  for (const ViewOpsType *nav_type : nav_types) {
    if (STREQ(op_name, nav_type->idname + sizeof("VIEW3D_OT_"))) {
      return nav_type;
    }
  }
  return nullptr;
}

ViewOpsData *ED_view3d_navigation_init(bContext *C, const wmKeyMapItem *kmi_merge)
{
  /* Unlike #viewops_data_create, #ED_view3d_navigation_init creates a navigation context along
   * with an array of `wmKeyMapItem`s used for navigation. */
  if (!CTX_wm_region_view3d(C)) {
    return nullptr;
  }

  return new ViewOpsData_Utility(C, kmi_merge);
}

bool ED_view3d_navigation_do(bContext *C,
                             ViewOpsData *vod,
                             const wmEvent *event,
                             const float depth_loc_override[3])
{
  if (!vod) {
    return false;
  }

  wmEvent event_tmp;
  if (event->type == EVT_MODAL_MAP) {
    /* Workaround to use the original event values. */
    event_tmp = *event;
    event_tmp.type = event->prev_type;
    event_tmp.val = event->prev_val;
    event = &event_tmp;
  }

  wmOperatorStatus op_return = OPERATOR_CANCELLED;

  ViewOpsData_Utility *vod_intern = static_cast<ViewOpsData_Utility *>(vod);
  if (vod_intern->is_modal_event) {
    const eV3D_OpEvent event_code = view3d_navigate_event(vod, event);
    op_return = vod->nav_type->apply_fn(C, vod, event_code, event->xy);
    if (op_return != OPERATOR_RUNNING_MODAL) {
      vod->end_navigation(C);
      vod_intern->is_modal_event = false;
    }
  }
  else {
    LISTBASE_FOREACH (wmKeyMapItem *, kmi, &vod_intern->keymap_items) {
      if (!WM_event_match(event, kmi)) {
        continue;
      }

      const ViewOpsType *nav_type = view3d_navigation_type_from_idname(kmi->idname);
      if (nav_type->poll_fn && !nav_type->poll_fn(C)) {
        break;
      }

      op_return = view3d_navigation_invoke_generic(
          C, vod, event, kmi->ptr, nav_type, depth_loc_override);

      if (op_return == OPERATOR_RUNNING_MODAL) {
        vod_intern->is_modal_event = true;
      }
      else {
        vod->end_navigation(C);
        /* Postpone the navigation confirmation to the next call.
         * This avoids constant updating of the transform operation for example. */
        vod->rv3d->rflag |= RV3D_NAVIGATING;
      }
      break;
    }
  }

  if (op_return != OPERATOR_CANCELLED) {
    /* Although #ED_view3d_update_viewmat is already called when redrawing the 3D View, do it here
     * as well, so the updated matrix values can be accessed by the operator. */
    ED_view3d_update_viewmat(
        vod->depsgraph, vod->scene, vod->v3d, vod->region, nullptr, nullptr, nullptr, false);

    return true;
  }
  if (vod->rv3d->rflag & RV3D_NAVIGATING) {
    /* Add a fake confirmation. */
    vod->rv3d->rflag &= ~RV3D_NAVIGATING;
    return true;
  }

  return false;
}

void ED_view3d_navigation_free(bContext *C, ViewOpsData *vod)
{
  ViewOpsData_Utility *vod_intern = static_cast<ViewOpsData_Utility *>(vod);
  vod_intern->end_navigation(C);
  delete vod_intern;
}

/** \} */

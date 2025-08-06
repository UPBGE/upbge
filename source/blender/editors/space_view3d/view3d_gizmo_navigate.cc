/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"

#include "ED_gizmo_library.hh"
#include "ED_screen.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "view3d_intern.hh" /* own include */

/* -------------------------------------------------------------------- */
/** \name View3D Navigation Gizmo Group
 * \{ */

/* Size of main icon. */
#define GIZMO_SIZE U.gizmo_size_navigate_v3d

/* Main gizmo offset from screen edges in unscaled pixels. */
#define GIZMO_OFFSET 10.0f

/* Width of smaller buttons in unscaled pixels. */
#define GIZMO_MINI_SIZE 28.0f

/* Margin around the smaller buttons. */
#define GIZMO_MINI_OFFSET 2.0f

namespace {

enum {
  GZ_INDEX_MOVE = 0,
  GZ_INDEX_ROTATE = 1,
  GZ_INDEX_ZOOM = 2,

  /* just buttons */
  /* overlaps GZ_INDEX_ORTHO (switch between) */
  GZ_INDEX_PERSP = 3,
  GZ_INDEX_ORTHO = 4,

  GZ_INDEX_CAMERA_OFF = 5,
  GZ_INDEX_CAMERA_ON = 6,

  GZ_INDEX_CAMERA_LOCK = 7,
  GZ_INDEX_CAMERA_UNLOCK = 8,

  GZ_INDEX_TOTAL = 9,
};

struct NavigateGizmoInfo {
  const char *opname;
  const char *gizmo;
  uint icon;
  void (*op_prop_fn)(PointerRNA *ptr);
};

struct NavigateWidgetGroup {
  wmGizmo *gz_array[GZ_INDEX_TOTAL];
  /* Store the view state to check for changes. */
  struct {
    rcti rect_visible;
    struct {
      char is_persp;
      bool is_camera;
      char viewlock;
      char cameralock;
    } rv3d;
  } state;
};

}  // namespace

static void navigate_context_toggle_camera_lock_init(PointerRNA *ptr)
{
  RNA_string_set(ptr, "data_path", "space_data.lock_camera");
}

static NavigateGizmoInfo g_navigate_params[GZ_INDEX_TOTAL] = {
    {
        "VIEW3D_OT_move",
        "GIZMO_GT_button_2d",
        ICON_VIEW_PAN,
        nullptr,
    },
    {
        "VIEW3D_OT_rotate",
        "VIEW3D_GT_navigate_rotate",
        ICON_NONE,
        nullptr,
    },
    {
        "VIEW3D_OT_zoom",
        "GIZMO_GT_button_2d",
        ICON_VIEW_ZOOM,
        nullptr,
    },
    {
        "VIEW3D_OT_view_persportho",
        "GIZMO_GT_button_2d",
        ICON_VIEW_PERSPECTIVE,
        nullptr,
    },
    {
        "VIEW3D_OT_view_persportho",
        "GIZMO_GT_button_2d",
        ICON_VIEW_ORTHO,
        nullptr,
    },
    {
        "VIEW3D_OT_view_camera",
        "GIZMO_GT_button_2d",
        ICON_VIEW_CAMERA_UNSELECTED,
        nullptr,
    },
    {
        "VIEW3D_OT_view_camera",
        "GIZMO_GT_button_2d",
        ICON_VIEW_CAMERA,
        nullptr,
    },
    {
        "WM_OT_context_toggle",
        "GIZMO_GT_button_2d",
        ICON_VIEW_LOCKED,
        navigate_context_toggle_camera_lock_init,
    },
    {
        "WM_OT_context_toggle",
        "GIZMO_GT_button_2d",
        ICON_VIEW_UNLOCKED,
        navigate_context_toggle_camera_lock_init,
    },
};

static bool WIDGETGROUP_navigate_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  View3D *v3d = CTX_wm_view3d(C);
  if ((((U.uiflag & USER_SHOW_GIZMO_NAVIGATE) == 0) &&
       (U.mini_axis_type != USER_MINI_AXIS_TYPE_GIZMO)) ||
      (v3d->gizmo_flag & (V3D_GIZMO_HIDE | V3D_GIZMO_HIDE_NAVIGATE)))
  {
    return false;
  }
  return true;
}

static void WIDGETGROUP_navigate_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  NavigateWidgetGroup *navgroup = MEM_callocN<NavigateWidgetGroup>(__func__);

  wmOperatorType *ot_view_axis = WM_operatortype_find("VIEW3D_OT_view_axis", true);
  wmOperatorType *ot_view_camera = WM_operatortype_find("VIEW3D_OT_view_camera", true);

  for (int i = 0; i < GZ_INDEX_TOTAL; i++) {
    const NavigateGizmoInfo *info = &g_navigate_params[i];
    navgroup->gz_array[i] = WM_gizmo_new(info->gizmo, gzgroup, nullptr);
    wmGizmo *gz = navgroup->gz_array[i];
    gz->flag |= WM_GIZMO_MOVE_CURSOR | WM_GIZMO_DRAW_MODAL;

    if (i == GZ_INDEX_ROTATE) {
      gz->color[3] = 0.0f;
      copy_v3_fl(gz->color_hi, 0.5f);
      gz->color_hi[3] = 0.5f;
    }
    else {
      uchar icon_color[3];
      UI_GetThemeColor3ubv(TH_TEXT, icon_color);
      int color_tint, color_tint_hi;
      if (icon_color[0] > 128) {
        color_tint = -40;
        color_tint_hi = 60;
        gz->color[3] = 0.5f;
        gz->color_hi[3] = 0.5f;
      }
      else {
        color_tint = 60;
        color_tint_hi = 60;
        gz->color[3] = 0.5f;
        gz->color_hi[3] = 0.75f;
      }
      UI_GetThemeColorShade3fv(TH_HEADER, color_tint, gz->color);
      UI_GetThemeColorShade3fv(TH_HEADER, color_tint_hi, gz->color_hi);
    }

    /* may be overwritten later */
    gz->scale_basis = GIZMO_MINI_SIZE / 2.0f;
    if (info->icon != ICON_NONE) {
      PropertyRNA *prop = RNA_struct_find_property(gz->ptr, "icon");
      RNA_property_enum_set(gz->ptr, prop, info->icon);
      RNA_enum_set(
          gz->ptr, "draw_options", ED_GIZMO_BUTTON_SHOW_OUTLINE | ED_GIZMO_BUTTON_SHOW_BACKDROP);
    }

    wmOperatorType *ot = WM_operatortype_find(info->opname, true);
#ifndef WITH_PYTHON
    if (ot != nullptr)
#endif
    {
      PointerRNA *ptr = WM_gizmo_operator_set(gz, 0, ot, nullptr);
      if (info->op_prop_fn != nullptr) {
        info->op_prop_fn(ptr);
      }
    }
  }

  {
    wmGizmo *gz = navgroup->gz_array[GZ_INDEX_CAMERA_OFF];
    WM_gizmo_operator_set(gz, 0, ot_view_camera, nullptr);
  }
  {
    wmGizmo *gz = navgroup->gz_array[GZ_INDEX_CAMERA_ON];
    WM_gizmo_operator_set(gz, 0, ot_view_camera, nullptr);
  }

  /* Click only buttons (not modal). */
  {
    int gz_ids[] = {GZ_INDEX_PERSP,
                    GZ_INDEX_ORTHO,
                    GZ_INDEX_CAMERA_OFF,
                    GZ_INDEX_CAMERA_ON,
                    GZ_INDEX_CAMERA_LOCK,
                    GZ_INDEX_CAMERA_UNLOCK};
    for (int i = 0; i < ARRAY_SIZE(gz_ids); i++) {
      wmGizmo *gz = navgroup->gz_array[gz_ids[i]];
      RNA_boolean_set(gz->ptr, "show_drag", false);
    }
  }

  /* Modal operators, don't use initial mouse location since we're clicking on a button. */
  {
    int gz_ids[] = {GZ_INDEX_MOVE, GZ_INDEX_ROTATE, GZ_INDEX_ZOOM};
    for (int i = 0; i < ARRAY_SIZE(gz_ids); i++) {
      wmGizmo *gz = navgroup->gz_array[gz_ids[i]];
      wmGizmoOpElem *gzop = WM_gizmo_operator_get(gz, 0);
      RNA_boolean_set(&gzop->ptr, "use_cursor_init", false);
    }
  }

  {
    wmGizmo *gz = navgroup->gz_array[GZ_INDEX_ROTATE];
    gz->scale_basis = GIZMO_SIZE / 2.0f;
    const char mapping[6] = {
        RV3D_VIEW_LEFT,
        RV3D_VIEW_RIGHT,
        RV3D_VIEW_FRONT,
        RV3D_VIEW_BACK,
        RV3D_VIEW_BOTTOM,
        RV3D_VIEW_TOP,
    };

    for (int part_index = 0; part_index < 6; part_index += 1) {
      PointerRNA *ptr = WM_gizmo_operator_set(gz, part_index + 1, ot_view_axis, nullptr);
      RNA_enum_set(ptr, "type", mapping[part_index]);
    }

    /* When dragging an axis, use this instead. */
    wmWindowManager *wm = CTX_wm_manager(C);
    gz->keymap = WM_gizmo_keymap_generic_press_drag(wm);
    gz->drag_part = 0;
  }

  gzgroup->customdata = navgroup;
}

static void WIDGETGROUP_navigate_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  NavigateWidgetGroup *navgroup = static_cast<NavigateWidgetGroup *>(gzgroup->customdata);
  ARegion *region = CTX_wm_region(C);
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  const View3D *v3d = CTX_wm_view3d(C);

  for (int i = 0; i < 3; i++) {
    copy_v3_v3(navgroup->gz_array[GZ_INDEX_ROTATE]->matrix_offset[i], rv3d->viewmat[i]);
  }

  const rcti *rect_visible = ED_region_visible_rect(region);

  /* Ensure types match so bits are never lost on assignment. */
  CHECK_TYPE_PAIR(navgroup->state.rv3d.viewlock, rv3d->viewlock);

  if ((navgroup->state.rect_visible.xmax == rect_visible->xmax) &&
      (navgroup->state.rect_visible.ymax == rect_visible->ymax) &&
      (navgroup->state.rv3d.is_persp == rv3d->is_persp) &&
      (navgroup->state.rv3d.is_camera == (rv3d->persp == RV3D_CAMOB)) &&
      (navgroup->state.rv3d.cameralock == (v3d->flag2 & V3D_LOCK_CAMERA)) &&
      (navgroup->state.rv3d.viewlock == RV3D_LOCK_FLAGS(rv3d)))
  {
    return;
  }

  navgroup->state.rect_visible = *rect_visible;
  navgroup->state.rv3d.is_persp = rv3d->is_persp;
  navgroup->state.rv3d.is_camera = (rv3d->persp == RV3D_CAMOB);
  navgroup->state.rv3d.viewlock = RV3D_LOCK_FLAGS(rv3d);
  navgroup->state.rv3d.cameralock = v3d->flag2 & V3D_LOCK_CAMERA;

  const bool show_navigate = (U.uiflag & USER_SHOW_GIZMO_NAVIGATE) != 0;
  const bool show_rotate_gizmo = (U.mini_axis_type == USER_MINI_AXIS_TYPE_GIZMO);
  const float icon_offset = ((GIZMO_SIZE / 2.0f) + GIZMO_OFFSET) * UI_SCALE_FAC;
  const float icon_offset_mini = (GIZMO_MINI_SIZE + GIZMO_MINI_OFFSET) * UI_SCALE_FAC;
  const float co_rotate[2] = {
      rect_visible->xmax - icon_offset,
      rect_visible->ymax - icon_offset,
  };

  float icon_offset_from_axis = 0.0f;
  switch ((eUserpref_MiniAxisType)U.mini_axis_type) {
    case USER_MINI_AXIS_TYPE_GIZMO:
      icon_offset_from_axis = icon_offset * 2.1f;
      break;
    case USER_MINI_AXIS_TYPE_MINIMAL:
      icon_offset_from_axis = (UI_UNIT_X * 2.5) + (U.rvisize * U.pixelsize * 2.0f);
      break;
    case USER_MINI_AXIS_TYPE_NONE:
      icon_offset_from_axis = icon_offset_mini * 0.75f;
      break;
  }

  const float co[2] = {
      roundf(rect_visible->xmax - icon_offset_mini * 0.75f),
      roundf(rect_visible->ymax - icon_offset_from_axis),
  };

  wmGizmo *gz;

  for (uint i = 0; i < ARRAY_SIZE(navgroup->gz_array); i++) {
    gz = navgroup->gz_array[i];
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
  }

  if (show_rotate_gizmo) {
    gz = navgroup->gz_array[GZ_INDEX_ROTATE];
    gz->matrix_basis[3][0] = roundf(co_rotate[0]);
    gz->matrix_basis[3][1] = roundf(co_rotate[1]);
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
  }

  if (show_navigate) {
    int icon_mini_slot = 0;
    if ((RV3D_LOCK_FLAGS(rv3d) & RV3D_LOCK_ZOOM_AND_DOLLY) == 0) {
      gz = navgroup->gz_array[GZ_INDEX_ZOOM];
      gz->matrix_basis[3][0] = roundf(co[0]);
      gz->matrix_basis[3][1] = roundf(co[1] - (icon_offset_mini * icon_mini_slot++));
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
    }

    if ((RV3D_LOCK_FLAGS(rv3d) & RV3D_LOCK_LOCATION) == 0) {
      gz = navgroup->gz_array[GZ_INDEX_MOVE];
      gz->matrix_basis[3][0] = roundf(co[0]);
      gz->matrix_basis[3][1] = roundf(co[1] - (icon_offset_mini * icon_mini_slot++));
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
    }

    if ((RV3D_LOCK_FLAGS(rv3d) & RV3D_LOCK_ROTATION) == 0) {
      gz = navgroup
               ->gz_array[(rv3d->persp == RV3D_CAMOB) ? GZ_INDEX_CAMERA_ON : GZ_INDEX_CAMERA_OFF];
      gz->matrix_basis[3][0] = roundf(co[0]);
      gz->matrix_basis[3][1] = roundf(co[1] - (icon_offset_mini * icon_mini_slot++));
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);

      if (navgroup->state.rv3d.is_camera == false) {
        gz = navgroup->gz_array[rv3d->is_persp ? GZ_INDEX_PERSP : GZ_INDEX_ORTHO];
        gz->matrix_basis[3][0] = roundf(co[0]);
        gz->matrix_basis[3][1] = roundf(co[1] - (icon_offset_mini * icon_mini_slot++));
        WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
      }
    }

    if (navgroup->state.rv3d.is_camera == true) {
      gz = navgroup->gz_array[(v3d->flag2 & V3D_LOCK_CAMERA) ? GZ_INDEX_CAMERA_LOCK :
                                                               GZ_INDEX_CAMERA_UNLOCK];
      gz->matrix_basis[3][0] = roundf(co[0]);
      gz->matrix_basis[3][1] = roundf(co[1] - (icon_offset_mini * icon_mini_slot++));
      WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);
    }
  }
}

void VIEW3D_GGT_navigate(wmGizmoGroupType *gzgt)
{
  gzgt->name = "View3D Navigate";
  gzgt->idname = "VIEW3D_GGT_navigate";

  gzgt->flag |= (WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_SCALE |
                 WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL);

  gzgt->poll = WIDGETGROUP_navigate_poll;
  gzgt->setup = WIDGETGROUP_navigate_setup;
  gzgt->draw_prepare = WIDGETGROUP_navigate_draw_prepare;
}

/** \} */

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spgraph
 *
 * Graph editor space & buttons.
 */

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_anim_data.h"
#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_fcurve.h"
#include "BKE_fcurve_driver.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_screen.h"
#include "BKE_unit.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_path.h"
#include "RNA_prototypes.h"

#include "ED_anim_api.h"
#include "ED_keyframing.h"
#include "ED_screen.h"
#include "ED_undo.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "graph_intern.h" /* own include */

#define B_REDR 1

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

static bool graph_panel_context(const bContext *C, bAnimListElem **ale, FCurve **fcu)
{
  bAnimContext ac;
  bAnimListElem *elem = NULL;

  /* For now, only draw if we could init the anim-context info
   * (necessary for all animation-related tools)
   * to work correctly is able to be correctly retrieved.
   * There's no point showing empty panels?
   */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return false;
  }

  /* try to find 'active' F-Curve */
  elem = get_active_fcurve_channel(&ac);
  if (elem == NULL) {
    return false;
  }

  if (fcu) {
    *fcu = (FCurve *)elem->data;
  }
  if (ale) {
    *ale = elem;
  }
  else {
    MEM_freeN(elem);
  }

  return true;
}

FCurve *ANIM_graph_context_fcurve(const bContext *C)
{
  FCurve *fcu;
  if (!graph_panel_context(C, NULL, &fcu)) {
    return NULL;
  }

  return fcu;
}

static bool graph_panel_poll(const bContext *C, PanelType *UNUSED(pt))
{
  return graph_panel_context(C, NULL, NULL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cursor Header
 * \{ */

static void graph_panel_cursor_header(const bContext *C, Panel *panel)
{
  bScreen *screen = CTX_wm_screen(C);
  SpaceGraph *sipo = CTX_wm_space_graph(C);
  Scene *scene = CTX_data_scene(C);
  PointerRNA spaceptr, sceneptr;
  uiLayout *col;

  /* get RNA pointers for use when creating the UI elements */
  RNA_id_pointer_create(&scene->id, &sceneptr);
  RNA_pointer_create(&screen->id, &RNA_SpaceGraphEditor, sipo, &spaceptr);

  /* 2D-Cursor */
  col = uiLayoutColumn(panel->layout, false);
  uiItemR(col, &spaceptr, "show_cursor", 0, "", ICON_NONE);
}

static void graph_panel_cursor(const bContext *C, Panel *panel)
{
  bScreen *screen = CTX_wm_screen(C);
  SpaceGraph *sipo = CTX_wm_space_graph(C);
  Scene *scene = CTX_data_scene(C);
  PointerRNA spaceptr, sceneptr;
  uiLayout *layout = panel->layout;
  uiLayout *col, *sub;

  /* get RNA pointers for use when creating the UI elements */
  RNA_id_pointer_create(&scene->id, &sceneptr);
  RNA_pointer_create(&screen->id, &RNA_SpaceGraphEditor, sipo, &spaceptr);

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  /* 2D-Cursor */
  col = uiLayoutColumn(layout, false);
  uiLayoutSetActive(col, RNA_boolean_get(&spaceptr, "show_cursor"));

  sub = uiLayoutColumn(col, true);
  if (sipo->mode == SIPO_MODE_DRIVERS) {
    uiItemR(sub, &spaceptr, "cursor_position_x", 0, IFACE_("Cursor X"), ICON_NONE);
  }
  else {
    uiItemR(sub, &sceneptr, "frame_current", 0, IFACE_("Cursor X"), ICON_NONE);
  }

  uiItemR(sub, &spaceptr, "cursor_position_y", 0, IFACE_("Y"), ICON_NONE);

  sub = uiLayoutColumn(col, true);
  uiItemO(sub, IFACE_("Cursor to Selection"), ICON_NONE, "GRAPH_OT_frame_jump");
  uiItemO(sub, IFACE_("Cursor Value to Selection"), ICON_NONE, "GRAPH_OT_snap_cursor_value");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Active F-Curve
 * \{ */

static void graph_panel_properties(const bContext *C, Panel *panel)
{
  bAnimListElem *ale;
  FCurve *fcu;
  PointerRNA fcu_ptr;
  uiLayout *layout = panel->layout;
  uiLayout *col;
  char name[256];
  int icon = 0;

  if (!graph_panel_context(C, &ale, &fcu)) {
    return;
  }

  /* F-Curve pointer */
  RNA_pointer_create(ale->fcurve_owner_id, &RNA_FCurve, fcu, &fcu_ptr);

  /* user-friendly 'name' for F-Curve */
  col = uiLayoutColumn(layout, false);
  if (ale->type == ANIMTYPE_FCURVE) {
    /* get user-friendly name for F-Curve */
    icon = getname_anim_fcurve(name, ale->id, fcu);
  }
  else {
    /* NLA Control Curve, etc. */
    const bAnimChannelType *acf = ANIM_channel_get_typeinfo(ale);

    /* get name */
    if (acf && acf->name) {
      acf->name(ale, name);
    }
    else {
      strcpy(name, IFACE_("<invalid>"));
      icon = ICON_ERROR;
    }

    /* icon */
    if (ale->type == ANIMTYPE_NLACURVE) {
      icon = ICON_NLA;
    }
  }
  uiItemL(col, name, icon);

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  /* RNA-Path Editing - only really should be enabled when things aren't working */
  col = uiLayoutColumn(layout, false);
  uiLayoutSetEnabled(col, (fcu->flag & FCURVE_DISABLED) != 0);
  uiItemR(col, &fcu_ptr, "data_path", 0, "", ICON_RNA);
  uiItemR(col, &fcu_ptr, "array_index", 0, NULL, ICON_NONE);

  /* color settings */
  col = uiLayoutColumn(layout, true);
  uiItemR(col, &fcu_ptr, "color_mode", 0, "Display Color", ICON_NONE);

  if (fcu->color_mode == FCURVE_COLOR_CUSTOM) {
    uiItemR(col, &fcu_ptr, "color", 0, "Color", ICON_NONE);
  }

  /* smoothing setting */
  col = uiLayoutColumn(layout, true);
  uiItemR(col, &fcu_ptr, "auto_smoothing", 0, "Handle Smoothing", ICON_NONE);

  MEM_freeN(ale);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Active Keyframe
 * \{ */

/* get 'active' keyframe for panel editing */
static bool get_active_fcurve_keyframe_edit(const FCurve *fcu,
                                            BezTriple **r_bezt,
                                            BezTriple **r_prevbezt)
{
  /* zero the pointers */
  *r_bezt = *r_prevbezt = NULL;

  const int active_keyframe_index = BKE_fcurve_active_keyframe_index(fcu);
  if (active_keyframe_index == FCURVE_ACTIVE_KEYFRAME_NONE) {
    return false;
  }

  /* The active keyframe should be selected. */
  BLI_assert(BEZT_ISSEL_ANY(&fcu->bezt[active_keyframe_index]));

  *r_bezt = &fcu->bezt[active_keyframe_index];
  /* Previous is either one before the active, or the point itself if it's the first. */
  const int prev_index = max_ii(active_keyframe_index - 1, 0);
  *r_prevbezt = &fcu->bezt[prev_index];

  return true;
}

/* update callback for active keyframe properties - base updates stuff */
static void graphedit_activekey_update_cb(bContext *UNUSED(C),
                                          void *fcu_ptr,
                                          void *UNUSED(bezt_ptr))
{
  FCurve *fcu = (FCurve *)fcu_ptr;

  /* make sure F-Curve and its handles are still valid after this editing */
  sort_time_fcurve(fcu);
  BKE_fcurve_handles_recalc(fcu);
}

/* update callback for active keyframe properties - handle-editing wrapper */
static void graphedit_activekey_handles_cb(bContext *C, void *fcu_ptr, void *bezt_ptr)
{
  BezTriple *bezt = (BezTriple *)bezt_ptr;

  /* since editing the handles, make sure they're set to types which are receptive to editing
   * see transform_conversions.c :: createTransGraphEditData(), last step in second loop
   */
  if (ELEM(bezt->h1, HD_AUTO, HD_AUTO_ANIM) && ELEM(bezt->h2, HD_AUTO, HD_AUTO_ANIM)) {
    /* by changing to aligned handles, these can now be moved... */
    bezt->h1 = HD_ALIGN;
    bezt->h2 = HD_ALIGN;
  }
  else {
    BKE_nurb_bezt_handle_test(bezt, SELECT, true, false);
  }

  /* now call standard updates */
  graphedit_activekey_update_cb(C, fcu_ptr, bezt_ptr);
}

/* update callback for editing coordinates of right handle in active keyframe properties
 * NOTE: we cannot just do graphedit_activekey_handles_cb() due to "order of computation"
 *       weirdness (see calchandleNurb_intern() and T39911)
 */
static void graphedit_activekey_left_handle_coord_cb(bContext *C, void *fcu_ptr, void *bezt_ptr)
{
  BezTriple *bezt = (BezTriple *)bezt_ptr;

  const char f1 = bezt->f1;
  const char f3 = bezt->f3;

  bezt->f1 |= SELECT;
  bezt->f3 &= ~SELECT;

  /* perform normal updates NOW */
  graphedit_activekey_handles_cb(C, fcu_ptr, bezt_ptr);

  /* restore selection state so that no one notices this hack */
  bezt->f1 = f1;
  bezt->f3 = f3;
}

static void graphedit_activekey_right_handle_coord_cb(bContext *C, void *fcu_ptr, void *bezt_ptr)
{
  BezTriple *bezt = (BezTriple *)bezt_ptr;

  /* original state of handle selection - to be restored after performing the recalculation */
  const char f1 = bezt->f1;
  const char f3 = bezt->f3;

  /* temporarily make it so that only the right handle is selected, so that updates go correctly
   * (i.e. it now acts as if we've just transforming the vert when it is selected by itself)
   */
  bezt->f1 &= ~SELECT;
  bezt->f3 |= SELECT;

  /* perform normal updates NOW */
  graphedit_activekey_handles_cb(C, fcu_ptr, bezt_ptr);

  /* restore selection state so that no one notices this hack */
  bezt->f1 = f1;
  bezt->f3 = f3;
}

static void graph_panel_key_properties(const bContext *C, Panel *panel)
{
  bAnimListElem *ale;
  FCurve *fcu;
  BezTriple *bezt, *prevbezt;

  uiLayout *layout = panel->layout;
  const ARegion *region = CTX_wm_region(C);
  /* Just a width big enough so buttons use entire layout width (will be clamped by it then). */
  const int but_max_width = region->winx;
  uiLayout *col;
  uiBlock *block;

  if (!graph_panel_context(C, &ale, &fcu)) {
    return;
  }

  block = uiLayoutGetBlock(layout);
  // UI_block_func_handle_set(block, do_graph_region_buttons, NULL);
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  /* only show this info if there are keyframes to edit */
  if (get_active_fcurve_keyframe_edit(fcu, &bezt, &prevbezt)) {
    PointerRNA bezt_ptr, id_ptr, fcu_prop_ptr;
    PropertyRNA *fcu_prop = NULL;
    uiBut *but;
    int unit = B_UNIT_NONE;

    /* RNA pointer to keyframe, to allow editing */
    RNA_pointer_create(ale->fcurve_owner_id, &RNA_Keyframe, bezt, &bezt_ptr);

    /* get property that F-Curve affects, for some unit-conversion magic */
    RNA_id_pointer_create(ale->id, &id_ptr);
    if (RNA_path_resolve_property(&id_ptr, fcu->rna_path, &fcu_prop_ptr, &fcu_prop)) {
      /* determine the unit for this property */
      unit = RNA_SUBTYPE_UNIT(RNA_property_subtype(fcu_prop));
    }

    /* interpolation */
    col = uiLayoutColumn(layout, false);
    if (fcu->flag & FCURVE_DISCRETE_VALUES) {
      uiLayout *split = uiLayoutSplit(col, 0.33f, true);
      uiItemL(split, IFACE_("Interpolation:"), ICON_NONE);
      uiItemL(split, IFACE_("None for Enum/Boolean"), ICON_IPO_CONSTANT);
    }
    else {
      uiItemR(col, &bezt_ptr, "interpolation", 0, NULL, ICON_NONE);
    }

    /* easing type */
    if (bezt->ipo > BEZT_IPO_BEZ) {
      uiItemR(col, &bezt_ptr, "easing", 0, NULL, 0);
    }

    /* easing extra */
    switch (bezt->ipo) {
      case BEZT_IPO_BACK:
        col = uiLayoutColumn(layout, 1);
        uiItemR(col, &bezt_ptr, "back", 0, NULL, 0);
        break;
      case BEZT_IPO_ELASTIC:
        col = uiLayoutColumn(layout, 1);
        uiItemR(col, &bezt_ptr, "amplitude", 0, NULL, 0);
        uiItemR(col, &bezt_ptr, "period", 0, NULL, 0);
        break;
      default:
        break;
    }

    /* numerical coordinate editing
     * - we use the button-versions of the calls so that we can attach special update handlers
     *   and unit conversion magic that cannot be achieved using a purely RNA-approach
     */
    col = uiLayoutColumn(layout, true);
    /* keyframe itself */
    {
      uiItemL_respect_property_split(col, IFACE_("Key Frame"), ICON_NONE);
      but = uiDefButR(block,
                      UI_BTYPE_NUM,
                      B_REDR,
                      "",
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "co_ui",
                      0,
                      0,
                      0,
                      0,
                      0,
                      NULL);
      UI_but_func_set(but, graphedit_activekey_update_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Value"), ICON_NONE);
      but = uiDefButR(block,
                      UI_BTYPE_NUM,
                      B_REDR,
                      "",
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "co_ui",
                      1,
                      0,
                      0,
                      0,
                      0,
                      NULL);
      UI_but_func_set(but, graphedit_activekey_update_cb, fcu, bezt);
      UI_but_unit_type_set(but, unit);
    }

    /* previous handle - only if previous was Bezier interpolation */
    if ((prevbezt) && (prevbezt->ipo == BEZT_IPO_BEZ)) {

      col = uiLayoutColumn(layout, true);
      uiItemL_respect_property_split(col, IFACE_("Left Handle Type"), ICON_NONE);
      but = uiDefButR(block,
                      UI_BTYPE_MENU,
                      B_REDR,
                      NULL,
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "handle_left_type",
                      0,
                      0,
                      0,
                      -1,
                      -1,
                      "Type of left handle");
      UI_but_func_set(but, graphedit_activekey_handles_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Frame"), ICON_NONE);
      but = uiDefButR(block,
                      UI_BTYPE_NUM,
                      B_REDR,
                      "",
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "handle_left",
                      0,
                      0,
                      0,
                      0,
                      0,
                      NULL);
      UI_but_func_set(but, graphedit_activekey_left_handle_coord_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Value"), ICON_NONE);
      but = uiDefButR(block,
                      UI_BTYPE_NUM,
                      B_REDR,
                      "",
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "handle_left",
                      1,
                      0,
                      0,
                      0,
                      0,
                      NULL);
      UI_but_func_set(but, graphedit_activekey_left_handle_coord_cb, fcu, bezt);
      UI_but_unit_type_set(but, unit);
    }

    /* next handle - only if current is Bezier interpolation */
    if (bezt->ipo == BEZT_IPO_BEZ) {
      /* NOTE: special update callbacks are needed on the coords here due to T39911 */

      col = uiLayoutColumn(layout, true);
      uiItemL_respect_property_split(col, IFACE_("Right Handle Type"), ICON_NONE);
      but = uiDefButR(block,
                      UI_BTYPE_MENU,
                      B_REDR,
                      NULL,
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "handle_right_type",
                      0,
                      0,
                      0,
                      -1,
                      -1,
                      "Type of right handle");
      UI_but_func_set(but, graphedit_activekey_handles_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Frame"), ICON_NONE);
      but = uiDefButR(block,
                      UI_BTYPE_NUM,
                      B_REDR,
                      "",
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "handle_right",
                      0,
                      0,
                      0,
                      0,
                      0,
                      NULL);
      UI_but_func_set(but, graphedit_activekey_right_handle_coord_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Value"), ICON_NONE);
      but = uiDefButR(block,
                      UI_BTYPE_NUM,
                      B_REDR,
                      "",
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "handle_right",
                      1,
                      0,
                      0,
                      0,
                      0,
                      NULL);
      UI_but_func_set(but, graphedit_activekey_right_handle_coord_cb, fcu, bezt);
      UI_but_unit_type_set(but, unit);
    }
  }
  else {
    if ((fcu->bezt == NULL) && (fcu->modifiers.first)) {
      /* modifiers only - so no keyframes to be active */
      uiItemL(layout, TIP_("F-Curve only has F-Modifiers"), ICON_NONE);
      uiItemL(layout, TIP_("See Modifiers panel below"), ICON_INFO);
    }
    else if (fcu->fpt) {
      /* samples only */
      uiItemL(layout,
              TIP_("F-Curve doesn't have any keyframes as it only contains sampled points"),
              ICON_NONE);
    }
    else {
      uiItemL(layout, TIP_("No active keyframe on F-Curve"), ICON_NONE);
    }
  }

  MEM_freeN(ale);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drivers
 * \{ */

#define B_IPO_DEPCHANGE 10

static void do_graph_region_driver_buttons(bContext *C, void *id_v, int event)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);

  switch (event) {
    case B_IPO_DEPCHANGE: {
      /* Was not actually run ever (NULL always passed as arg to this callback).
       * If needed again, will need to check how to pass both fcurve and ID... :/ */
#if 0
      /* force F-Curve & Driver to get re-evaluated (same as the old Update Dependencies) */
      FCurve *fcu = (FCurve *)fcu_v;
      ChannelDriver *driver = (fcu) ? fcu->driver : NULL;

      /* clear invalid flags */
      if (fcu) {
        fcu->flag &= ~FCURVE_DISABLED;
        driver->flag &= ~DRIVER_FLAG_INVALID;
      }
#endif
      ID *id = id_v;
      AnimData *adt = BKE_animdata_from_id(id);

      /* rebuild depsgraph for the new deps, and ensure COW copies get flushed. */
      DEG_relations_tag_update(bmain);
      DEG_id_tag_update_ex(bmain, id, ID_RECALC_COPY_ON_WRITE);
      if (adt != NULL) {
        if (adt->action != NULL) {
          DEG_id_tag_update_ex(bmain, &adt->action->id, ID_RECALC_COPY_ON_WRITE);
        }
        if (adt->tmpact != NULL) {
          DEG_id_tag_update_ex(bmain, &adt->tmpact->id, ID_RECALC_COPY_ON_WRITE);
        }
      }

      break;
    }
  }

  /* default for now */
  WM_event_add_notifier(C, NC_SCENE | ND_FRAME, scene); /* XXX could use better notifier */
}

/* callback to add a target variable to the active driver */
static void driver_add_var_cb(bContext *C, void *driver_v, void *UNUSED(arg))
{
  ChannelDriver *driver = (ChannelDriver *)driver_v;

  /* add a new variable */
  driver_add_new_variable(driver);
  ED_undo_push(C, "Add Driver Variable");
}

/* callback to remove target variable from active driver */
static void driver_delete_var_cb(bContext *C, void *driver_v, void *dvar_v)
{
  ChannelDriver *driver = (ChannelDriver *)driver_v;
  DriverVar *dvar = (DriverVar *)dvar_v;

  /* remove the active variable */
  driver_free_variable_ex(driver, dvar);
  ED_undo_push(C, "Delete Driver Variable");
}

/* callback to report why a driver variable is invalid */
static void driver_dvar_invalid_name_query_cb(bContext *C, void *dvar_v, void *UNUSED(arg))
{
  uiPopupMenu *pup = UI_popup_menu_begin(
      C, CTX_IFACE_(BLT_I18NCONTEXT_OPERATOR_DEFAULT, "Invalid Variable Name"), ICON_NONE);
  uiLayout *layout = UI_popup_menu_layout(pup);

  DriverVar *dvar = (DriverVar *)dvar_v;

  if (dvar->flag & DVAR_FLAG_INVALID_EMPTY) {
    uiItemL(layout, TIP_("It cannot be left blank"), ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_START_NUM) {
    uiItemL(layout, TIP_("It cannot start with a number"), ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_START_CHAR) {
    uiItemL(layout,
            TIP_("It cannot start with a special character,"
                 " including '$', '@', '!', '~', '+', '-', '_', '.', or ' '"),
            ICON_NONE);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_HAS_SPACE) {
    uiItemL(layout, TIP_("It cannot contain spaces (e.g. 'a space')"), ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_HAS_DOT) {
    uiItemL(layout, TIP_("It cannot contain dots (e.g. 'a.dot')"), ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_HAS_SPECIAL) {
    uiItemL(layout,
            TIP_("It cannot contain special (non-alphabetical/numeric) characters"),
            ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_PY_KEYWORD) {
    uiItemL(layout, TIP_("It cannot be a reserved keyword in Python"), ICON_INFO);
  }

  UI_popup_menu_end(C, pup);
}

/* callback to reset the driver's flags */
static void driver_update_flags_cb(bContext *UNUSED(C), void *fcu_v, void *UNUSED(arg))
{
  FCurve *fcu = (FCurve *)fcu_v;
  ChannelDriver *driver = fcu->driver;

  /* clear invalid flags */
  fcu->flag &= ~FCURVE_DISABLED;
  driver->flag &= ~DRIVER_FLAG_INVALID;
}

/* drivers panel poll */
static bool graph_panel_drivers_poll(const bContext *C, PanelType *UNUSED(pt))
{
  SpaceGraph *sipo = CTX_wm_space_graph(C);

  if (sipo->mode != SIPO_MODE_DRIVERS) {
    return false;
  }

  return graph_panel_context(C, NULL, NULL);
}

/* settings for 'single property' driver variable type */
static void graph_panel_driverVar__singleProp(uiLayout *layout, ID *id, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];
  PointerRNA dtar_ptr;
  uiLayout *row, *col;

  /* initialize RNA pointer to the target */
  RNA_pointer_create(id, &RNA_DriverTarget, dtar, &dtar_ptr);

  /* Target ID */
  row = uiLayoutRow(layout, false);
  uiLayoutSetRedAlert(row, ((dtar->flag & DTAR_FLAG_INVALID) && !dtar->id));
  uiTemplateAnyID(row, &dtar_ptr, "id", "id_type", IFACE_("Prop:"));

  /* Target Property */
  if (dtar->id) {
    PointerRNA root_ptr;

    /* get pointer for resolving the property selected */
    RNA_id_pointer_create(dtar->id, &root_ptr);

    /* rna path */
    col = uiLayoutColumn(layout, true);
    uiLayoutSetRedAlert(col, (dtar->flag & DTAR_FLAG_INVALID));
    uiTemplatePathBuilder(col, &dtar_ptr, "data_path", &root_ptr, IFACE_("Path"));
  }
}

/* settings for 'rotation difference' driver variable type */
/* FIXME: 1) Must be same armature for both dtars, 2) Alignment issues... */
static void graph_panel_driverVar__rotDiff(uiLayout *layout, ID *id, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];
  DriverTarget *dtar2 = &dvar->targets[1];
  Object *ob1 = (Object *)dtar->id;
  Object *ob2 = (Object *)dtar2->id;
  PointerRNA dtar_ptr, dtar2_ptr;
  uiLayout *col;

  /* initialize RNA pointer to the target */
  RNA_pointer_create(id, &RNA_DriverTarget, dtar, &dtar_ptr);
  RNA_pointer_create(id, &RNA_DriverTarget, dtar2, &dtar2_ptr);

  /* Object 1 */
  col = uiLayoutColumn(layout, true);
  uiLayoutSetRedAlert(col, (dtar->flag & DTAR_FLAG_INVALID)); /* XXX: per field... */
  uiItemR(col, &dtar_ptr, "id", 0, IFACE_("Object 1"), ICON_NONE);

  if (dtar->id && GS(dtar->id->name) == ID_OB && ob1->pose) {
    PointerRNA tar_ptr;

    RNA_pointer_create(dtar->id, &RNA_Pose, ob1->pose, &tar_ptr);
    uiItemPointerR(col, &dtar_ptr, "bone_target", &tar_ptr, "bones", "", ICON_BONE_DATA);
  }

  /* Object 2 */
  col = uiLayoutColumn(layout, true);
  uiLayoutSetRedAlert(col, (dtar2->flag & DTAR_FLAG_INVALID)); /* XXX: per field... */
  uiItemR(col, &dtar2_ptr, "id", 0, IFACE_("Object 2"), ICON_NONE);

  if (dtar2->id && GS(dtar2->id->name) == ID_OB && ob2->pose) {
    PointerRNA tar_ptr;

    RNA_pointer_create(dtar2->id, &RNA_Pose, ob2->pose, &tar_ptr);
    uiItemPointerR(col, &dtar2_ptr, "bone_target", &tar_ptr, "bones", "", ICON_BONE_DATA);
  }
}

/* settings for 'location difference' driver variable type */
static void graph_panel_driverVar__locDiff(uiLayout *layout, ID *id, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];
  DriverTarget *dtar2 = &dvar->targets[1];
  Object *ob1 = (Object *)dtar->id;
  Object *ob2 = (Object *)dtar2->id;
  PointerRNA dtar_ptr, dtar2_ptr;
  uiLayout *col;

  /* initialize RNA pointer to the target */
  RNA_pointer_create(id, &RNA_DriverTarget, dtar, &dtar_ptr);
  RNA_pointer_create(id, &RNA_DriverTarget, dtar2, &dtar2_ptr);

  /* Object 1 */
  col = uiLayoutColumn(layout, true);
  uiLayoutSetRedAlert(col, (dtar->flag & DTAR_FLAG_INVALID)); /* XXX: per field... */
  uiItemR(col, &dtar_ptr, "id", 0, IFACE_("Object 1"), ICON_NONE);

  if (dtar->id && GS(dtar->id->name) == ID_OB && ob1->pose) {
    PointerRNA tar_ptr;

    RNA_pointer_create(dtar->id, &RNA_Pose, ob1->pose, &tar_ptr);
    uiItemPointerR(
        col, &dtar_ptr, "bone_target", &tar_ptr, "bones", IFACE_("Bone"), ICON_BONE_DATA);
  }

  /* we can clear it again now - it's only needed when creating the ID/Bone fields */
  uiLayoutSetRedAlert(col, false);

  uiItemR(col, &dtar_ptr, "transform_space", 0, NULL, ICON_NONE);

  /* Object 2 */
  col = uiLayoutColumn(layout, true);
  uiLayoutSetRedAlert(col, (dtar2->flag & DTAR_FLAG_INVALID)); /* XXX: per field... */
  uiItemR(col, &dtar2_ptr, "id", 0, IFACE_("Object 2"), ICON_NONE);

  if (dtar2->id && GS(dtar2->id->name) == ID_OB && ob2->pose) {
    PointerRNA tar_ptr;

    RNA_pointer_create(dtar2->id, &RNA_Pose, ob2->pose, &tar_ptr);
    uiItemPointerR(
        col, &dtar2_ptr, "bone_target", &tar_ptr, "bones", IFACE_("Bone"), ICON_BONE_DATA);
  }

  /* we can clear it again now - it's only needed when creating the ID/Bone fields */
  uiLayoutSetRedAlert(col, false);

  uiItemR(col, &dtar2_ptr, "transform_space", 0, NULL, ICON_NONE);
}

/* settings for 'transform channel' driver variable type */
static void graph_panel_driverVar__transChan(uiLayout *layout, ID *id, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];
  Object *ob = (Object *)dtar->id;
  PointerRNA dtar_ptr;
  uiLayout *col, *sub;

  /* initialize RNA pointer to the target */
  RNA_pointer_create(id, &RNA_DriverTarget, dtar, &dtar_ptr);

  /* properties */
  col = uiLayoutColumn(layout, true);
  uiLayoutSetRedAlert(col, (dtar->flag & DTAR_FLAG_INVALID)); /* XXX: per field... */
  uiItemR(col, &dtar_ptr, "id", 0, IFACE_("Object"), ICON_NONE);

  if (dtar->id && GS(dtar->id->name) == ID_OB && ob->pose) {
    PointerRNA tar_ptr;

    RNA_pointer_create(dtar->id, &RNA_Pose, ob->pose, &tar_ptr);
    uiItemPointerR(
        col, &dtar_ptr, "bone_target", &tar_ptr, "bones", IFACE_("Bone"), ICON_BONE_DATA);
  }

  sub = uiLayoutColumn(layout, true);
  uiItemR(sub, &dtar_ptr, "transform_type", 0, NULL, ICON_NONE);

  if (ELEM(dtar->transChan,
           DTAR_TRANSCHAN_ROTX,
           DTAR_TRANSCHAN_ROTY,
           DTAR_TRANSCHAN_ROTZ,
           DTAR_TRANSCHAN_ROTW)) {
    uiItemR(sub, &dtar_ptr, "rotation_mode", 0, IFACE_("Mode"), ICON_NONE);
  }

  uiItemR(sub, &dtar_ptr, "transform_space", 0, IFACE_("Space"), ICON_NONE);
}

/* ----------------------------------------------------------------- */

/* property driven by the driver - duplicates Active FCurve, but useful for clarity */
static void graph_draw_driven_property_panel(uiLayout *layout, ID *id, FCurve *fcu)
{
  PointerRNA fcu_ptr;
  uiLayout *row;
  char name[256];
  int icon = 0;

  /* F-Curve pointer */
  RNA_pointer_create(id, &RNA_FCurve, fcu, &fcu_ptr);

  /* get user-friendly 'name' for F-Curve */
  icon = getname_anim_fcurve(name, id, fcu);

  /* panel layout... */
  row = uiLayoutRow(layout, true);
  uiLayoutSetAlignment(row, UI_LAYOUT_ALIGN_LEFT);

  /* -> user friendly 'name' for datablock that owns F-Curve */
  /* XXX: Actually, we may need the datablock icons only...
   * (e.g. right now will show bone for bone props). */
  uiItemL(row, id->name + 2, icon);

  /* -> user friendly 'name' for F-Curve/driver target */
  uiItemL(row, "", ICON_RIGHTARROW);
  uiItemL(row, name, ICON_RNA);
}

/* UI properties panel layout for driver settings - shared for Drivers Editor and for */
static void graph_draw_driver_settings_panel(uiLayout *layout,
                                             ID *id,
                                             FCurve *fcu,
                                             const bool is_popover)
{
  ChannelDriver *driver = fcu->driver;
  DriverVar *dvar;

  PointerRNA driver_ptr;
  uiLayout *col, *row, *row_outer;
  uiBlock *block;
  uiBut *but;

  /* set event handler for panel */
  block = uiLayoutGetBlock(layout);
  UI_block_func_handle_set(block, do_graph_region_driver_buttons, id);

  /* driver-level settings - type, expressions, and errors */
  RNA_pointer_create(id, &RNA_Driver, driver, &driver_ptr);

  col = uiLayoutColumn(layout, true);
  block = uiLayoutGetBlock(col);
  uiItemR(col, &driver_ptr, "type", 0, NULL, ICON_NONE);

  {
    char valBuf[32];

    /* value of driver */
    row = uiLayoutRow(col, true);
    uiItemL(row, IFACE_("Driver Value:"), ICON_NONE);
    BLI_snprintf(valBuf, sizeof(valBuf), "%.3f", driver->curval);
    uiItemL(row, valBuf, ICON_NONE);
  }

  uiItemS(layout);
  uiItemS(layout);

  /* show expression box if doing scripted drivers,
   * and/or error messages when invalid drivers exist */
  if (driver->type == DRIVER_TYPE_PYTHON) {
    bool bpy_data_expr_error = (strstr(driver->expression, "bpy.data.") != NULL);
    bool bpy_ctx_expr_error = (strstr(driver->expression, "bpy.context.") != NULL);

    /* expression */
    /* TODO: "Show syntax hints" button */
    col = uiLayoutColumn(layout, true);
    block = uiLayoutGetBlock(col);

    uiItemL(col, IFACE_("Expression:"), ICON_NONE);
    uiItemR(col, &driver_ptr, "expression", 0, "", ICON_NONE);
    uiItemR(col, &driver_ptr, "use_self", 0, NULL, ICON_NONE);

    /* errors? */
    col = uiLayoutColumn(layout, true);
    block = uiLayoutGetBlock(col);

    if (driver->flag & DRIVER_FLAG_INVALID) {
      uiItemL(col, TIP_("ERROR: Invalid Python expression"), ICON_CANCEL);
    }
    else if (!BKE_driver_has_simple_expression(driver)) {
      if ((G.f & G_FLAG_SCRIPT_AUTOEXEC) == 0) {
        /* TODO: Add button to enable? */
        uiItemL(col, TIP_("Python restricted for security"), ICON_ERROR);
      }
      else {
        uiItemL(col, TIP_("Slow Python expression"), ICON_INFO);
      }
    }

    /* Explicit bpy-references are evil. Warn about these to prevent errors */
    /* TODO: put these in a box? */
    if (bpy_data_expr_error || bpy_ctx_expr_error) {
      uiItemL(col, TIP_("WARNING: Driver expression may not work correctly"), ICON_HELP);

      if (bpy_data_expr_error) {
        uiItemL(col, TIP_("TIP: Use variables instead of bpy.data paths (see below)"), ICON_ERROR);
      }
      if (bpy_ctx_expr_error) {
        uiItemL(col, TIP_("TIP: bpy.context is not safe for renderfarm usage"), ICON_ERROR);
      }
    }
  }
  else {
    /* errors? */
    col = uiLayoutColumn(layout, true);
    block = uiLayoutGetBlock(col);

    if (driver->flag & DRIVER_FLAG_INVALID) {
      uiItemL(col, TIP_("ERROR: Invalid target channel(s)"), ICON_ERROR);
    }

    /* Warnings about a lack of variables
     * NOTE: The lack of variables is generally a bad thing, since it indicates
     *       that the driver doesn't work at all. This particular scenario arises
     *       primarily when users mistakenly try to use drivers for procedural
     *       property animation
     */
    if (BLI_listbase_is_empty(&driver->variables)) {
      uiItemL(col, TIP_("ERROR: Driver is useless without any inputs"), ICON_ERROR);

      if (!BLI_listbase_is_empty(&fcu->modifiers)) {
        uiItemL(col, TIP_("TIP: Use F-Curves for procedural animation instead"), ICON_INFO);
        uiItemL(col, TIP_("F-Modifiers can generate curves for those too"), ICON_INFO);
      }
    }
  }

  uiItemS(layout);

  /* add/copy/paste driver variables */
  row_outer = uiLayoutRow(layout, false);

  /* add driver variable - add blank */
  row = uiLayoutRow(row_outer, true);
  block = uiLayoutGetBlock(row);
  but = uiDefIconTextBut(
      block,
      UI_BTYPE_BUT,
      B_IPO_DEPCHANGE,
      ICON_ADD,
      IFACE_("Add Input Variable"),
      0,
      0,
      10 * UI_UNIT_X,
      UI_UNIT_Y,
      NULL,
      0.0,
      0.0,
      0,
      0,
      TIP_("Add a Driver Variable to keep track of an input used by the driver"));
  UI_but_func_set(but, driver_add_var_cb, driver, NULL);

  if (is_popover) {
    /* add driver variable - add using eyedropper */
    /* XXX: will this operator work like this? */
    uiItemO(row, "", ICON_EYEDROPPER, "UI_OT_eyedropper_driver");
  }

  /* copy/paste (as sub-row) */
  row = uiLayoutRow(row_outer, true);
  block = uiLayoutGetBlock(row);

  uiItemO(row, "", ICON_COPYDOWN, "GRAPH_OT_driver_variables_copy");
  uiItemO(row, "", ICON_PASTEDOWN, "GRAPH_OT_driver_variables_paste");

  /* loop over targets, drawing them */
  for (dvar = driver->variables.first; dvar; dvar = dvar->next) {
    PointerRNA dvar_ptr;
    uiLayout *box;
    uiLayout *subrow, *sub;

    /* sub-layout column for this variable's settings */
    col = uiLayoutColumn(layout, true);

    /* 1) header panel */
    box = uiLayoutBox(col);
    RNA_pointer_create(id, &RNA_DriverVariable, dvar, &dvar_ptr);

    row = uiLayoutRow(box, false);
    block = uiLayoutGetBlock(row);

    /* 1.1) variable type and name */
    subrow = uiLayoutRow(row, true);

    /* 1.1.1) variable type */

    /* HACK: special group just for the enum,
     * otherwise we get ugly layout with text included too... */
    sub = uiLayoutRow(subrow, true);

    uiLayoutSetAlignment(sub, UI_LAYOUT_ALIGN_LEFT);

    uiItemR(sub, &dvar_ptr, "type", UI_ITEM_R_ICON_ONLY, "", ICON_NONE);

    /* 1.1.2) variable name */

    /* HACK: special group to counteract the effects of the previous enum,
     * which now pushes everything too far right */
    sub = uiLayoutRow(subrow, true);

    uiLayoutSetAlignment(sub, UI_LAYOUT_ALIGN_EXPAND);

    uiItemR(sub, &dvar_ptr, "name", 0, "", ICON_NONE);

    /* 1.2) invalid name? */
    UI_block_emboss_set(block, UI_EMBOSS_NONE);

    if (dvar->flag & DVAR_FLAG_INVALID_NAME) {
      but = uiDefIconBut(block,
                         UI_BTYPE_BUT,
                         B_IPO_DEPCHANGE,
                         ICON_ERROR,
                         290,
                         0,
                         UI_UNIT_X,
                         UI_UNIT_Y,
                         NULL,
                         0.0,
                         0.0,
                         0.0,
                         0.0,
                         TIP_("Invalid variable name, click here for details"));
      UI_but_func_set(but, driver_dvar_invalid_name_query_cb, dvar, NULL); /* XXX: reports? */
    }

    /* 1.3) remove button */
    but = uiDefIconBut(block,
                       UI_BTYPE_BUT,
                       B_IPO_DEPCHANGE,
                       ICON_X,
                       290,
                       0,
                       UI_UNIT_X,
                       UI_UNIT_Y,
                       NULL,
                       0.0,
                       0.0,
                       0.0,
                       0.0,
                       TIP_("Delete target variable"));
    UI_but_func_set(but, driver_delete_var_cb, driver, dvar);
    UI_block_emboss_set(block, UI_EMBOSS);

    /* 2) variable type settings */
    box = uiLayoutBox(col);
    /* controls to draw depends on the type of variable */
    switch (dvar->type) {
      case DVAR_TYPE_SINGLE_PROP: /* single property */
        graph_panel_driverVar__singleProp(box, id, dvar);
        break;
      case DVAR_TYPE_ROT_DIFF: /* rotational difference */
        graph_panel_driverVar__rotDiff(box, id, dvar);
        break;
      case DVAR_TYPE_LOC_DIFF: /* location difference */
        graph_panel_driverVar__locDiff(box, id, dvar);
        break;
      case DVAR_TYPE_TRANSFORM_CHAN: /* transform channel */
        graph_panel_driverVar__transChan(box, id, dvar);
        break;
    }

    /* 3) value of variable */
    {
      char valBuf[32];

      box = uiLayoutBox(col);
      row = uiLayoutRow(box, true);
      uiItemL(row, IFACE_("Value:"), ICON_NONE);

      if ((dvar->type == DVAR_TYPE_ROT_DIFF) ||
          (dvar->type == DVAR_TYPE_TRANSFORM_CHAN &&
           ELEM(dvar->targets[0].transChan,
                DTAR_TRANSCHAN_ROTX,
                DTAR_TRANSCHAN_ROTY,
                DTAR_TRANSCHAN_ROTZ,
                DTAR_TRANSCHAN_ROTW) &&
           dvar->targets[0].rotation_mode != DTAR_ROTMODE_QUATERNION)) {
        BLI_snprintf(
            valBuf, sizeof(valBuf), "%.3f (%4.1f°)", dvar->curval, RAD2DEGF(dvar->curval));
      }
      else {
        BLI_snprintf(valBuf, sizeof(valBuf), "%.3f", dvar->curval);
      }

      uiItemL(row, valBuf, ICON_NONE);
    }
  }

  uiItemS(layout);
  uiItemS(layout);

  /* XXX: This should become redundant. But sometimes the flushing fails,
   * so keep this around for a while longer as a "last resort" */
  row = uiLayoutRow(layout, true);
  block = uiLayoutGetBlock(row);
  but = uiDefIconTextBut(
      block,
      UI_BTYPE_BUT,
      B_IPO_DEPCHANGE,
      ICON_FILE_REFRESH,
      IFACE_("Update Dependencies"),
      0,
      0,
      10 * UI_UNIT_X,
      UI_UNIT_Y,
      NULL,
      0.0,
      0.0,
      0,
      0,
      TIP_("Force updates of dependencies - Only use this if drivers are not updating correctly"));
  UI_but_func_set(but, driver_update_flags_cb, fcu, NULL);
}

/* ----------------------------------------------------------------- */

/* Panel to show property driven by the driver (in Drivers Editor) - duplicates Active FCurve,
 * but useful for clarity. */
static void graph_panel_driven_property(const bContext *C, Panel *panel)
{
  bAnimListElem *ale;
  FCurve *fcu;

  if (!graph_panel_context(C, &ale, &fcu)) {
    return;
  }

  graph_draw_driven_property_panel(panel->layout, ale->id, fcu);

  MEM_freeN(ale);
}

/* driver settings for active F-Curve
 * (only for 'Drivers' mode in Graph Editor, i.e. the full "Drivers Editor") */
static void graph_panel_drivers(const bContext *C, Panel *panel)
{
  bAnimListElem *ale;
  FCurve *fcu;

  /* Get settings from context */
  if (!graph_panel_context(C, &ale, &fcu)) {
    return;
  }

  graph_draw_driver_settings_panel(panel->layout, ale->id, fcu, false);

  /* cleanup */
  MEM_freeN(ale);
}

/* ----------------------------------------------------------------- */

/* Poll to make this not show up in the graph editor,
 * as this is only to be used as a popup elsewhere. */
static bool graph_panel_drivers_popover_poll(const bContext *C, PanelType *UNUSED(pt))
{
  return ED_operator_graphedit_active((bContext *)C) == false;
}

/* popover panel for driver editing anywhere in ui */
static void graph_panel_drivers_popover(const bContext *C, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ptr = {NULL};
  PropertyRNA *prop = NULL;
  int index = -1;
  uiBut *but = NULL;

  /* Get active property to show driver properties for */
  but = UI_context_active_but_prop_get((bContext *)C, &ptr, &prop, &index);
  if (but) {
    FCurve *fcu;
    bool driven, special;

    fcu = BKE_fcurve_find_by_rna_context_ui(
        (bContext *)C, &ptr, prop, index, NULL, NULL, &driven, &special);

    /* Hack: Force all buttons in this panel to be able to know the driver button
     * this panel is getting spawned from, so that things like the "Open Drivers Editor"
     * button will work.
     */
    uiLayoutSetContextFromBut(layout, but);

    /* Populate Panel - With a combination of the contents of the Driven and Driver panels */
    if (fcu && fcu->driver) {
      ID *id = ptr.owner_id;

      PointerRNA ptr_fcurve;
      RNA_pointer_create(id, &RNA_FCurve, fcu, &ptr_fcurve);
      uiLayoutSetContextPointer(layout, "active_editable_fcurve", &ptr_fcurve);

      /* Driven Property Settings */
      uiItemL(layout, IFACE_("Driven Property:"), ICON_NONE);
      graph_draw_driven_property_panel(panel->layout, id, fcu);
      /* TODO: All vs Single */

      uiItemS(layout);
      uiItemS(layout);

      /* Drivers Settings */
      uiItemL(layout, IFACE_("Driver Settings:"), ICON_NONE);
      graph_draw_driver_settings_panel(panel->layout, id, fcu, true);
    }
  }

  /* Show drivers editor is always visible */
  uiItemO(layout, IFACE_("Show in Drivers Editor"), ICON_DRIVER, "SCREEN_OT_drivers_editor_show");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F-Curve Modifiers
 *
 * \note All the drawing code is in `editors/animation/fmodifier_ui.c`
 * \{ */

#define B_FMODIFIER_REDRAW 20
/** The start of FModifier panels registered for the graph editor. */
#define GRAPH_FMODIFIER_PANEL_PREFIX "GRAPH"

static void graph_fmodifier_panel_id(void *fcm_link, char *r_name)
{
  FModifier *fcm = (FModifier *)fcm_link;
  eFModifier_Types type = fcm->type;
  const FModifierTypeInfo *fmi = get_fmodifier_typeinfo(type);
  BLI_snprintf(r_name, BKE_ST_MAXNAME, "%s_PT_%s", GRAPH_FMODIFIER_PANEL_PREFIX, fmi->name);
}

static void do_graph_region_modifier_buttons(bContext *C, void *UNUSED(arg), int event)
{
  switch (event) {
    case B_FMODIFIER_REDRAW: /* XXX this should send depsgraph updates too */
      /* XXX: need a notifier specially for F-Modifiers */
      WM_event_add_notifier(C, NC_ANIMATION, NULL);
      break;
  }
}

static void graph_panel_modifiers(const bContext *C, Panel *panel)
{
  bAnimListElem *ale;
  FCurve *fcu;
  uiLayout *row;
  uiBlock *block;

  if (!graph_panel_context(C, &ale, &fcu)) {
    return;
  }

  block = uiLayoutGetBlock(panel->layout);
  UI_block_func_handle_set(block, do_graph_region_modifier_buttons, NULL);

  /* 'add modifier' button at top of panel */
  {
    row = uiLayoutRow(panel->layout, false);

    /* this is an operator button which calls a 'add modifier' operator...
     * a menu might be nicer but would be tricky as we need some custom filtering
     */
    uiItemMenuEnumO(
        row, (bContext *)C, "GRAPH_OT_fmodifier_add", "type", IFACE_("Add Modifier"), ICON_NONE);

    /* copy/paste (as sub-row) */
    row = uiLayoutRow(row, true);
    uiItemO(row, "", ICON_COPYDOWN, "GRAPH_OT_fmodifier_copy");
    uiItemO(row, "", ICON_PASTEDOWN, "GRAPH_OT_fmodifier_paste");
  }

  ANIM_fmodifier_panels(C, ale->fcurve_owner_id, &fcu->modifiers, graph_fmodifier_panel_id);

  MEM_freeN(ale);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void graph_buttons_register(ARegionType *art)
{
  PanelType *pt;

  pt = MEM_callocN(sizeof(PanelType), "spacetype graph panel properties");
  strcpy(pt->idname, "GRAPH_PT_properties");
  strcpy(pt->label, N_("Active F-Curve"));
  strcpy(pt->category, "F-Curve");
  strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_properties;
  pt->poll = graph_panel_poll;
  BLI_addtail(&art->paneltypes, pt);

  pt = MEM_callocN(sizeof(PanelType), "spacetype graph panel properties");
  strcpy(pt->idname, "GRAPH_PT_key_properties");
  strcpy(pt->label, N_("Active Keyframe"));
  strcpy(pt->category, "F-Curve");
  strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_key_properties;
  pt->poll = graph_panel_poll;
  BLI_addtail(&art->paneltypes, pt);

  pt = MEM_callocN(sizeof(PanelType), "spacetype graph panel drivers driven");
  strcpy(pt->idname, "GRAPH_PT_driven_property");
  strcpy(pt->label, N_("Driven Property"));
  strcpy(pt->category, "Drivers");
  strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_driven_property;
  pt->poll = graph_panel_drivers_poll;
  BLI_addtail(&art->paneltypes, pt);

  pt = MEM_callocN(sizeof(PanelType), "spacetype graph panel drivers");
  strcpy(pt->idname, "GRAPH_PT_drivers");
  strcpy(pt->label, N_("Driver"));
  strcpy(pt->category, "Drivers");
  strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_drivers;
  pt->poll = graph_panel_drivers_poll;
  BLI_addtail(&art->paneltypes, pt);

  pt = MEM_callocN(sizeof(PanelType), "spacetype graph panel drivers popover");
  strcpy(pt->idname, "GRAPH_PT_drivers_popover");
  strcpy(pt->label, N_("Add/Edit Driver"));
  strcpy(pt->category, "Drivers");
  strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_drivers_popover;
  pt->poll = graph_panel_drivers_popover_poll;
  BLI_addtail(&art->paneltypes, pt);
  /* This panel isn't used in this region.
   * Add explicitly to global list (so popovers work). */
  WM_paneltype_add(pt);

  pt = MEM_callocN(sizeof(PanelType), "spacetype graph panel modifiers");
  strcpy(pt->idname, "GRAPH_PT_modifiers");
  strcpy(pt->label, N_("Modifiers"));
  strcpy(pt->category, "Modifiers");
  strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->flag = PANEL_TYPE_NO_HEADER;
  pt->draw = graph_panel_modifiers;
  pt->poll = graph_panel_poll;
  BLI_addtail(&art->paneltypes, pt);

  ANIM_modifier_panels_register_graph_and_NLA(art, GRAPH_FMODIFIER_PANEL_PREFIX, graph_panel_poll);
  ANIM_modifier_panels_register_graph_only(art, GRAPH_FMODIFIER_PANEL_PREFIX, graph_panel_poll);

  pt = MEM_callocN(sizeof(PanelType), "spacetype graph panel view");
  strcpy(pt->idname, "GRAPH_PT_view");
  strcpy(pt->label, N_("Show Cursor"));
  strcpy(pt->category, "View");
  strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_cursor;
  pt->draw_header = graph_panel_cursor_header;
  BLI_addtail(&art->paneltypes, pt);
}

/** \} */

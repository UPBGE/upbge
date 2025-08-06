/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spgraph
 *
 * Graph editor space & buttons.
 */

#include <cfloat>
#include <cstring>

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_rotation.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_anim_data.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_fcurve.hh"
#include "BKE_fcurve_driver.h"
#include "BKE_global.hh"
#include "BKE_screen.hh"
#include "BKE_unit.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "ED_anim_api.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "graph_intern.hh" /* own include */

#define B_REDR 1

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

static bool graph_panel_context(const bContext *C, bAnimListElem **ale, FCurve **fcu)
{
  bAnimContext ac;
  bAnimListElem *elem = nullptr;

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
  if (elem == nullptr) {
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
  if (!graph_panel_context(C, nullptr, &fcu)) {
    return nullptr;
  }

  return fcu;
}

static bool graph_panel_poll(const bContext *C, PanelType * /*pt*/)
{
  return graph_panel_context(C, nullptr, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cursor Header
 * \{ */

static void graph_panel_cursor_header(const bContext *C, Panel *panel)
{
  bScreen *screen = CTX_wm_screen(C);
  SpaceGraph *sipo = CTX_wm_space_graph(C);
  uiLayout *col;

  /* get RNA pointers for use when creating the UI elements */
  PointerRNA spaceptr = RNA_pointer_create_discrete(&screen->id, &RNA_SpaceGraphEditor, sipo);

  /* 2D-Cursor */
  col = &panel->layout->column(false);
  col->prop(&spaceptr, "show_cursor", UI_ITEM_NONE, "", ICON_NONE);
}

static void graph_panel_cursor(const bContext *C, Panel *panel)
{
  bScreen *screen = CTX_wm_screen(C);
  SpaceGraph *sipo = CTX_wm_space_graph(C);
  Scene *scene = CTX_data_scene(C);
  uiLayout *layout = panel->layout;
  uiLayout *col, *sub;

  /* get RNA pointers for use when creating the UI elements */
  PointerRNA sceneptr = RNA_id_pointer_create(&scene->id);
  PointerRNA spaceptr = RNA_pointer_create_discrete(&screen->id, &RNA_SpaceGraphEditor, sipo);

  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  /* 2D-Cursor */
  col = &layout->column(false);
  col->active_set(RNA_boolean_get(&spaceptr, "show_cursor"));

  sub = &col->column(true);
  if (sipo->mode == SIPO_MODE_DRIVERS) {
    sub->prop(&spaceptr, "cursor_position_x", UI_ITEM_NONE, IFACE_("Cursor X"), ICON_NONE);
  }
  else {
    sub->prop(&sceneptr, "frame_current", UI_ITEM_NONE, IFACE_("Cursor X"), ICON_NONE);
  }

  sub->prop(&spaceptr, "cursor_position_y", UI_ITEM_NONE, IFACE_("Y"), ICON_NONE);

  sub = &col->column(true);
  sub->op("GRAPH_OT_frame_jump", IFACE_("Cursor to Selection"), ICON_NONE);
  sub->op("GRAPH_OT_snap_cursor_value", IFACE_("Cursor Value to Selection"), ICON_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Active F-Curve
 * \{ */

static void graph_panel_properties(const bContext *C, Panel *panel)
{
  bAnimListElem *ale;
  FCurve *fcu;
  uiLayout *layout = panel->layout;
  uiLayout *col;
  char name[256];
  int icon = 0;

  if (!graph_panel_context(C, &ale, &fcu)) {
    return;
  }

  /* F-Curve pointer */
  PointerRNA fcu_ptr = RNA_pointer_create_discrete(ale->fcurve_owner_id, &RNA_FCurve, fcu);

  /* user-friendly 'name' for F-Curve */
  col = &layout->column(false);
  if (ale->type == ANIMTYPE_FCURVE) {
    /* get user-friendly name for F-Curve */
    const std::optional<int> optional_icon = getname_anim_fcurve(name, ale->id, fcu);
    if (optional_icon) {
      icon = *optional_icon;
    }
    else if (ale->id) {
      icon = RNA_struct_ui_icon(ID_code_to_RNA_type(GS(ale->id->name)));
    }
    else {
      icon = ICON_NONE;
    }
  }
  else {
    /* NLA Control Curve, etc. */
    const bAnimChannelType *acf = ANIM_channel_get_typeinfo(ale);

    /* get name */
    if (acf && acf->name) {
      acf->name(ale, name);
    }
    else {
      STRNCPY_UTF8(name, IFACE_("<invalid>"));
      icon = ICON_ERROR;
    }

    /* icon */
    if (ale->type == ANIMTYPE_NLACURVE) {
      icon = ICON_NLA;
    }
  }
  col->label(name, icon);

  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  /* RNA-Path Editing - only really should be enabled when things aren't working */
  col = &layout->column(false);
  col->enabled_set((fcu->flag & FCURVE_DISABLED) != 0);
  col->prop(&fcu_ptr, "data_path", UI_ITEM_NONE, "", ICON_RNA);
  col->prop(&fcu_ptr, "array_index", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  /* color settings */
  col = &layout->column(true);
  col->prop(&fcu_ptr, "color_mode", UI_ITEM_NONE, IFACE_("Display Color"), ICON_NONE);

  if (fcu->color_mode == FCURVE_COLOR_CUSTOM) {
    col->prop(&fcu_ptr, "color", UI_ITEM_NONE, IFACE_("Color"), ICON_NONE);
  }

  /* smoothing setting */
  col = &layout->column(true);
  col->prop(&fcu_ptr, "auto_smoothing", UI_ITEM_NONE, IFACE_("Handle Smoothing"), ICON_NONE);

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
  *r_bezt = *r_prevbezt = nullptr;

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
static void graphedit_activekey_update_cb(bContext * /*C*/, void *fcu_ptr, void * /*bezt_ptr*/)
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
    BKE_nurb_bezt_handle_test(bezt, SELECT, NURB_HANDLE_TEST_EACH, false);
  }

  /* now call standard updates */
  graphedit_activekey_update_cb(C, fcu_ptr, bezt_ptr);
}

/* update callback for editing coordinates of right handle in active keyframe properties
 * NOTE: we cannot just do graphedit_activekey_handles_cb() due to "order of computation"
 *       weirdness (see calchandleNurb_intern() and #39911)
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

  block = layout->block();
  // UI_block_func_handle_set(block, do_graph_region_buttons, nullptr);
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  /* only show this info if there are keyframes to edit */
  if (get_active_fcurve_keyframe_edit(fcu, &bezt, &prevbezt)) {
    PointerRNA fcu_prop_ptr;
    PropertyRNA *fcu_prop = nullptr;
    uiBut *but;
    int unit = B_UNIT_NONE;

    /* RNA pointer to keyframe, to allow editing */
    PointerRNA bezt_ptr = RNA_pointer_create_discrete(ale->fcurve_owner_id, &RNA_Keyframe, bezt);

    /* get property that F-Curve affects, for some unit-conversion magic */
    PointerRNA id_ptr = RNA_id_pointer_create(ale->id);
    if (RNA_path_resolve_property(&id_ptr, fcu->rna_path, &fcu_prop_ptr, &fcu_prop)) {
      /* determine the unit for this property */
      unit = RNA_SUBTYPE_UNIT(RNA_property_subtype(fcu_prop));
    }

    /* interpolation */
    col = &layout->column(false);
    if (fcu->flag & FCURVE_DISCRETE_VALUES) {
      uiLayout *split = &col->split(0.33f, true);
      split->label(IFACE_("Interpolation:"), ICON_NONE);
      split->label(IFACE_("None for Enum/Boolean"), ICON_IPO_CONSTANT);
    }
    else {
      col->prop(&bezt_ptr, "interpolation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }

    /* easing type */
    if (bezt->ipo > BEZT_IPO_BEZ) {
      col->prop(&bezt_ptr, "easing", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }

    /* easing extra */
    switch (bezt->ipo) {
      case BEZT_IPO_BACK:
        col = &layout->column(true);
        col->prop(&bezt_ptr, "back", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        break;
      case BEZT_IPO_ELASTIC:
        col = &layout->column(true);
        col->prop(&bezt_ptr, "amplitude", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        col->prop(&bezt_ptr, "period", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        break;
      default:
        break;
    }

    /* numerical coordinate editing
     * - we use the button-versions of the calls so that we can attach special update handlers
     *   and unit conversion magic that cannot be achieved using a purely RNA-approach
     */
    col = &layout->column(true);
    /* keyframe itself */
    {
      uiItemL_respect_property_split(col, IFACE_("Key Frame"), ICON_NONE);
      but = uiDefButR(block,
                      ButType::Num,
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
                      std::nullopt);
      UI_but_func_set(but, graphedit_activekey_update_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Value"), ICON_NONE);
      but = uiDefButR(block,
                      ButType::Num,
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
                      std::nullopt);
      UI_but_func_set(but, graphedit_activekey_update_cb, fcu, bezt);
      UI_but_unit_type_set(but, unit);
    }

    /* previous handle - only if previous was Bezier interpolation */
    if ((prevbezt) && (prevbezt->ipo == BEZT_IPO_BEZ)) {

      col = &layout->column(true);
      uiItemL_respect_property_split(col, IFACE_("Left Handle Type"), ICON_NONE);
      but = uiDefButR(block,
                      ButType::Menu,
                      B_REDR,
                      std::nullopt,
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "handle_left_type",
                      0,
                      0,
                      0,
                      "Type of left handle");
      UI_but_func_set(but, graphedit_activekey_handles_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Frame"), ICON_NONE);
      but = uiDefButR(block,
                      ButType::Num,
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
                      std::nullopt);
      UI_but_func_set(but, graphedit_activekey_left_handle_coord_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Value"), ICON_NONE);
      but = uiDefButR(block,
                      ButType::Num,
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
                      std::nullopt);
      UI_but_func_set(but, graphedit_activekey_left_handle_coord_cb, fcu, bezt);
      UI_but_unit_type_set(but, unit);
    }

    /* next handle - only if current is Bezier interpolation */
    if (bezt->ipo == BEZT_IPO_BEZ) {
      /* NOTE: special update callbacks are needed on the coords here due to #39911 */

      col = &layout->column(true);
      uiItemL_respect_property_split(col, IFACE_("Right Handle Type"), ICON_NONE);
      but = uiDefButR(block,
                      ButType::Menu,
                      B_REDR,
                      std::nullopt,
                      0,
                      0,
                      but_max_width,
                      UI_UNIT_Y,
                      &bezt_ptr,
                      "handle_right_type",
                      0,
                      0,
                      0,
                      "Type of right handle");
      UI_but_func_set(but, graphedit_activekey_handles_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Frame"), ICON_NONE);
      but = uiDefButR(block,
                      ButType::Num,
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
                      std::nullopt);
      UI_but_func_set(but, graphedit_activekey_right_handle_coord_cb, fcu, bezt);

      uiItemL_respect_property_split(col, IFACE_("Value"), ICON_NONE);
      but = uiDefButR(block,
                      ButType::Num,
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
                      std::nullopt);
      UI_but_func_set(but, graphedit_activekey_right_handle_coord_cb, fcu, bezt);
      UI_but_unit_type_set(but, unit);
    }
  }
  else {
    if ((fcu->bezt == nullptr) && (fcu->modifiers.first)) {
      /* modifiers only - so no keyframes to be active */
      layout->label(RPT_("F-Curve only has F-Modifiers"), ICON_NONE);
      layout->label(RPT_("See Modifiers panel below"), ICON_INFO);
    }
    else if (fcu->fpt) {
      /* samples only */
      layout->label(RPT_("F-Curve does not have any keyframes as it only contains sampled points"),
                    ICON_NONE);
    }
    else {
      layout->label(RPT_("No active keyframe on F-Curve"), ICON_NONE);
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
/* Was not actually run ever (nullptr always passed as arg to this callback).
 * If needed again, will need to check how to pass both fcurve and ID... :/ */
#if 0
      /* force F-Curve & Driver to get re-evaluated (same as the old Update Dependencies) */
      FCurve *fcu = (FCurve *)fcu_v;
      ChannelDriver *driver = (fcu) ? fcu->driver : nullptr;

      /* clear invalid flags */
      if (fcu) {
        fcu->flag &= ~FCURVE_DISABLED;
        driver->flag &= ~DRIVER_FLAG_INVALID;
      }
#endif
      ID *id = static_cast<ID *>(id_v);
      AnimData *adt = BKE_animdata_from_id(id);

      /* Rebuild depsgraph for the new dependencies, and ensure evaluated copies get flushed. */
      DEG_relations_tag_update(bmain);
      DEG_id_tag_update_ex(bmain, id, ID_RECALC_SYNC_TO_EVAL);
      if (adt != nullptr) {
        if (adt->action != nullptr) {
          DEG_id_tag_update_ex(bmain, &adt->action->id, ID_RECALC_SYNC_TO_EVAL);
        }
        if (adt->tmpact != nullptr) {
          DEG_id_tag_update_ex(bmain, &adt->tmpact->id, ID_RECALC_SYNC_TO_EVAL);
        }
      }

      break;
    }
  }

  /* default for now */
  WM_event_add_notifier(C, NC_SCENE | ND_FRAME, scene); /* XXX could use better notifier */
}

/* callback to add a target variable to the active driver */
static void driver_add_var_cb(bContext *C, void *driver_v, void * /*arg*/)
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
static void driver_dvar_invalid_name_query_cb(bContext *C, void *dvar_v, void * /*arg*/)
{
  uiPopupMenu *pup = UI_popup_menu_begin(
      C, CTX_IFACE_(BLT_I18NCONTEXT_OPERATOR_DEFAULT, "Invalid Variable Name"), ICON_NONE);
  uiLayout *layout = UI_popup_menu_layout(pup);

  DriverVar *dvar = (DriverVar *)dvar_v;

  if (dvar->flag & DVAR_FLAG_INVALID_EMPTY) {
    layout->label(RPT_("It cannot be left blank"), ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_START_NUM) {
    layout->label(RPT_("It cannot start with a number"), ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_START_CHAR) {
    layout->label(RPT_("It cannot start with a special character,"
                       " including '$', '@', '!', '~', '+', '-', '_', '.', or ' '"),
                  ICON_NONE);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_HAS_SPACE) {
    layout->label(RPT_("It cannot contain spaces (e.g. 'a space')"), ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_HAS_DOT) {
    layout->label(RPT_("It cannot contain dots (e.g. 'a.dot')"), ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_HAS_SPECIAL) {
    layout->label(RPT_("It cannot contain special (non-alphabetical/numeric) characters"),
                  ICON_ERROR);
  }
  if (dvar->flag & DVAR_FLAG_INVALID_PY_KEYWORD) {
    layout->label(RPT_("It cannot be a reserved keyword in Python"), ICON_INFO);
  }

  UI_popup_menu_end(C, pup);
}

/* callback to reset the driver's flags */
static void driver_update_flags_cb(bContext * /*C*/, void *fcu_v, void * /*arg*/)
{
  FCurve *fcu = (FCurve *)fcu_v;
  ChannelDriver *driver = fcu->driver;

  /* clear invalid flags */
  fcu->flag &= ~FCURVE_DISABLED;
  driver->flag &= ~DRIVER_FLAG_INVALID;
}

/* drivers panel poll */
static bool graph_panel_drivers_poll(const bContext *C, PanelType * /*pt*/)
{
  SpaceGraph *sipo = CTX_wm_space_graph(C);

  if (sipo->mode != SIPO_MODE_DRIVERS) {
    return false;
  }

  return graph_panel_context(C, nullptr, nullptr);
}

static void graph_panel_driverVar_fallback(uiLayout *layout,
                                           const DriverTarget *dtar,
                                           PointerRNA *dtar_ptr)
{
  if (dtar->options & DTAR_OPTION_USE_FALLBACK) {
    uiLayout *row = &layout->row(true);
    row->prop(dtar_ptr, "use_fallback_value", UI_ITEM_NONE, "", ICON_NONE);
    row->prop(dtar_ptr, "fallback_value", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    layout->prop(dtar_ptr, "use_fallback_value", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

/* settings for 'single property' driver variable type */
static void graph_panel_driverVar__singleProp(uiLayout *layout, ID *id, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];
  uiLayout *row, *col;

  /* initialize RNA pointer to the target */
  PointerRNA dtar_ptr = RNA_pointer_create_discrete(id, &RNA_DriverTarget, dtar);

  /* Target ID */
  row = &layout->row(false);
  row->red_alert_set((dtar->flag & DTAR_FLAG_INVALID) && !dtar->id);
  uiTemplateAnyID(row, &dtar_ptr, "id", "id_type", IFACE_("Prop:"));

  /* Target Property */
  if (dtar->id) {
    /* get pointer for resolving the property selected */
    PointerRNA root_ptr = RNA_id_pointer_create(dtar->id);

    /* rna path */
    col = &layout->column(true);
    col->red_alert_set(dtar->flag & (DTAR_FLAG_INVALID | DTAR_FLAG_FALLBACK_USED));
    uiTemplatePathBuilder(col,
                          &dtar_ptr,
                          "data_path",
                          &root_ptr,
                          CTX_IFACE_(BLT_I18NCONTEXT_EDITOR_FILEBROWSER, "Path"));

    /* Default value. */
    graph_panel_driverVar_fallback(layout, dtar, &dtar_ptr);
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
  uiLayout *col;

  /* initialize RNA pointer to the target */
  PointerRNA dtar_ptr = RNA_pointer_create_discrete(id, &RNA_DriverTarget, dtar);
  PointerRNA dtar2_ptr = RNA_pointer_create_discrete(id, &RNA_DriverTarget, dtar2);

  /* Object 1 */
  col = &layout->column(true);
  col->red_alert_set(dtar->flag & DTAR_FLAG_INVALID); /* XXX: per field... */
  col->prop(&dtar_ptr, "id", UI_ITEM_NONE, IFACE_("Object 1"), ICON_NONE);

  if (dtar->id && GS(dtar->id->name) == ID_OB && ob1->pose) {
    PointerRNA tar_ptr = RNA_pointer_create_discrete(dtar->id, &RNA_Pose, ob1->pose);
    col->prop_search(&dtar_ptr, "bone_target", &tar_ptr, "bones", "", ICON_BONE_DATA);
  }

  /* Object 2 */
  col = &layout->column(true);
  col->red_alert_set(dtar2->flag & DTAR_FLAG_INVALID); /* XXX: per field... */
  col->prop(&dtar2_ptr, "id", UI_ITEM_NONE, IFACE_("Object 2"), ICON_NONE);

  if (dtar2->id && GS(dtar2->id->name) == ID_OB && ob2->pose) {
    PointerRNA tar_ptr = RNA_pointer_create_discrete(dtar2->id, &RNA_Pose, ob2->pose);
    col->prop_search(&dtar2_ptr, "bone_target", &tar_ptr, "bones", "", ICON_BONE_DATA);
  }
}

/* settings for 'location difference' driver variable type */
static void graph_panel_driverVar__locDiff(uiLayout *layout, ID *id, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];
  DriverTarget *dtar2 = &dvar->targets[1];
  Object *ob1 = (Object *)dtar->id;
  Object *ob2 = (Object *)dtar2->id;
  uiLayout *col;

  /* initialize RNA pointer to the target */
  PointerRNA dtar_ptr = RNA_pointer_create_discrete(id, &RNA_DriverTarget, dtar);
  PointerRNA dtar2_ptr = RNA_pointer_create_discrete(id, &RNA_DriverTarget, dtar2);

  /* Object 1 */
  col = &layout->column(true);
  col->red_alert_set(dtar->flag & DTAR_FLAG_INVALID); /* XXX: per field... */
  col->prop(&dtar_ptr, "id", UI_ITEM_NONE, IFACE_("Object 1"), ICON_NONE);

  if (dtar->id && GS(dtar->id->name) == ID_OB && ob1->pose) {
    PointerRNA tar_ptr = RNA_pointer_create_discrete(dtar->id, &RNA_Pose, ob1->pose);
    col->prop_search(&dtar_ptr, "bone_target", &tar_ptr, "bones", IFACE_("Bone"), ICON_BONE_DATA);
  }

  /* we can clear it again now - it's only needed when creating the ID/Bone fields */
  col->red_alert_set(false);

  col->prop(&dtar_ptr, "transform_space", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  /* Object 2 */
  col = &layout->column(true);
  col->red_alert_set(dtar2->flag & DTAR_FLAG_INVALID); /* XXX: per field... */
  col->prop(&dtar2_ptr, "id", UI_ITEM_NONE, IFACE_("Object 2"), ICON_NONE);

  if (dtar2->id && GS(dtar2->id->name) == ID_OB && ob2->pose) {
    PointerRNA tar_ptr = RNA_pointer_create_discrete(dtar2->id, &RNA_Pose, ob2->pose);
    col->prop_search(&dtar2_ptr, "bone_target", &tar_ptr, "bones", IFACE_("Bone"), ICON_BONE_DATA);
  }

  /* we can clear it again now - it's only needed when creating the ID/Bone fields */
  col->red_alert_set(false);

  col->prop(&dtar2_ptr, "transform_space", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

/* settings for 'transform channel' driver variable type */
static void graph_panel_driverVar__transChan(uiLayout *layout, ID *id, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];
  Object *ob = (Object *)dtar->id;
  uiLayout *col, *sub;

  /* initialize RNA pointer to the target */
  PointerRNA dtar_ptr = RNA_pointer_create_discrete(id, &RNA_DriverTarget, dtar);

  /* properties */
  col = &layout->column(true);
  col->red_alert_set(dtar->flag & DTAR_FLAG_INVALID); /* XXX: per field... */
  col->prop(&dtar_ptr, "id", UI_ITEM_NONE, IFACE_("Object"), ICON_NONE);

  if (dtar->id && GS(dtar->id->name) == ID_OB && ob->pose) {
    PointerRNA tar_ptr = RNA_pointer_create_discrete(dtar->id, &RNA_Pose, ob->pose);
    col->prop_search(&dtar_ptr, "bone_target", &tar_ptr, "bones", IFACE_("Bone"), ICON_BONE_DATA);
  }

  sub = &layout->column(true);
  sub->prop(&dtar_ptr, "transform_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (ELEM(dtar->transChan,
           DTAR_TRANSCHAN_ROTX,
           DTAR_TRANSCHAN_ROTY,
           DTAR_TRANSCHAN_ROTZ,
           DTAR_TRANSCHAN_ROTW))
  {
    sub->prop(&dtar_ptr, "rotation_mode", UI_ITEM_NONE, IFACE_("Mode"), ICON_NONE);
  }

  sub->prop(&dtar_ptr, "transform_space", UI_ITEM_NONE, IFACE_("Space"), ICON_NONE);
}

/* Settings for 'Context Property' driver variable type. */
static void graph_panel_driverVar__contextProp(uiLayout *layout, ID *id, DriverVar *dvar)
{
  DriverTarget *dtar = &dvar->targets[0];

  /* Initialize RNA pointer to the target. */
  PointerRNA dtar_ptr = RNA_pointer_create_discrete(id, &RNA_DriverTarget, dtar);

  /* Target Property. */
  {
    uiLayout *row = &layout->row(false);
    row->prop(&dtar_ptr, "context_property", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  /* Target Path */
  {
    uiLayout *col = &layout->column(true);
    col->red_alert_set(dtar->flag & (DTAR_FLAG_INVALID | DTAR_FLAG_FALLBACK_USED));
    uiTemplatePathBuilder(col,
                          &dtar_ptr,
                          "data_path",
                          nullptr,
                          CTX_IFACE_(BLT_I18NCONTEXT_EDITOR_FILEBROWSER, "Path"));
  }

  /* Default value. */
  graph_panel_driverVar_fallback(layout, dtar, &dtar_ptr);
}

/* ----------------------------------------------------------------- */

/* property driven by the driver - duplicates Active FCurve, but useful for clarity */

static void graph_draw_driven_property_enabled_btn(uiLayout *layout,
                                                   ID *id,
                                                   FCurve *fcu,
                                                   const char *label)
{
  PointerRNA fcurve_ptr = RNA_pointer_create_discrete(id, &RNA_FCurve, fcu);

  uiBlock *block = layout->block();
  uiDefButR(block,
            ButType::CheckboxN,
            0,
            label,
            0,
            0,
            UI_UNIT_X,
            UI_UNIT_Y,
            &fcurve_ptr,
            "mute",
            0,
            0,
            0,
            TIP_("Let the driver determine this property's value"));
}

static void graph_panel_drivers_header(const bContext *C, Panel *panel)
{
  bAnimListElem *ale;
  FCurve *fcu;
  if (!graph_panel_context(C, &ale, &fcu)) {
    return;
  }

  graph_draw_driven_property_enabled_btn(panel->layout, ale->id, fcu, IFACE_("Driver"));
  MEM_freeN(ale);
}

static void graph_draw_driven_property_panel(uiLayout *layout, ID *id, FCurve *fcu)
{
  uiLayout *row;
  char name[256];
  int icon = 0;

  /* get user-friendly 'name' for F-Curve */
  const std::optional<int> optional_icon = getname_anim_fcurve(name, id, fcu);
  if (optional_icon) {
    icon = *optional_icon;
  }
  else {
    icon = RNA_struct_ui_icon(ID_code_to_RNA_type(GS(id->name)));
  }

  /* panel layout... */
  row = &layout->row(true);
  row->alignment_set(blender::ui::LayoutAlign::Left);

  /* -> user friendly 'name' for datablock that owns F-Curve */
  /* XXX: Actually, we may need the datablock icons only...
   * (e.g. right now will show bone for bone props). */
  row->label(id->name + 2, icon);

  /* -> user friendly 'name' for F-Curve/driver target */
  row->label("", ICON_RIGHTARROW);
  row->label(name, ICON_RNA);
}

/* UI properties panel layout for driver settings - shared for Drivers Editor and for */
static void graph_draw_driver_settings_panel(uiLayout *layout,
                                             ID *id,
                                             FCurve *fcu,
                                             const bool is_popover)
{
  ChannelDriver *driver = fcu->driver;

  uiLayout *col, *row, *row_outer;
  uiBlock *block;
  uiBut *but;

  /* set event handler for panel */
  block = layout->block();
  UI_block_func_handle_set(block, do_graph_region_driver_buttons, id);

  /* driver-level settings - type, expressions, and errors */
  PointerRNA driver_ptr = RNA_pointer_create_discrete(id, &RNA_Driver, driver);

  col = &layout->column(true);
  block = col->block();
  col->prop(&driver_ptr, "type", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  {
    char valBuf[32];

    /* value of driver */
    row = &col->row(true);
    row->label(IFACE_("Driver Value:"), ICON_NONE);
    SNPRINTF_UTF8(valBuf, "%.3f", driver->curval);
    row->label(valBuf, ICON_NONE);
  }

  layout->separator();
  layout->separator();

  /* show expression box if doing scripted drivers,
   * and/or error messages when invalid drivers exist */
  if (driver->type == DRIVER_TYPE_PYTHON) {
    bool bpy_data_expr_error = (strstr(driver->expression, "bpy.data.") != nullptr);
    bool bpy_ctx_expr_error = (strstr(driver->expression, "bpy.context.") != nullptr);

    /* expression */
    /* TODO: "Show syntax hints" button */
    col = &layout->column(true);
    block = col->block();

    col->label(IFACE_("Expression:"), ICON_NONE);
    col->prop(&driver_ptr, "expression", UI_ITEM_NONE, "", ICON_NONE);
    col->prop(&driver_ptr, "use_self", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    /* errors? */
    col = &layout->column(true);
    block = col->block();

    if (driver->flag & DRIVER_FLAG_PYTHON_BLOCKED) {
      /* TODO: Add button to enable? */
      col->label(RPT_("Python restricted for security"), ICON_ERROR);
      col->label(RPT_("Slow Python expression"), ICON_INFO);
    }
    else if (driver->flag & DRIVER_FLAG_INVALID) {
      col->label(RPT_("ERROR: Invalid Python expression"), ICON_CANCEL);
    }
    else if (!BKE_driver_has_simple_expression(driver)) {
      col->label(RPT_("Slow Python expression"), ICON_INFO);
    }

    /* Explicit bpy-references are evil. Warn about these to prevent errors */
    /* TODO: put these in a box? */
    if (bpy_data_expr_error || bpy_ctx_expr_error) {
      col->label(RPT_("WARNING: Driver expression may not work correctly"), ICON_HELP);

      if (bpy_data_expr_error) {
        col->label(RPT_("TIP: Use variables instead of bpy.data paths (see below)"), ICON_ERROR);
      }
      if (bpy_ctx_expr_error) {
        col->label(RPT_("TIP: bpy.context is not safe for renderfarm usage"), ICON_ERROR);
      }
    }
  }
  else {
    /* errors? */
    col = &layout->column(true);
    block = col->block();

    if (driver->flag & DRIVER_FLAG_INVALID) {
      col->label(RPT_("ERROR: Invalid target channel(s)"), ICON_ERROR);
    }

    /* Warnings about a lack of variables
     * NOTE: The lack of variables is generally a bad thing, since it indicates
     *       that the driver doesn't work at all. This particular scenario arises
     *       primarily when users mistakenly try to use drivers for procedural
     *       property animation
     */
    if (BLI_listbase_is_empty(&driver->variables)) {
      col->label(RPT_("ERROR: Driver is useless without any inputs"), ICON_ERROR);

      if (!BLI_listbase_is_empty(&fcu->modifiers)) {
        col->label(RPT_("TIP: Use F-Curves for procedural animation instead"), ICON_INFO);
        col->label(RPT_("F-Modifiers can generate curves for those too"), ICON_INFO);
      }
    }
  }

  layout->separator();

  /* add/copy/paste driver variables */
  row_outer = &layout->row(false);

  /* add driver variable - add blank */
  row = &row_outer->row(true);
  block = row->block();
  but = uiDefIconTextBut(
      block,
      ButType::But,
      B_IPO_DEPCHANGE,
      ICON_ADD,
      IFACE_("Add Input Variable"),
      0,
      0,
      10 * UI_UNIT_X,
      UI_UNIT_Y,
      nullptr,
      TIP_("Add a Driver Variable to keep track of an input used by the driver"));
  UI_but_func_set(but, driver_add_var_cb, driver, nullptr);

  if (is_popover) {
    /* add driver variable - add using eyedropper */
    /* XXX: will this operator work like this? */
    row->op("UI_OT_eyedropper_driver", "", ICON_EYEDROPPER);
  }

  /* copy/paste (as sub-row) */
  row = &row_outer->row(true);
  block = row->block();

  row->op("GRAPH_OT_driver_variables_copy", "", ICON_COPYDOWN);
  row->op("GRAPH_OT_driver_variables_paste", "", ICON_PASTEDOWN);

  /* loop over targets, drawing them */
  LISTBASE_FOREACH (DriverVar *, dvar, &driver->variables) {
    uiLayout *box;
    uiLayout *subrow, *sub;

    /* sub-layout column for this variable's settings */
    col = &layout->column(true);

    /* 1) header panel */
    box = &col->box();
    PointerRNA dvar_ptr = RNA_pointer_create_discrete(id, &RNA_DriverVariable, dvar);

    row = &box->row(false);
    block = row->block();

    /* 1.1) variable type and name */
    subrow = &row->row(true);

    /* 1.1.1) variable type */

    /* HACK: special group just for the enum,
     * otherwise we get ugly layout with text included too... */
    sub = &subrow->row(true);

    sub->alignment_set(blender::ui::LayoutAlign::Left);

    sub->prop(&dvar_ptr, "type", UI_ITEM_R_ICON_ONLY, "", ICON_NONE);

    /* 1.1.2) variable name */

    /* HACK: special group to counteract the effects of the previous enum,
     * which now pushes everything too far right */
    sub = &subrow->row(true);

    sub->alignment_set(blender::ui::LayoutAlign::Expand);

    sub->prop(&dvar_ptr, "name", UI_ITEM_NONE, "", ICON_NONE);

    /* 1.2) invalid name? */
    UI_block_emboss_set(block, blender::ui::EmbossType::None);

    if (dvar->flag & DVAR_FLAG_INVALID_NAME) {
      but = uiDefIconBut(block,
                         ButType::But,
                         B_IPO_DEPCHANGE,
                         ICON_ERROR,
                         290,
                         0,
                         UI_UNIT_X,
                         UI_UNIT_Y,
                         nullptr,
                         0.0,
                         0.0,
                         TIP_("Invalid variable name, click here for details"));
      UI_but_func_set(but, driver_dvar_invalid_name_query_cb, dvar, nullptr); /* XXX: reports? */
    }

    /* 1.3) remove button */
    but = uiDefIconBut(block,
                       ButType::But,
                       B_IPO_DEPCHANGE,
                       ICON_X,
                       290,
                       0,
                       UI_UNIT_X,
                       UI_UNIT_Y,
                       nullptr,
                       0.0,
                       0.0,
                       TIP_("Delete target variable"));
    UI_but_func_set(but, driver_delete_var_cb, driver, dvar);
    UI_block_emboss_set(block, blender::ui::EmbossType::Emboss);

    /* 2) variable type settings */
    box = &col->box();
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
      case DVAR_TYPE_CONTEXT_PROP: /* context property */
        graph_panel_driverVar__contextProp(box, id, dvar);
        break;
    }

    /* 3) value of variable */
    {
      char valBuf[32];

      box = &col->box();
      row = &box->row(true);
      row->label(IFACE_("Value:"), ICON_NONE);

      if ((dvar->type == DVAR_TYPE_ROT_DIFF) ||
          (dvar->type == DVAR_TYPE_TRANSFORM_CHAN &&
           ELEM(dvar->targets[0].transChan,
                DTAR_TRANSCHAN_ROTX,
                DTAR_TRANSCHAN_ROTY,
                DTAR_TRANSCHAN_ROTZ,
                DTAR_TRANSCHAN_ROTW) &&
           dvar->targets[0].rotation_mode != DTAR_ROTMODE_QUATERNION))
      {
        SNPRINTF_UTF8(valBuf,
                      "%.3f (%4.1f" BLI_STR_UTF8_DEGREE_SIGN ")",
                      dvar->curval,
                      RAD2DEGF(dvar->curval));
      }
      else {
        SNPRINTF_UTF8(valBuf, "%.3f", dvar->curval);
      }

      row->label(valBuf, ICON_NONE);
    }
  }
  /* Quiet warning about old value being unused before re-assigned. */
  UNUSED_VARS(block);

  layout->separator();
  layout->separator();

  /* XXX: This should become redundant. But sometimes the flushing fails,
   * so keep this around for a while longer as a "last resort" */
  row = &layout->row(true);
  block = row->block();
  but = uiDefIconTextBut(
      block,
      ButType::But,
      B_IPO_DEPCHANGE,
      ICON_FILE_REFRESH,
      IFACE_("Update Dependencies"),
      0,
      0,
      10 * UI_UNIT_X,
      UI_UNIT_Y,
      nullptr,
      TIP_("Force updates of dependencies - Only use this if drivers are not updating correctly"));
  UI_but_func_set(but, driver_update_flags_cb, fcu, nullptr);
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
static bool graph_panel_drivers_popover_poll(const bContext *C, PanelType * /*pt*/)
{
  return ED_operator_graphedit_active((bContext *)C) == false;
}

/* popover panel for driver editing anywhere in ui */
static void graph_panel_drivers_popover(const bContext *C, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ptr = {};
  PropertyRNA *prop = nullptr;
  int index = -1;
  uiBut *but = nullptr;

  /* Get active property to show driver properties for */
  but = UI_region_active_but_prop_get(CTX_wm_region(C), &ptr, &prop, &index);
  if (but) {
    FCurve *fcu;
    bool driven, special;

    fcu = BKE_fcurve_find_by_rna_context_ui(
        (bContext *)C, &ptr, prop, index, nullptr, nullptr, &driven, &special);

    /* Hack: Force all buttons in this panel to be able to know the driver button
     * this panel is getting spawned from, so that things like the "Open Drivers Editor"
     * button will work.
     */
    layout->context_set_from_but(but);

    /* Populate Panel - With a combination of the contents of the Driven and Driver panels */
    if (fcu && fcu->driver) {
      ID *id = ptr.owner_id;

      PointerRNA ptr_fcurve = RNA_pointer_create_discrete(id, &RNA_FCurve, fcu);
      layout->context_ptr_set("active_editable_fcurve", &ptr_fcurve);

      /* Driven Property Settings */
      layout->label(IFACE_("Driven Property:"), ICON_NONE);
      graph_draw_driven_property_panel(panel->layout, id, fcu);
      /* TODO: All vs Single */

      layout->separator();
      layout->separator();

      /* Drivers Settings */
      graph_draw_driven_property_enabled_btn(panel->layout, id, fcu, IFACE_("Driver:"));
      graph_draw_driver_settings_panel(panel->layout, id, fcu, true);
    }
  }

  /* Show drivers editor is always visible */
  layout->op("SCREEN_OT_drivers_editor_show", IFACE_("Show in Drivers Editor"), ICON_DRIVER);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F-Curve Modifiers
 *
 * \note All the drawing code is in `editors/animation/fmodifier_ui.cc`.
 * \{ */

#define B_FMODIFIER_REDRAW 20
/** The start of FModifier panels registered for the graph editor. */
#define GRAPH_FMODIFIER_PANEL_PREFIX "GRAPH"

static void graph_fmodifier_panel_id(void *fcm_link, char *r_name)
{
  FModifier *fcm = (FModifier *)fcm_link;
  eFModifier_Types type = eFModifier_Types(fcm->type);
  const FModifierTypeInfo *fmi = get_fmodifier_typeinfo(type);
  BLI_snprintf_utf8(r_name, BKE_ST_MAXNAME, "%s_PT_%s", GRAPH_FMODIFIER_PANEL_PREFIX, fmi->name);
}

static void do_graph_region_modifier_buttons(bContext *C, void * /*arg*/, int event)
{
  switch (event) {
    case B_FMODIFIER_REDRAW: /* XXX this should send depsgraph updates too */
      /* XXX: need a notifier specially for F-Modifiers */
      WM_event_add_notifier(C, NC_ANIMATION, nullptr);
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

  block = panel->layout->block();
  UI_block_func_handle_set(block, do_graph_region_modifier_buttons, nullptr);

  /* 'add modifier' button at top of panel */
  {
    row = &panel->layout->row(false);

    /* this is an operator button which calls a 'add modifier' operator...
     * a menu might be nicer but would be tricky as we need some custom filtering
     */
    row->op_menu_enum(C, "GRAPH_OT_fmodifier_add", "type", IFACE_("Add Modifier"), ICON_NONE);

    /* copy/paste (as sub-row) */
    row = &row->row(true);
    row->op("GRAPH_OT_fmodifier_copy", "", ICON_COPYDOWN);
    row->op("GRAPH_OT_fmodifier_paste", "", ICON_PASTEDOWN);
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

  pt = MEM_callocN<PanelType>("spacetype graph panel properties");
  STRNCPY_UTF8(pt->idname, "GRAPH_PT_properties");
  STRNCPY_UTF8(pt->label, N_("Active F-Curve"));
  STRNCPY_UTF8(pt->category, "F-Curve");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_properties;
  pt->poll = graph_panel_poll;
  BLI_addtail(&art->paneltypes, pt);

  pt = MEM_callocN<PanelType>("spacetype graph panel properties");
  STRNCPY_UTF8(pt->idname, "GRAPH_PT_key_properties");
  STRNCPY_UTF8(pt->label, N_("Active Keyframe"));
  STRNCPY_UTF8(pt->category, "F-Curve");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_key_properties;
  pt->poll = graph_panel_poll;
  BLI_addtail(&art->paneltypes, pt);

  pt = MEM_callocN<PanelType>("spacetype graph panel drivers driven");
  STRNCPY_UTF8(pt->idname, "GRAPH_PT_driven_property");
  STRNCPY_UTF8(pt->label, N_("Driven Property"));
  STRNCPY_UTF8(pt->category, "Drivers");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_driven_property;
  pt->poll = graph_panel_drivers_poll;
  BLI_addtail(&art->paneltypes, pt);

  pt = MEM_callocN<PanelType>("spacetype graph panel drivers");
  STRNCPY_UTF8(pt->idname, "GRAPH_PT_drivers");
  STRNCPY_UTF8(pt->label, N_("Driver"));
  STRNCPY_UTF8(pt->category, "Drivers");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_drivers;
  pt->draw_header = graph_panel_drivers_header;
  pt->poll = graph_panel_drivers_poll;
  BLI_addtail(&art->paneltypes, pt);

  pt = MEM_callocN<PanelType>("spacetype graph panel drivers popover");
  STRNCPY_UTF8(pt->idname, "GRAPH_PT_drivers_popover");
  STRNCPY_UTF8(pt->label, N_("Add/Edit Driver"));
  STRNCPY_UTF8(pt->category, "Drivers");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_drivers_popover;
  pt->poll = graph_panel_drivers_popover_poll;
  BLI_addtail(&art->paneltypes, pt);
  /* This panel isn't used in this region.
   * Add explicitly to global list (so popovers work). */
  WM_paneltype_add(pt);

  pt = MEM_callocN<PanelType>("spacetype graph panel modifiers");
  STRNCPY_UTF8(pt->idname, "GRAPH_PT_modifiers");
  STRNCPY_UTF8(pt->label, N_("Modifiers"));
  STRNCPY_UTF8(pt->category, "Modifiers");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->flag = PANEL_TYPE_NO_HEADER;
  pt->draw = graph_panel_modifiers;
  pt->poll = graph_panel_poll;
  BLI_addtail(&art->paneltypes, pt);

  ANIM_modifier_panels_register_graph_and_NLA(art, GRAPH_FMODIFIER_PANEL_PREFIX, graph_panel_poll);
  ANIM_modifier_panels_register_graph_only(art, GRAPH_FMODIFIER_PANEL_PREFIX, graph_panel_poll);

  pt = MEM_callocN<PanelType>("spacetype graph panel view");
  STRNCPY_UTF8(pt->idname, "GRAPH_PT_view");
  STRNCPY_UTF8(pt->label, N_("Show Cursor"));
  STRNCPY_UTF8(pt->category, "View");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = graph_panel_cursor;
  pt->draw_header = graph_panel_cursor_header;
  BLI_addtail(&art->paneltypes, pt);
}

/** \} */

/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BKE_sca.hh"
#include "BKE_screen.hh"

#include "BLI_string_ref.hh"

#include "ED_fileselect.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "interface_intern.hh"
#include "interface_templates_intern.hh"

using blender::StringRefNull;

/* -------------------------------------------------------------------- */
/** \name Search Menu Helpers
 * \{ */

int template_search_textbut_width(PointerRNA *ptr, PropertyRNA *name_prop)
{
  char str[UI_MAX_DRAW_STR];
  int buf_len = 0;

  BLI_assert(RNA_property_type(name_prop) == PROP_STRING);

  const char *name = RNA_property_string_get_alloc(ptr, name_prop, str, sizeof(str), &buf_len);

  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  const int margin = UI_UNIT_X * 0.75f;
  const int estimated_width = UI_fontstyle_string_width(fstyle, name) + margin;

  if (name != str) {
    MEM_freeN((void *)name);
  }

  /* Clamp to some min/max width. */
  return std::clamp(
      estimated_width, TEMPLATE_SEARCH_TEXTBUT_MIN_WIDTH, TEMPLATE_SEARCH_TEXTBUT_MIN_WIDTH * 4);
}

int template_search_textbut_height()
{
  return TEMPLATE_SEARCH_TEXTBUT_HEIGHT;
}

/**
 * Add a block button for the search menu for templateID and templateSearch.
 */
void template_add_button_search_menu(const bContext *C,
                                     uiLayout *layout,
                                     uiBlock *block,
                                     PointerRNA *ptr,
                                     PropertyRNA *prop,
                                     uiBlockCreateFunc block_func,
                                     void *block_argN,
                                     const char *const tip,
                                     const bool use_previews,
                                     const bool editable,
                                     const bool live_icon,
                                     uiButArgNFree func_argN_free_fn,
                                     uiButArgNCopy func_argN_copy_fn)
{
  const PointerRNA active_ptr = RNA_property_pointer_get(ptr, prop);
  ID *id = (active_ptr.data && RNA_struct_is_ID(active_ptr.type)) ?
               static_cast<ID *>(active_ptr.data) :
               nullptr;
  const ID *idfrom = ptr->owner_id;
  const StructRNA *type = active_ptr.type ? active_ptr.type : RNA_property_pointer_type(ptr, prop);
  uiBut *but;

  if (use_previews) {
    ARegion *region = CTX_wm_region(C);
    /* Ugly tool header exception. */
    const bool use_big_size = (region->regiontype != RGN_TYPE_TOOL_HEADER);
    /* Ugly exception for screens here,
     * drawing their preview in icon size looks ugly/useless */
    const bool use_preview_icon = use_big_size || (id && (GS(id->name) != ID_SCR));
    const short width = UI_UNIT_X * (use_big_size ? 6 : 1.6f);
    const short height = UI_UNIT_Y * (use_big_size ? 6 : 1);
    uiLayout *col = nullptr;

    if (use_big_size) {
      /* Assume column layout here. To be more correct, we should check if the layout passed to
       * template_id is a column one, but this should work well in practice. */
      col = uiLayoutColumn(layout, true);
    }

    but = uiDefBlockButN(block,
                         block_func,
                         block_argN,
                         "",
                         0,
                         0,
                         width,
                         height,
                         tip,
                         func_argN_free_fn,
                         func_argN_copy_fn);
    if (use_preview_icon) {
      const int icon = id ? ui_id_icon_get(C, id, use_big_size) : RNA_struct_ui_icon(type);
      ui_def_but_icon(but, icon, UI_HAS_ICON | UI_BUT_ICON_PREVIEW);
    }
    else {
      ui_def_but_icon(but, RNA_struct_ui_icon(type), UI_HAS_ICON);
      UI_but_drawflag_enable(but, UI_BUT_ICON_LEFT);
    }

    if ((idfrom && !ID_IS_EDITABLE(idfrom)) || !editable) {
      UI_but_flag_enable(but, UI_BUT_DISABLED);
    }
    if (use_big_size) {
      uiLayoutRow(col ? col : layout, true);
    }
  }
  else {
    but = uiDefBlockButN(block,
                         block_func,
                         block_argN,
                         "",
                         0,
                         0,
                         UI_UNIT_X * 1.6,
                         UI_UNIT_Y,
                         tip,
                         func_argN_free_fn,
                         func_argN_copy_fn);

    if (live_icon) {
      const int icon = id ? ui_id_icon_get(C, id, false) : RNA_struct_ui_icon(type);
      ui_def_but_icon(but, icon, UI_HAS_ICON | UI_BUT_ICON_PREVIEW);
    }
    else {
      ui_def_but_icon(but, RNA_struct_ui_icon(type), UI_HAS_ICON);
    }
    if (id) {
      /* default dragging of icon for id browse buttons */
      UI_but_drag_set_id(but, id);
    }
    UI_but_drawflag_enable(but, UI_BUT_ICON_LEFT);

    if ((idfrom && !ID_IS_EDITABLE(idfrom)) || !editable) {
      UI_but_flag_enable(but, UI_BUT_DISABLED);
    }
  }
}

uiBlock *template_common_search_menu(const bContext *C,
                                     ARegion *region,
                                     uiButSearchUpdateFn search_update_fn,
                                     void *search_arg,
                                     uiButHandleFunc search_exec_fn,
                                     void *active_item,
                                     uiButSearchTooltipFn item_tooltip_fn,
                                     const int preview_rows,
                                     const int preview_cols,
                                     float scale)
{
  static char search[256];
  wmWindow *win = CTX_wm_window(C);
  uiBut *but;

  /* clear initial search string, then all items show */
  search[0] = 0;

  uiBlock *block = UI_block_begin(C, region, "_popup", UI_EMBOSS);
  UI_block_flag_enable(block, UI_BLOCK_LOOP | UI_BLOCK_SEARCH_MENU);
  UI_block_theme_style_set(block, UI_BLOCK_THEME_STYLE_POPUP);

  /* preview thumbnails */
  if (preview_rows > 0 && preview_cols > 0) {
    const int w = 4 * U.widget_unit * preview_cols * scale;
    const int h = 5 * U.widget_unit * preview_rows * scale;

    /* fake button, it holds space for search items */
    uiDefBut(block, UI_BTYPE_LABEL, 0, "", 10, 26, w, h, nullptr, 0, 0, nullptr);

    but = uiDefSearchBut(block, search, 0, ICON_VIEWZOOM, sizeof(search), 10, 0, w, UI_UNIT_Y, "");
    UI_but_search_preview_grid_size_set(but, preview_rows, preview_cols);
  }
  /* list view */
  else {
    const int searchbox_width = int(float(UI_searchbox_size_x()) * 1.4f);
    const int searchbox_height = UI_searchbox_size_y();

    /* fake button, it holds space for search items */
    uiDefBut(block,
             UI_BTYPE_LABEL,
             0,
             "",
             10,
             15,
             searchbox_width,
             searchbox_height,
             nullptr,
             0,
             0,
             nullptr);
    but = uiDefSearchBut(block,
                         search,
                         0,
                         ICON_VIEWZOOM,
                         sizeof(search),
                         10,
                         0,
                         searchbox_width,
                         UI_UNIT_Y - 1,
                         "");
  }
  UI_but_func_search_set(but,
                         ui_searchbox_create_generic,
                         search_update_fn,
                         search_arg,
                         false,
                         nullptr,
                         search_exec_fn,
                         active_item);
  UI_but_func_search_set_tooltip(but, item_tooltip_fn);

  UI_block_bounds_set_normal(block, 0.3f * U.widget_unit);
  UI_block_direction_set(block, UI_DIR_DOWN);

  /* give search-field focus */
  UI_but_focus_on_enter_event(win, but);
  /* this type of search menu requires undo */
  but->flag |= UI_BUT_UNDO;

  return block;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Header Template
 * \{ */

void uiTemplateHeader(uiLayout *layout, bContext *C)
{
  uiBlock *block = uiLayoutAbsoluteBlock(layout);
  ED_area_header_switchbutton(C, block, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name RNA Path Builder Template
 * \{ */

void uiTemplatePathBuilder(uiLayout *layout,
                           PointerRNA *ptr,
                           const StringRefNull propname,
                           PointerRNA * /*root_ptr*/,
                           const std::optional<StringRefNull> text)
{
  /* check that properties are valid */
  PropertyRNA *propPath = RNA_struct_find_property(ptr, propname.c_str());
  if (!propPath || RNA_property_type(propPath) != PROP_STRING) {
    RNA_warning(
        "path property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  /* Start drawing UI Elements using standard defines */
  uiLayout *row = uiLayoutRow(layout, true);

  /* Path (existing string) Widget */
  uiItemR(row, ptr, propname, UI_ITEM_NONE, text, ICON_RNA);

  /* TODO: attach something to this to make allow
   * searching of nested properties to 'build' the path */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node Socket Icon Template
 * \{ */

void uiTemplateNodeSocket(uiLayout *layout, bContext * /*C*/, const float color[4])
{
  uiBlock *block = uiLayoutGetBlock(layout);
  UI_block_align_begin(block);

  /* XXX using explicit socket colors is not quite ideal.
   * Eventually it should be possible to use theme colors for this purpose,
   * but this requires a better design for extendable color palettes in user preferences. */
  uiBut *but = uiDefBut(
      block, UI_BTYPE_NODE_SOCKET, 0, "", 0, 0, UI_UNIT_X, UI_UNIT_Y, nullptr, 0, 0, "");
  rgba_float_to_uchar(but->col, color);

  UI_block_align_end(block);
}

/* -------------------------------------------------------------------- */
/** \name FileSelectParams Path Button Template
 * \{ */

void uiTemplateFileSelectPath(uiLayout *layout, bContext *C, FileSelectParams *params)
{
  bScreen *screen = CTX_wm_screen(C);
  SpaceFile *sfile = CTX_wm_space_file(C);

  ED_file_path_button(screen, sfile, params, uiLayoutGetBlock(layout));
}

/** \} */

static void handle_layer_buttons(bContext *C, void *arg1, void *arg2)
{
  uiBut *but = static_cast<uiBut *>(arg1);
  const int cur = POINTER_AS_INT(arg2);
  wmWindow *win = CTX_wm_window(C);
  const bool shift = win->eventstate->modifier & KM_SHIFT;

  if (!shift) {
    const int tot = RNA_property_array_length(&but->rnapoin, but->rnaprop);

    /* Normally clicking only selects one layer */
    RNA_property_boolean_set_index(&but->rnapoin, but->rnaprop, cur, true);
    for (int i = 0; i < tot; i++) {
      if (i != cur) {
        RNA_property_boolean_set_index(&but->rnapoin, but->rnaprop, i, false);
      }
    }
  }

  /* view3d layer change should update depsgraph (invisible object changed maybe) */
  /* see `view3d_header.cc` */
}

void uiTemplateGameStates(uiLayout *layout,
                          PointerRNA *ptr,
                          const char *propname,
                          PointerRNA *used_ptr,
                          const char *used_propname,
                          int active_state)
{
  uiLayout *uRow, *uCol;
  PropertyRNA *prop, *used_prop = NULL;
  int groups, cols, states;
  int group, col, state, row;
  int cols_per_group = 5;
  Object *ob = (Object *)ptr->owner_id;

  prop = RNA_struct_find_property(ptr, propname);
  if (!prop) {
    RNA_warning("states property not found: %s.%s", RNA_struct_identifier(ptr->type), propname);
    return;
  }

  /* the number of states determines the way we group them
   *	- we want 2 rows only (for now)
   *	- the number of columns (cols) is the total number of buttons per row
   *	  the 'remainder' is added to this, as it will be ok to have first row slightly wider if need
   *be
   *	- for now, only split into groups if group will have at least 5 items
   */
  states = RNA_property_array_length(ptr, prop);
  cols = (states / 2) + (states % 2);
  groups = ((cols / 2) < cols_per_group) ? (1) : (cols / cols_per_group);

  if (used_ptr && used_propname) {
    used_prop = RNA_struct_find_property(used_ptr, used_propname);
    if (!used_prop) {
      RNA_warning("used layers property not found: %s.%s",
                  RNA_struct_identifier(ptr->type),
                  used_propname);
      return;
    }

    if (RNA_property_array_length(used_ptr, used_prop) < states)
      used_prop = NULL;
  }

  /* layers are laid out going across rows, with the columns being divided into groups */

  for (group = 0; group < groups; group++) {
    uCol = uiLayoutColumn(layout, true);

    for (row = 0; row < 2; row++) {
      uiBlock *block;
      uiBut *but;

      uRow = uiLayoutRow(uCol, true);
      block = uiLayoutGetBlock(uRow);
      state = groups * cols_per_group * row + cols_per_group * group;

      /* add layers as toggle buts */
      for (col = 0; (col < cols_per_group) && (state < states); col++, state++) {
        int icon = 0;
        int butlay = 1 << state;

        if (active_state & butlay)
          icon = ICON_LAYER_ACTIVE;
        else if (used_prop && RNA_property_boolean_get_index(used_ptr, used_prop, state))
          icon = ICON_LAYER_USED;

        but = uiDefIconButR_prop(block,
                                 UI_BTYPE_ICON_TOGGLE,
                                 0,
                                 icon,
                                 0,
                                 0,
                                 UI_UNIT_X / 2,
                                 UI_UNIT_Y / 2,
                                 ptr,
                                 prop,
                                 state,
                                 0,
                                 0,
                                 BKE_sca_get_name_state(ob, state));
        UI_but_func_set(but, handle_layer_buttons, but, POINTER_FROM_INT(state));
        but->type = UI_BTYPE_TOGGLE;
      }
    }
  }
}

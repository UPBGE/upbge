/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sptext
 */

#include <cstring>

#include "DNA_text_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"

#include "BKE_context.hh"
#include "BKE_lib_query.hh"
#include "BKE_lib_remap.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "BLO_read_write.hh"

#include "RNA_access.hh"
#include "RNA_path.hh"

#include "text_format.hh"
#include "text_intern.hh" /* Own include. */

/* ******************** default callbacks for text space ***************** */

static SpaceLink *text_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  ARegion *region;
  SpaceText *stext;

  stext = MEM_callocN<SpaceText>("inittext");
  stext->spacetype = SPACE_TEXT;

  stext->lheight = 12;
  stext->tabnumber = 4;
  stext->margin_column = 80;
  stext->showsyntax = true;
  stext->showlinenrs = true;
  stext->flags |= ST_FIND_WRAP;

  stext->runtime = MEM_new<SpaceText_Runtime>(__func__);

  /* Header. */
  region = BKE_area_region_new();

  BLI_addtail(&stext->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  /* Footer. */
  region = BKE_area_region_new();
  BLI_addtail(&stext->regionbase, region);
  region->regiontype = RGN_TYPE_FOOTER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_TOP : RGN_ALIGN_BOTTOM;

  /* Properties region. */
  region = BKE_area_region_new();

  BLI_addtail(&stext->regionbase, region);
  region->regiontype = RGN_TYPE_UI;
  region->alignment = RGN_ALIGN_RIGHT;
  region->flag = RGN_FLAG_HIDDEN;

  /* Main region. */
  region = BKE_area_region_new();

  BLI_addtail(&stext->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  return (SpaceLink *)stext;
}

/* Doesn't free the space-link itself. */
static void text_free(SpaceLink *sl)
{
  SpaceText *stext = (SpaceText *)sl;
  space_text_free_caches(stext);
  MEM_delete(stext->runtime);
  stext->text = nullptr;
}

/* Spacetype; init callback. */
static void text_init(wmWindowManager * /*wm*/, ScrArea * /*area*/) {}

static SpaceLink *text_duplicate(SpaceLink *sl)
{
  SpaceText *stextn = static_cast<SpaceText *>(MEM_dupallocN(sl));

  /* Add its own runtime data. */
  stextn->runtime = MEM_new<SpaceText_Runtime>(__func__);

  return (SpaceLink *)stextn;
}

static void text_listener(const wmSpaceTypeListenerParams *params)
{
  ScrArea *area = params->area;
  const wmNotifier *wmn = params->notifier;
  SpaceText *st = static_cast<SpaceText *>(area->spacedata.first);

  /* context changes. */
  switch (wmn->category) {
    case NC_TEXT:
      /* Check if active text was changed, no need to redraw if text isn't active
       * `reference == nullptr` means text was unlinked, should update anyway for this
       * case -- no way to know was text active before unlinking or not. */
      if (wmn->reference && wmn->reference != st->text) {
        break;
      }

      switch (wmn->data) {
        case ND_DISPLAY:
        case ND_CURSOR:
          ED_area_tag_redraw(area);
          break;
      }

      switch (wmn->action) {
        case NA_EDITED:
          if (st->text) {
            space_text_drawcache_tag_update(st, true);
            text_update_edited(st->text);
          }

          ED_area_tag_redraw(area);
          ATTR_FALLTHROUGH; /* Fall down to tag redraw. */
        case NA_ADDED:
        case NA_REMOVED:
        case NA_SELECTED:
          ED_area_tag_redraw(area);
          break;
      }

      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_TEXT) {
        ED_area_tag_redraw(area);
      }
      break;
  }
}

static void text_operatortypes()
{
  WM_operatortype_append(TEXT_OT_new);
  WM_operatortype_append(TEXT_OT_open);
  WM_operatortype_append(TEXT_OT_reload);
  WM_operatortype_append(TEXT_OT_unlink);
  WM_operatortype_append(TEXT_OT_save);
  WM_operatortype_append(TEXT_OT_save_as);
  WM_operatortype_append(TEXT_OT_make_internal);
  WM_operatortype_append(TEXT_OT_run_script);

  WM_operatortype_append(TEXT_OT_paste);
  WM_operatortype_append(TEXT_OT_copy);
  WM_operatortype_append(TEXT_OT_cut);
  WM_operatortype_append(TEXT_OT_duplicate_line);

  WM_operatortype_append(TEXT_OT_convert_whitespace);
  WM_operatortype_append(TEXT_OT_comment_toggle);
  WM_operatortype_append(TEXT_OT_unindent);
  WM_operatortype_append(TEXT_OT_indent);
  WM_operatortype_append(TEXT_OT_indent_or_autocomplete);

  WM_operatortype_append(TEXT_OT_select_line);
  WM_operatortype_append(TEXT_OT_select_all);
  WM_operatortype_append(TEXT_OT_select_word);

  WM_operatortype_append(TEXT_OT_move_lines);

  WM_operatortype_append(TEXT_OT_jump);
  WM_operatortype_append(TEXT_OT_move);
  WM_operatortype_append(TEXT_OT_move_select);
  WM_operatortype_append(TEXT_OT_delete);
  WM_operatortype_append(TEXT_OT_overwrite_toggle);

  WM_operatortype_append(TEXT_OT_selection_set);
  WM_operatortype_append(TEXT_OT_cursor_set);
  WM_operatortype_append(TEXT_OT_scroll);
  WM_operatortype_append(TEXT_OT_scroll_bar);
  WM_operatortype_append(TEXT_OT_line_number);

  WM_operatortype_append(TEXT_OT_line_break);
  WM_operatortype_append(TEXT_OT_insert);

  WM_operatortype_append(TEXT_OT_find);
  WM_operatortype_append(TEXT_OT_find_set_selected);
  WM_operatortype_append(TEXT_OT_replace);
  WM_operatortype_append(TEXT_OT_replace_set_selected);

  WM_operatortype_append(TEXT_OT_start_find);
  WM_operatortype_append(TEXT_OT_jump_to_file_at_point);

  WM_operatortype_append(TEXT_OT_to_3d_object);

  WM_operatortype_append(TEXT_OT_resolve_conflict);

  WM_operatortype_append(TEXT_OT_autocomplete);

  WM_operatortype_append(TEXT_OT_update_shader);
}

static void text_keymap(wmKeyConfig *keyconf)
{
  WM_keymap_ensure(keyconf, "Text Generic", SPACE_TEXT, RGN_TYPE_WINDOW);
  WM_keymap_ensure(keyconf, "Text", SPACE_TEXT, RGN_TYPE_WINDOW);
}

const char *text_context_dir[] = {"edit_text", nullptr};

static int /*eContextResult*/ text_context(const bContext *C,
                                           const char *member,
                                           bContextDataResult *result)
{
  SpaceText *st = CTX_wm_space_text(C);

  if (CTX_data_dir(member)) {
    CTX_data_dir_set(result, text_context_dir);
    return CTX_RESULT_OK;
  }
  if (CTX_data_equals(member, "edit_text")) {
    if (st->text != nullptr) {
      CTX_data_id_pointer_set(result, &st->text->id);
    }
    return CTX_RESULT_OK;
  }

  return CTX_RESULT_MEMBER_NOT_FOUND;
}

/********************* main region ********************/

/* Add handlers, stuff you only do once or on area/region changes. */
static void text_main_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;
  ListBase *lb;

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_STANDARD, region->winx, region->winy);

  /* Own keymap. */
  keymap = WM_keymap_ensure(wm->defaultconf, "Text Generic", SPACE_TEXT, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler_v2d_mask(&region->runtime->handlers, keymap);
  keymap = WM_keymap_ensure(wm->defaultconf, "Text", SPACE_TEXT, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler_v2d_mask(&region->runtime->handlers, keymap);

  /* Add drop boxes. */
  lb = WM_dropboxmap_find("Text", SPACE_TEXT, RGN_TYPE_WINDOW);

  WM_event_add_dropbox_handler(&region->runtime->handlers, lb);
}

static void text_main_region_draw(const bContext *C, ARegion *region)
{
  /* Draw entirely, view changes should be handled here. */
  SpaceText *st = CTX_wm_space_text(C);
  // View2D *v2d = &region->v2d;

  /* Clear and setup matrix. */
  UI_ThemeClearColor(TH_BACK);

  // UI_view2d_view_ortho(v2d);

  /* Data. */
  draw_text_main(st, region);

  /* Reset view matrix. */
  // UI_view2d_view_restore(C);

  /* Scroll-bars? */
}

static void text_cursor(wmWindow *win, ScrArea *area, ARegion *region)
{
  SpaceText *st = static_cast<SpaceText *>(area->spacedata.first);
  int wmcursor = WM_CURSOR_TEXT_EDIT;

  if (st->text && BLI_rcti_isect_pt(&st->runtime->scroll_region_handle,
                                    win->eventstate->xy[0] - region->winrct.xmin,
                                    st->runtime->scroll_region_handle.ymin))
  {
    wmcursor = WM_CURSOR_DEFAULT;
  }

  WM_cursor_set(win, wmcursor);
}

/* ************* dropboxes ************* */

static bool text_drop_path_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{
  if (drag->type == WM_DRAG_PATH) {
    const eFileSel_File_Types file_type = eFileSel_File_Types(WM_drag_get_path_file_type(drag));
    if (ELEM(file_type, FILE_TYPE_PYSCRIPT, FILE_TYPE_TEXT)) {
      return true;
    }
  }
  return false;
}

static void text_drop_path_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  /* Copy drag path to properties. */
  RNA_string_set(drop->ptr, "filepath", WM_drag_get_single_path(drag));
}

static bool text_drop_id_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{
  return (drag->type == WM_DRAG_ID);
}

static void text_drop_id_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID(drag, 0);

  /* Copy drag path to properties. */
  std::string text = RNA_path_full_ID_py(id);
  RNA_string_set(drop->ptr, "text", text.c_str());
}

static bool text_drop_string_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{
  return (drag->type == WM_DRAG_STRING);
}

static void text_drop_string_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  const std::string &str = WM_drag_get_string(drag);
  RNA_string_set(drop->ptr, "text", str.c_str());
}

/* This region dropbox definition. */
static void text_dropboxes()
{
  ListBase *lb = WM_dropboxmap_find("Text", SPACE_TEXT, RGN_TYPE_WINDOW);

  WM_dropbox_add(lb, "TEXT_OT_open", text_drop_path_poll, text_drop_path_copy, nullptr, nullptr);
  WM_dropbox_add(lb, "TEXT_OT_insert", text_drop_id_poll, text_drop_id_copy, nullptr, nullptr);
  WM_dropbox_add(
      lb, "TEXT_OT_insert", text_drop_string_poll, text_drop_string_copy, nullptr, nullptr);
}

/* ************* end drop *********** */

/****************** header region ******************/

/* Add handlers, stuff you only do once or on area/region changes. */
static void text_header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void text_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

/****************** properties region ******************/

/* Add handlers, stuff you only do once or on area/region changes. */
static void text_properties_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  region->v2d.scroll = V2D_SCROLL_RIGHT | V2D_SCROLL_VERTICAL_HIDE;
  ED_region_panels_init(wm, region);

  /* Own keymaps. */
  keymap = WM_keymap_ensure(wm->defaultconf, "Text Generic", SPACE_TEXT, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler_v2d_mask(&region->runtime->handlers, keymap);
}

static void text_properties_region_draw(const bContext *C, ARegion *region)
{
  ED_region_panels(C, region);
}

static void text_id_remap(ScrArea * /*area*/,
                          SpaceLink *slink,
                          const blender::bke::id::IDRemapper &mappings)
{
  SpaceText *stext = (SpaceText *)slink;
  mappings.apply(reinterpret_cast<ID **>(&stext->text), ID_REMAP_APPLY_ENSURE_REAL);
}

static void text_foreach_id(SpaceLink *space_link, LibraryForeachIDData *data)
{
  SpaceText *st = reinterpret_cast<SpaceText *>(space_link);
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(
      data, st->text, IDWALK_CB_USER_ONE | IDWALK_CB_DIRECT_WEAK_LINK);
}

static void text_space_blend_read_data(BlendDataReader * /*reader*/, SpaceLink *sl)
{
  SpaceText *st = (SpaceText *)sl;
  st->runtime = MEM_new<SpaceText_Runtime>(__func__);
}

static void text_space_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  BLO_write_struct(writer, SpaceText, sl);
}

/********************* registration ********************/

void ED_spacetype_text()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  st->spaceid = SPACE_TEXT;
  STRNCPY_UTF8(st->name, "Text");

  st->create = text_create;
  st->free = text_free;
  st->init = text_init;
  st->duplicate = text_duplicate;
  st->operatortypes = text_operatortypes;
  st->keymap = text_keymap;
  st->listener = text_listener;
  st->context = text_context;
  st->dropboxes = text_dropboxes;
  st->id_remap = text_id_remap;
  st->foreach_id = text_foreach_id;
  st->blend_read_data = text_space_blend_read_data;
  st->blend_read_after_liblink = nullptr;
  st->blend_write = text_space_blend_write;

  /* Regions: main window. */
  art = MEM_callocN<ARegionType>("spacetype text region");
  art->regionid = RGN_TYPE_WINDOW;
  art->init = text_main_region_init;
  art->draw = text_main_region_draw;
  art->cursor = text_cursor;
  art->event_cursor = true;

  BLI_addhead(&st->regiontypes, art);

  /* Regions: properties. */
  art = MEM_callocN<ARegionType>("spacetype text region");
  art->regionid = RGN_TYPE_UI;
  art->prefsizex = UI_COMPACT_PANEL_WIDTH;
  art->keymapflag = ED_KEYMAP_UI;

  art->init = text_properties_region_init;
  art->snap_size = ED_region_generic_panel_region_snap_size;
  art->draw = text_properties_region_draw;
  BLI_addhead(&st->regiontypes, art);

  /* Regions: header. */
  art = MEM_callocN<ARegionType>("spacetype text region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;

  art->init = text_header_region_init;
  art->draw = text_header_region_draw;
  BLI_addhead(&st->regiontypes, art);

  /* Regions: footer. */
  art = MEM_callocN<ARegionType>("spacetype text region");
  art->regionid = RGN_TYPE_FOOTER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_FOOTER;
  art->init = text_header_region_init;
  art->draw = text_header_region_draw;
  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));

  /* Register formatters.
   * The first registered formatter is default when there is no extension in the ID-name. */
  ED_text_format_register_py(); /* Keep first (default formatter). */
  ED_text_format_register_osl();
  ED_text_format_register_glsl();
  ED_text_format_register_pov();
  ED_text_format_register_pov_ini();
  ED_text_format_register_glsl();
}

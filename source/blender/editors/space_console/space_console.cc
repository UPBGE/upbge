/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spconsole
 */

#include <cstdio>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "RNA_access.hh"
#include "RNA_path.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "BLO_read_write.hh"

#include "console_intern.hh" /* own include */

/* ******************** default callbacks for console space ***************** */

static SpaceLink *console_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  ARegion *region;
  SpaceConsole *sconsole;

  sconsole = MEM_callocN<SpaceConsole>("initconsole");
  sconsole->spacetype = SPACE_CONSOLE;

  sconsole->lheight = 14;

  /* header */
  region = BKE_area_region_new();

  BLI_addtail(&sconsole->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  /* main region */
  region = BKE_area_region_new();

  BLI_addtail(&sconsole->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  /* keep in sync with info */
  region->v2d.scroll |= V2D_SCROLL_RIGHT | V2D_SCROLL_VERTICAL_HIDE;
  region->v2d.align |= V2D_ALIGN_NO_NEG_X | V2D_ALIGN_NO_NEG_Y; /* align bottom left */
  region->v2d.keepofs |= V2D_LOCKOFS_X;
  region->v2d.keepzoom = (V2D_LOCKZOOM_X | V2D_LOCKZOOM_Y | V2D_LIMITZOOM | V2D_KEEPASPECT);
  region->v2d.keeptot = V2D_KEEPTOT_BOUNDS;
  region->v2d.minzoom = region->v2d.maxzoom = 1.0f;

  /* for now, aspect ratio should be maintained, and zoom is clamped within sane default limits */
  // region->v2d.keepzoom = (V2D_KEEPASPECT|V2D_LIMITZOOM);

  return (SpaceLink *)sconsole;
}

/* Doesn't free the space-link itself. */
static void console_free(SpaceLink *sl)
{
  SpaceConsole *sc = (SpaceConsole *)sl;

  while (sc->scrollback.first) {
    console_scrollback_free(sc, static_cast<ConsoleLine *>(sc->scrollback.first));
  }

  while (sc->history.first) {
    console_history_free(sc, static_cast<ConsoleLine *>(sc->history.first));
  }
}

/* spacetype; init callback */
static void console_init(wmWindowManager * /*wm*/, ScrArea * /*area*/) {}

static SpaceLink *console_duplicate(SpaceLink *sl)
{
  SpaceConsole *sconsolen = static_cast<SpaceConsole *>(MEM_dupallocN(sl));

  /* clear or remove stuff from old */

  /* TODO: duplicate?, then we also need to duplicate the py namespace. */
  BLI_listbase_clear(&sconsolen->scrollback);
  BLI_listbase_clear(&sconsolen->history);

  return (SpaceLink *)sconsolen;
}

/* add handlers, stuff you only do once or on area/region changes */
static void console_main_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;
  ListBase *lb;

  const float prev_y_min = region->v2d.cur.ymin; /* so re-sizing keeps the cursor visible */

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_CUSTOM, region->winx, region->winy);

  /* always keep the bottom part of the view aligned, less annoying */
  if (prev_y_min != region->v2d.cur.ymin) {
    const float cur_y_range = BLI_rctf_size_y(&region->v2d.cur);
    region->v2d.cur.ymin = prev_y_min;
    region->v2d.cur.ymax = prev_y_min + cur_y_range;
  }

  /* own keymap */
  keymap = WM_keymap_ensure(wm->defaultconf, "Console", SPACE_CONSOLE, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler_v2d_mask(&region->runtime->handlers, keymap);

  /* Include after "Console" so cursor motion keys such as "Home" isn't overridden. */
  keymap = WM_keymap_ensure(wm->defaultconf, "View2D Buttons List", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);

  /* add drop boxes */
  lb = WM_dropboxmap_find("Console", SPACE_CONSOLE, RGN_TYPE_WINDOW);

  WM_event_add_dropbox_handler(&region->runtime->handlers, lb);
}

/* same as 'text_cursor' */
static void console_cursor(wmWindow *win, ScrArea * /*area*/, ARegion *region)
{
  int wmcursor = WM_CURSOR_TEXT_EDIT;
  const wmEvent *event = win->eventstate;
  if (UI_view2d_mouse_in_scrollers(region, &region->v2d, event->xy)) {
    wmcursor = WM_CURSOR_DEFAULT;
  }

  WM_cursor_set(win, wmcursor);
}

/* ************* dropboxes ************* */

static bool console_drop_id_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{
  return WM_drag_get_local_ID(drag, 0) != nullptr;
}

static void console_drop_id_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID(drag, 0);

  /* copy drag path to properties */
  std::string text = RNA_path_full_ID_py(id);
  RNA_string_set(drop->ptr, "text", text.c_str());
}

static bool console_drop_path_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{
  return (drag->type == WM_DRAG_PATH);
}

static void console_drop_path_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  char pathname[FILE_MAX + 2];
  SNPRINTF(pathname, "\"%s\"", WM_drag_get_single_path(drag));
  RNA_string_set(drop->ptr, "text", pathname);
}

static bool console_drop_string_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{
  return (drag->type == WM_DRAG_STRING);
}

static void console_drop_string_copy(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  /* NOTE(@ideasman42): Only a single line is supported, multiple lines could be supported
   * but this implies executing all lines except for the last. While we could consider that,
   * there are some security implications for this, so just drop one line for now. */
  std::string str = WM_drag_get_string_firstline(drag);
  RNA_string_set(drop->ptr, "text", str.c_str());
}

/* this region dropbox definition */
static void console_dropboxes()
{
  ListBase *lb = WM_dropboxmap_find("Console", SPACE_CONSOLE, RGN_TYPE_WINDOW);

  WM_dropbox_add(
      lb, "CONSOLE_OT_insert", console_drop_id_poll, console_drop_id_copy, nullptr, nullptr);
  WM_dropbox_add(
      lb, "CONSOLE_OT_insert", console_drop_path_poll, console_drop_path_copy, nullptr, nullptr);
  WM_dropbox_add(lb,
                 "CONSOLE_OT_insert",
                 console_drop_string_poll,
                 console_drop_string_copy,
                 nullptr,
                 nullptr);
}

/* ************* end drop *********** */

static void console_main_region_draw(const bContext *C, ARegion *region)
{
  /* draw entirely, view changes should be handled here */
  SpaceConsole *sc = CTX_wm_space_console(C);
  View2D *v2d = &region->v2d;

  if (BLI_listbase_is_empty(&sc->scrollback)) {
    WM_operator_name_call((bContext *)C,
                          "CONSOLE_OT_banner",
                          blender::wm::OpCallContext::ExecDefault,
                          nullptr,
                          nullptr);
  }

  /* clear and setup matrix */
  UI_ThemeClearColor(TH_BACK);

  /* Works best with no view2d matrix set. */
  UI_view2d_view_ortho(v2d);

  /* data... */

  console_history_verify(C); /* make sure we have some command line */
  console_textview_main(sc, region);

  /* reset view matrix */
  UI_view2d_view_restore(C);

  /* scrollers */
  UI_view2d_scrollers_draw(v2d, nullptr);
}

static void console_operatortypes()
{
  /* `console_ops.cc` */
  WM_operatortype_append(CONSOLE_OT_move);
  WM_operatortype_append(CONSOLE_OT_delete);
  WM_operatortype_append(CONSOLE_OT_insert);

  WM_operatortype_append(CONSOLE_OT_indent);
  WM_operatortype_append(CONSOLE_OT_indent_or_autocomplete);
  WM_operatortype_append(CONSOLE_OT_unindent);

  /* for use by python only */
  WM_operatortype_append(CONSOLE_OT_history_append);
  WM_operatortype_append(CONSOLE_OT_scrollback_append);

  WM_operatortype_append(CONSOLE_OT_clear);
  WM_operatortype_append(CONSOLE_OT_clear_line);
  WM_operatortype_append(CONSOLE_OT_history_cycle);
  WM_operatortype_append(CONSOLE_OT_copy);
  WM_operatortype_append(CONSOLE_OT_paste);
  WM_operatortype_append(CONSOLE_OT_select_set);
  WM_operatortype_append(CONSOLE_OT_select_all);
  WM_operatortype_append(CONSOLE_OT_select_word);
}

static void console_keymap(wmKeyConfig *keyconf)
{
  WM_keymap_ensure(keyconf, "Console", SPACE_CONSOLE, RGN_TYPE_WINDOW);
}

/****************** header region ******************/

/* add handlers, stuff you only do once or on area/region changes */
static void console_header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void console_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

static void console_main_region_listener(const wmRegionListenerParams *params)
{
  ScrArea *area = params->area;
  ARegion *region = params->region;
  const wmNotifier *wmn = params->notifier;

  /* context changes */
  switch (wmn->category) {
    case NC_SPACE: {
      if (wmn->data == ND_SPACE_CONSOLE) {
        if (wmn->action == NA_EDITED) {
          if ((wmn->reference && area) && (wmn->reference == area->spacedata.first)) {
            /* we've modified the geometry (font size), re-calculate rect */
            console_textview_update_rect(static_cast<SpaceConsole *>(wmn->reference), region);
            ED_region_tag_redraw(region);
          }
        }
        else {
          /* generic redraw request */
          ED_region_tag_redraw(region);
        }
      }
      break;
    }
  }
}

static void console_blend_read_data(BlendDataReader *reader, SpaceLink *sl)
{
  SpaceConsole *sconsole = (SpaceConsole *)sl;

  BLO_read_struct_list(reader, ConsoleLine, &sconsole->scrollback);
  BLO_read_struct_list(reader, ConsoleLine, &sconsole->history);

  /* Comma expressions, (e.g. expr1, expr2, expr3) evaluate each expression,
   * from left to right.  the right-most expression sets the result of the comma
   * expression as a whole. */
  LISTBASE_FOREACH_MUTABLE (ConsoleLine *, cl, &sconsole->history) {
    BLO_read_char_array(reader, size_t(cl->len) + 1, &cl->line);
    if (cl->line) {
      /* The allocated length is not written, so reset here. */
      cl->len_alloc = cl->len + 1;
    }
    else {
      BLI_remlink(&sconsole->history, cl);
      MEM_freeN(cl);
    }
  }
}

static void console_space_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  SpaceConsole *con = (SpaceConsole *)sl;

  LISTBASE_FOREACH (ConsoleLine *, cl, &con->history) {
    /* 'len_alloc' is invalid on write, set from 'len' on read */
    BLO_write_struct(writer, ConsoleLine, cl);
    BLO_write_char_array(writer, size_t(cl->len) + 1, cl->line);
  }
  BLO_write_struct(writer, SpaceConsole, sl);
}

void ED_spacetype_console()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  st->spaceid = SPACE_CONSOLE;
  STRNCPY_UTF8(st->name, "Console");

  st->create = console_create;
  st->free = console_free;
  st->init = console_init;
  st->duplicate = console_duplicate;
  st->operatortypes = console_operatortypes;
  st->keymap = console_keymap;
  st->dropboxes = console_dropboxes;
  st->blend_read_data = console_blend_read_data;
  st->blend_write = console_space_blend_write;

  /* regions: main window */
  art = MEM_callocN<ARegionType>("spacetype console region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;

  art->init = console_main_region_init;
  art->draw = console_main_region_draw;
  art->cursor = console_cursor;
  art->event_cursor = true;
  art->listener = console_main_region_listener;

  BLI_addhead(&st->regiontypes, art);

  /* regions: header */
  art = MEM_callocN<ARegionType>("spacetype console region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;

  art->init = console_header_region_init;
  art->draw = console_header_region_draw;

  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));
}

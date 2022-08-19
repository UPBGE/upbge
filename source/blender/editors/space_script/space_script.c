/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spscript
 */

#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_screen.h"

#include "ED_screen.h"
#include "ED_space_api.h"

#include "WM_api.h"

#include "UI_resources.h"
#include "UI_view2d.h"

#ifdef WITH_PYTHON
#endif

#include "script_intern.h" /* own include */

// static script_run_python(char *funcname, )

/* ******************** default callbacks for script space ***************** */

static SpaceLink *script_create(const ScrArea *UNUSED(area), const Scene *UNUSED(scene))
{
  ARegion *region;
  SpaceScript *sscript;

  sscript = MEM_callocN(sizeof(SpaceScript), "initscript");
  sscript->spacetype = SPACE_SCRIPT;

  /* header */
  region = MEM_callocN(sizeof(ARegion), "header for script");

  BLI_addtail(&sscript->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  /* main region */
  region = MEM_callocN(sizeof(ARegion), "main region for script");

  BLI_addtail(&sscript->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  /* channel list region XXX */

  return (SpaceLink *)sscript;
}

/* not spacelink itself */
static void script_free(SpaceLink *sl)
{
  SpaceScript *sscript = (SpaceScript *)sl;

#ifdef WITH_PYTHON
  /* Free buttons references. */
  if (sscript->but_refs) {
    sscript->but_refs = NULL;
  }
#endif
  sscript->script = NULL;
}

/* spacetype; init callback */
static void script_init(struct wmWindowManager *UNUSED(wm), ScrArea *UNUSED(area))
{
}

static SpaceLink *script_duplicate(SpaceLink *sl)
{
  SpaceScript *sscriptn = MEM_dupallocN(sl);

  /* clear or remove stuff from old */

  return (SpaceLink *)sscriptn;
}

/* add handlers, stuff you only do once or on area/region changes */
static void script_main_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_STANDARD, region->winx, region->winy);

  /* own keymap */
  keymap = WM_keymap_ensure(wm->defaultconf, "Script", SPACE_SCRIPT, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);
}

static void script_main_region_draw(const bContext *C, ARegion *region)
{
  /* draw entirely, view changes should be handled here */
  SpaceScript *sscript = (SpaceScript *)CTX_wm_space_data(C);
  View2D *v2d = &region->v2d;

  /* clear and setup matrix */
  UI_ThemeClearColor(TH_BACK);

  UI_view2d_view_ortho(v2d);

  /* data... */
  // BPY_script_exec(C, "/root/blender-svn/blender25/test.py", NULL);

#ifdef WITH_PYTHON
  if (sscript->script) {
    // BPY_run_script_space_draw(C, sscript);
  }
#else
  (void)sscript;
#endif

  /* reset view matrix */
  UI_view2d_view_restore(C);

  /* scrollers? */
}

/* add handlers, stuff you only do once or on area/region changes */
static void script_header_region_init(wmWindowManager *UNUSED(wm), ARegion *region)
{
  ED_region_header_init(region);
}

static void script_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

static void script_main_region_listener(const wmRegionListenerParams *UNUSED(params))
{
/* XXX: Todo, need the ScriptSpace accessible to get the python script to run. */
#if 0
  BPY_run_script_space_listener()
#endif
}

void ED_spacetype_script(void)
{
  SpaceType *st = MEM_callocN(sizeof(SpaceType), "spacetype script");
  ARegionType *art;

  st->spaceid = SPACE_SCRIPT;
  strncpy(st->name, "Script", BKE_ST_MAXNAME);

  st->create = script_create;
  st->free = script_free;
  st->init = script_init;
  st->duplicate = script_duplicate;
  st->operatortypes = script_operatortypes;
  st->keymap = script_keymap;

  /* regions: main window */
  art = MEM_callocN(sizeof(ARegionType), "spacetype script region");
  art->regionid = RGN_TYPE_WINDOW;
  art->init = script_main_region_init;
  art->draw = script_main_region_draw;
  art->listener = script_main_region_listener;
  /* XXX: Need to further test this ED_KEYMAP_UI is needed for button interaction. */
  art->keymapflag = ED_KEYMAP_VIEW2D | ED_KEYMAP_UI | ED_KEYMAP_FRAMES;

  BLI_addhead(&st->regiontypes, art);

  /* regions: header */
  art = MEM_callocN(sizeof(ARegionType), "spacetype script region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;

  art->init = script_header_region_init;
  art->draw = script_header_region_draw;

  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(st);
}

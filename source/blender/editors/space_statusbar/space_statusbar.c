/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spstatusbar
 */

#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"

#include "BKE_context.h"
#include "BKE_screen.h"

#include "ED_screen.h"
#include "ED_space_api.h"

#include "RNA_access.h"

#include "UI_interface.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_types.h"

/* ******************** default callbacks for statusbar space ******************** */

static SpaceLink *statusbar_create(const ScrArea *UNUSED(area), const Scene *UNUSED(scene))
{
  ARegion *region;
  SpaceStatusBar *sstatusbar;

  sstatusbar = MEM_callocN(sizeof(*sstatusbar), "init statusbar");
  sstatusbar->spacetype = SPACE_STATUSBAR;

  /* header region */
  region = MEM_callocN(sizeof(*region), "header for statusbar");
  BLI_addtail(&sstatusbar->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = RGN_ALIGN_NONE;

  return (SpaceLink *)sstatusbar;
}

/* not spacelink itself */
static void statusbar_free(SpaceLink *UNUSED(sl))
{
}

/* spacetype; init callback */
static void statusbar_init(struct wmWindowManager *UNUSED(wm), ScrArea *UNUSED(area))
{
}

static SpaceLink *statusbar_duplicate(SpaceLink *sl)
{
  SpaceStatusBar *sstatusbarn = MEM_dupallocN(sl);

  /* clear or remove stuff from old */

  return (SpaceLink *)sstatusbarn;
}

/* add handlers, stuff you only do once or on area/region changes */
static void statusbar_header_region_init(wmWindowManager *UNUSED(wm), ARegion *region)
{
  if (ELEM(RGN_ALIGN_ENUM_FROM_MASK(region->alignment), RGN_ALIGN_RIGHT)) {
    region->flag |= RGN_FLAG_DYNAMIC_SIZE;
  }
  ED_region_header_init(region);
}

static void statusbar_operatortypes(void)
{
}

static void statusbar_keymap(struct wmKeyConfig *UNUSED(keyconf))
{
}

static void statusbar_header_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  wmNotifier *wmn = params->notifier;

  /* context changes */
  switch (wmn->category) {
    case NC_SCREEN:
      if (ELEM(wmn->data, ND_LAYER, ND_ANIMPLAY)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_WM:
      if (wmn->data == ND_JOB) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SCENE:
      if (wmn->data == ND_RENDER_RESULT) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_INFO) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_ID:
      if (wmn->action == NA_RENAME) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

static void statusbar_header_region_message_subscribe(const wmRegionMessageSubscribeParams *params)
{
  struct wmMsgBus *mbus = params->message_bus;
  ARegion *region = params->region;

  wmMsgSubscribeValue msg_sub_value_region_tag_redraw = {
      .owner = region,
      .user_data = region,
      .notify = ED_region_do_msg_notify_tag_redraw,
  };

  WM_msg_subscribe_rna_anon_prop(mbus, Window, view_layer, &msg_sub_value_region_tag_redraw);
  WM_msg_subscribe_rna_anon_prop(mbus, ViewLayer, name, &msg_sub_value_region_tag_redraw);
}

void ED_spacetype_statusbar(void)
{
  SpaceType *st = MEM_callocN(sizeof(*st), "spacetype statusbar");
  ARegionType *art;

  st->spaceid = SPACE_STATUSBAR;
  strncpy(st->name, "Status Bar", BKE_ST_MAXNAME);

  st->create = statusbar_create;
  st->free = statusbar_free;
  st->init = statusbar_init;
  st->duplicate = statusbar_duplicate;
  st->operatortypes = statusbar_operatortypes;
  st->keymap = statusbar_keymap;

  /* regions: header window */
  art = MEM_callocN(sizeof(*art), "spacetype statusbar header region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = 0.8f * HEADERY;
  art->prefsizex = UI_UNIT_X * 5; /* Mainly to avoid glitches */
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;
  art->init = statusbar_header_region_init;
  art->layout = ED_region_header_layout;
  art->draw = ED_region_header_draw;
  art->listener = statusbar_header_region_listener;
  art->message_subscribe = statusbar_header_region_message_subscribe;
  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(st);
}

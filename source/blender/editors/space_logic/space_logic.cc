/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2009 Blender Foundation.
 * All rights reserved.
 *
 *
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/space_logic/space_logic.c
 *  \ingroup splogic
 */

#include <cstdio>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "BLO_read_write.hh"

#include "DNA_gpencil_legacy_types.h"

#include "BKE_context.hh"
#include "BKE_gpencil_legacy.h"
#include "BKE_lib_id.hh"
#include "BKE_lib_remap.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "logic_intern.hh"

#include "GPU_framebuffer.hh"

/* ******************** manage regions ********************* */

ARegion *logic_has_buttons_region(ScrArea *sa)
{
  ARegion *region, *regionnew;

  region = BKE_area_find_region_type(sa, RGN_TYPE_UI);
  if (region)
    return region;

  /* add subdiv level; after header */
  region = BKE_area_find_region_type(sa, RGN_TYPE_HEADER);

  /* is error! */
  if (region == nullptr)
    return nullptr;

  regionnew = MEM_cnew<ARegion>("buttons for image");

  BLI_insertlinkafter(&sa->regionbase, region, regionnew);
  regionnew->regiontype = RGN_TYPE_UI;
  regionnew->alignment = RGN_ALIGN_RIGHT;

  regionnew->flag = RGN_FLAG_HIDDEN;

  return regionnew;
}

/* ******************** default callbacks for logic space ***************** */

static SpaceLink *logic_new(const ScrArea *sa, const Scene */*scene*/)
{
  ARegion *region;
  SpaceLogic *slogic;

  slogic = MEM_cnew<SpaceLogic>("initlogic");
  slogic->spacetype = SPACE_LOGIC;

  /* default options */
  slogic->scaflag = ((BUTS_SENS_SEL | BUTS_SENS_ACT | BUTS_SENS_LINK) |
                     (BUTS_CONT_SEL | BUTS_CONT_ACT | BUTS_CONT_LINK) |
                     (BUTS_ACT_SEL | BUTS_ACT_ACT | BUTS_ACT_LINK) |
                     (BUTS_SENS_STATE | BUTS_ACT_STATE));

  /* header */
  region = BKE_area_region_new();

  BLI_addtail(&slogic->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = RGN_ALIGN_BOTTOM;

  /* buttons/list view */
  region = BKE_area_region_new();

  BLI_addtail(&slogic->regionbase, region);
  region->regiontype = RGN_TYPE_UI;
  region->alignment = RGN_ALIGN_RIGHT;

  /* main region */
  region = BKE_area_region_new();

  BLI_addtail(&slogic->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  region->v2d.tot.xmin = 0.0f;
  region->v2d.tot.ymax = 0.0f;
  region->v2d.tot.xmax = 1150.0f;
  region->v2d.tot.ymin = (1150.0f / (float)sa->winx) * (float)-sa->winy;

  region->v2d.cur = region->v2d.tot;

  region->v2d.min[0] = 1.0f;
  region->v2d.min[1] = 1.0f;

  region->v2d.max[0] = 32000.0f;
  region->v2d.max[1] = 32000.0f;

  region->v2d.minzoom = 0.5f;
  region->v2d.maxzoom = 1.5f;

  region->v2d.scroll = (V2D_SCROLL_RIGHT | V2D_SCROLL_BOTTOM);
  region->v2d.keepzoom = V2D_KEEPZOOM | V2D_LIMITZOOM | V2D_KEEPASPECT;
  region->v2d.keeptot = V2D_KEEPTOT_BOUNDS;
  region->v2d.align = V2D_ALIGN_NO_POS_Y | V2D_ALIGN_NO_NEG_X;
  region->v2d.keepofs = V2D_KEEPOFS_Y;

  return (SpaceLink *)slogic;
}

/* not spacelink itself */
static void logic_free(SpaceLink */*sl*/)
{
  //	Spacelogic *slogic= (SpaceLogic *) sl;

  //	if (slogic->gpd)
  // XXX		BKE_gpencil_free(slogic->gpd);
}

/* spacetype; init callback */
static void logic_init(struct wmWindowManager */*wm*/, ScrArea */*sa*/)
{
}

static SpaceLink *logic_duplicate(SpaceLink *sl)
{
  SpaceLogic *slogicn = (SpaceLogic *)MEM_dupallocN(sl);

  return (SpaceLink *)slogicn;
}

static void logic_operatortypes()
{
  WM_operatortype_append(LOGIC_OT_properties);
  WM_operatortype_append(LOGIC_OT_links_cut);
}

static void logic_keymap(struct wmKeyConfig */*keyconf*/)
{
}

static void logic_refresh(const bContext */*C*/, ScrArea */*sa*/)
{
  //	SpaceLogic *slogic= CTX_wm_space_logic(C);
  //	Object *obedit= CTX_data_edit_object(C);
}

static void logic_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  const wmNotifier *wmn = params->notifier;
  /* context changes */
  switch (wmn->category) {
    case NC_LOGIC:
      ED_region_tag_redraw(region);
      break;
    case NC_SCENE:
      switch (wmn->data) {
        case ND_FRAME:
          ED_region_tag_redraw(region);
          break;

        case ND_OB_ACTIVE:
          ED_region_tag_redraw(region);
          break;
      }
      break;
    case NC_OBJECT:
      break;
    case NC_ID:
      if (wmn->action == NA_RENAME)
        ED_region_tag_redraw(region);
      break;
  }
}

static int logic_context(const bContext */*C*/,
                         const char */*member*/,
                         bContextDataResult */*result*/)
{
  //	SpaceLogic *slogic= CTX_wm_space_logic(C);
  return 0;
}

/************************** main region ***************************/

/* add handlers, stuff you only do once or on area/region changes */
static void logic_main_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_CUSTOM, region->winx, region->winy);

  ///* own keymaps */
  keymap = WM_keymap_ensure(wm->defaultconf, "Logic Bricks Editor", SPACE_LOGIC, 0);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);
}

static void logic_main_region_draw(const bContext *C, ARegion *region)
{
  /* draw entirely, view changes should be handled here */
  //	SpaceLogic *slogic= CTX_wm_space_logic(C);
  View2D *v2d = &region->v2d;

  /* clear and setup matrix */
  UI_ThemeClearColor(TH_BACK);

  UI_view2d_view_ortho(v2d);

  logic_buttons((bContext *)C, region);

  /* reset view matrix */
  UI_view2d_view_restore(C);

  /* scrollers */
  UI_view2d_scrollers_draw(v2d, nullptr);
}

/* *********************** buttons region ************************ */

/* add handlers, stuff you only do once or on area/region changes */
static void logic_buttons_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  ED_region_panels_init(wm, region);

  keymap = WM_keymap_ensure(wm->defaultconf, "Logic Bricks Editor", SPACE_LOGIC, 0);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);
}

static void logic_buttons_region_draw(const bContext *C, ARegion *region)
{
  ED_region_panels(C, region);
}

/************************* header region **************************/

/* add handlers, stuff you only do once or on area/region changes */
static void logic_header_region_init(wmWindowManager */*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void logic_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

/**************************** spacetype *****************************/

static void logic_id_remap(ScrArea */*area*/,
                           SpaceLink *slink,
                           const blender::bke::id::IDRemapper &mappings)
{
  SpaceLogic *slog = (SpaceLogic *)slink;

  if (!mappings.contains_mappings_for_any(FILTER_ID_GD_LEGACY)) {
    return;
  }

  mappings.apply((ID **)&slog->gpd, ID_REMAP_APPLY_UPDATE_REFCOUNT);
}

static void logic_blend_read_data(BlendDataReader *reader, SpaceLink *sl)
{
  SpaceLogic *slogic = (SpaceLogic *)sl;

  /* XXX: this is new stuff, which shouldn't be directly linking to gpd... */
  if (slogic->gpd) {
    BLO_read_data_address(reader, &slogic->gpd);
    BKE_gpencil_blend_read_data(reader, slogic->gpd);
  }
}

//static void logic_blend_read_lib(BlendLibReader *reader, ID *parent_id, SpaceLink *sl)
//{
//  SpaceLogic *slogic = (SpaceLogic *)sl;
//  BLO_read_id_address(reader, parent_id, &slogic->gpd);
//}

static void logic_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  BLO_write_struct(writer, SpaceLogic, sl);
}

/* only called once, from space/spacetypes.c */
void ED_spacetype_logic()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  st->spaceid = SPACE_LOGIC;
  strncpy(st->name, "Logic", BKE_ST_MAXNAME);

  st->create = logic_new;
  st->free = logic_free;
  st->init = logic_init;
  st->duplicate = logic_duplicate;
  st->operatortypes = logic_operatortypes;
  st->keymap = logic_keymap;
  st->refresh = logic_refresh;
  st->context = logic_context;
  st->id_remap = logic_id_remap;

  st->blend_read_data = logic_blend_read_data;
  st->blend_read_after_liblink = nullptr/*logic_blend_read_lib*/;
  st->blend_write = logic_blend_write;

  /* regions: main window */
  art = MEM_cnew<ARegionType>("spacetype logic region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_FRAMES | ED_KEYMAP_VIEW2D;
  art->init = logic_main_region_init;
  art->draw = logic_main_region_draw;
  art->listener = logic_listener;

  BLI_addhead(&st->regiontypes, art);

  /* regions: listview/buttons */
  art = MEM_cnew<ARegionType>("spacetype logic region");
  art->regionid = RGN_TYPE_UI;
  art->prefsizex = 220;  // XXX
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_FRAMES;
  art->listener = logic_listener;
  art->init = logic_buttons_region_init;
  art->draw = logic_buttons_region_draw;
  BLI_addhead(&st->regiontypes, art);

  /* regions: header */
  art = MEM_cnew<ARegionType>("spacetype logic region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_FRAMES | ED_KEYMAP_HEADER;
  art->init = logic_header_region_init;
  art->draw = logic_header_region_draw;

  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));
}

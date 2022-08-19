/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spoutliner
 */

#include <cstdio>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_mempool.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_lib_remap.h"
#include "BKE_outliner_treehash.hh"
#include "BKE_screen.h"

#include "ED_screen.h"
#include "ED_space_api.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_types.h"

#include "RNA_access.h"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "UI_resources.h"
#include "UI_view2d.h"

#include "outliner_intern.hh"
#include "tree/tree_display.hh"

SpaceOutliner_Runtime::SpaceOutliner_Runtime(const SpaceOutliner_Runtime & /*other*/)
    : tree_display(nullptr), tree_hash(nullptr)
{
}

static void outliner_main_region_init(wmWindowManager *wm, ARegion *region)
{
  ListBase *lb;
  wmKeyMap *keymap;

  /* make sure we keep the hide flags */
  region->v2d.scroll |= (V2D_SCROLL_RIGHT | V2D_SCROLL_BOTTOM);
  region->v2d.scroll &= ~(V2D_SCROLL_LEFT | V2D_SCROLL_TOP); /* prevent any noise of past */
  region->v2d.scroll |= V2D_SCROLL_HORIZONTAL_HIDE;
  region->v2d.scroll |= V2D_SCROLL_VERTICAL_HIDE;

  region->v2d.align = (V2D_ALIGN_NO_NEG_X | V2D_ALIGN_NO_POS_Y);
  region->v2d.keepzoom = (V2D_LOCKZOOM_X | V2D_LOCKZOOM_Y | V2D_LIMITZOOM | V2D_KEEPASPECT);
  region->v2d.keeptot = V2D_KEEPTOT_STRICT;
  region->v2d.minzoom = region->v2d.maxzoom = 1.0f;

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_LIST, region->winx, region->winy);

  /* own keymap */
  keymap = WM_keymap_ensure(wm->defaultconf, "Outliner", SPACE_OUTLINER, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);

  /* Add dropboxes */
  lb = WM_dropboxmap_find("Outliner", SPACE_OUTLINER, RGN_TYPE_WINDOW);
  WM_event_add_dropbox_handler(&region->handlers, lb);
}

static void outliner_main_region_draw(const bContext *C, ARegion *region)
{
  View2D *v2d = &region->v2d;

  /* clear */
  UI_ThemeClearColor(TH_BACK);

  draw_outliner(C);

  /* reset view matrix */
  UI_view2d_view_restore(C);

  /* scrollers */
  UI_view2d_scrollers_draw(v2d, nullptr);
}

static void outliner_main_region_free(ARegion *UNUSED(region))
{
}

static void outliner_main_region_listener(const wmRegionListenerParams *params)
{
  ScrArea *area = params->area;
  ARegion *region = params->region;
  wmNotifier *wmn = params->notifier;
  SpaceOutliner *space_outliner = static_cast<SpaceOutliner *>(area->spacedata.first);

  /* context changes */
  switch (wmn->category) {
    case NC_WM:
      switch (wmn->data) {
        case ND_LIB_OVERRIDE_CHANGED:
          ED_region_tag_redraw(region);
          break;
      }
      break;
    case NC_SCENE:
      switch (wmn->data) {
        case ND_OB_ACTIVE:
        case ND_OB_SELECT:
          if (outliner_requires_rebuild_on_select_or_active_change(space_outliner)) {
            ED_region_tag_redraw(region);
          }
          else {
            ED_region_tag_redraw_no_rebuild(region);
          }
          break;
        case ND_OB_VISIBLE:
        case ND_OB_RENDER:
        case ND_MODE:
        case ND_KEYINGSET:
        case ND_FRAME:
        case ND_RENDER_OPTIONS:
        case ND_SEQUENCER:
        case ND_LAYER_CONTENT:
        case ND_WORLD:
        case ND_SCENEBROWSE:
          ED_region_tag_redraw(region);
          break;
        case ND_LAYER:
          /* Avoid rebuild if only the active collection changes */
          if ((wmn->subtype == NS_LAYER_COLLECTION) && (wmn->action == NA_ACTIVATED)) {
            ED_region_tag_redraw_no_rebuild(region);
            break;
          }

          ED_region_tag_redraw(region);
          break;
      }
      if (wmn->action == NA_EDITED) {
        ED_region_tag_redraw_no_rebuild(region);
      }
      break;
    case NC_OBJECT:
      switch (wmn->data) {
        case ND_TRANSFORM:
          ED_region_tag_redraw_no_rebuild(region);
          break;
        case ND_BONE_ACTIVE:
        case ND_BONE_SELECT:
        case ND_DRAW:
        case ND_PARENT:
        case ND_OB_SHADING:
          ED_region_tag_redraw(region);
          break;
        case ND_CONSTRAINT:
          /* all constraint actions now, for reordering */
          ED_region_tag_redraw(region);
          break;
        case ND_MODIFIER:
          /* all modifier actions now */
          ED_region_tag_redraw(region);
          break;
        default:
          /* Trigger update for NC_OBJECT itself */
          ED_region_tag_redraw(region);
          break;
      }
      break;
    case NC_GROUP:
      /* All actions now, TODO: check outliner view mode? */
      ED_region_tag_redraw(region);
      break;
    case NC_LAMP:
      /* For updating light icons, when changing light type */
      if (wmn->data == ND_LIGHTING_DRAW) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_OUTLINER) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_ID:
      if (ELEM(wmn->action, NA_RENAME, NA_ADDED, NA_REMOVED)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_MATERIAL:
      switch (wmn->data) {
        case ND_SHADING_LINKS:
          ED_region_tag_redraw_no_rebuild(region);
          break;
      }
      break;
    case NC_GEOM:
      switch (wmn->data) {
        case ND_VERTEX_GROUP:
        case ND_DATA:
          ED_region_tag_redraw(region);
          break;
      }
      break;
    case NC_ANIMATION:
      switch (wmn->data) {
        case ND_NLA_ACTCHANGE:
        case ND_KEYFRAME:
          ED_region_tag_redraw(region);
          break;
        case ND_ANIMCHAN:
          if (ELEM(wmn->action, NA_SELECTED, NA_RENAME)) {
            ED_region_tag_redraw(region);
          }
          break;
        case ND_NLA:
          if (ELEM(wmn->action, NA_ADDED, NA_REMOVED)) {
            ED_region_tag_redraw(region);
          }
          break;
        case ND_NLA_ORDER:
          ED_region_tag_redraw(region);
          break;
      }
      break;
    case NC_GPENCIL:
      if (ELEM(wmn->action, NA_EDITED, NA_SELECTED)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SCREEN:
      if (ELEM(wmn->data, ND_LAYOUTDELETE, ND_LAYER)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_MASK:
      if (ELEM(wmn->action, NA_ADDED)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_PAINTCURVE:
      if (ELEM(wmn->action, NA_ADDED)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_TEXT:
      if (ELEM(wmn->action, NA_ADDED, NA_REMOVED)) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

static void outliner_main_region_message_subscribe(const wmRegionMessageSubscribeParams *params)
{
  struct wmMsgBus *mbus = params->message_bus;
  ScrArea *area = params->area;
  ARegion *region = params->region;
  SpaceOutliner *space_outliner = static_cast<SpaceOutliner *>(area->spacedata.first);

  wmMsgSubscribeValue msg_sub_value_region_tag_redraw{};
  msg_sub_value_region_tag_redraw.owner = region;
  msg_sub_value_region_tag_redraw.user_data = region;
  msg_sub_value_region_tag_redraw.notify = ED_region_do_msg_notify_tag_redraw;

  if (ELEM(space_outliner->outlinevis, SO_VIEW_LAYER, SO_SCENES, SO_OVERRIDES_LIBRARY)) {
    WM_msg_subscribe_rna_anon_prop(mbus, Window, view_layer, &msg_sub_value_region_tag_redraw);
  }
}

/* ************************ header outliner area region *********************** */

/* add handlers, stuff you only do once or on area/region changes */
static void outliner_header_region_init(wmWindowManager *UNUSED(wm), ARegion *region)
{
  ED_region_header_init(region);
}

static void outliner_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

static void outliner_header_region_free(ARegion *UNUSED(region))
{
}

static void outliner_header_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  wmNotifier *wmn = params->notifier;

  /* context changes */
  switch (wmn->category) {
    case NC_SCENE:
      if (wmn->data == ND_KEYINGSET) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_OUTLINER) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

/* ******************** default callbacks for outliner space ***************** */

static SpaceLink *outliner_create(const ScrArea *UNUSED(area), const Scene *UNUSED(scene))
{
  ARegion *region;
  SpaceOutliner *space_outliner;

  space_outliner = MEM_cnew<SpaceOutliner>("initoutliner");
  space_outliner->spacetype = SPACE_OUTLINER;
  space_outliner->filter_id_type = ID_GR;
  space_outliner->show_restrict_flags = SO_RESTRICT_ENABLE | SO_RESTRICT_HIDE | SO_RESTRICT_RENDER;
  space_outliner->outlinevis = SO_VIEW_LAYER;
  space_outliner->sync_select_dirty |= WM_OUTLINER_SYNC_SELECT_FROM_ALL;
  space_outliner->flag = SO_SYNC_SELECT | SO_MODE_COLUMN;
  space_outliner->filter = SO_FILTER_NO_VIEW_LAYERS;

  /* header */
  region = MEM_cnew<ARegion>("header for outliner");

  BLI_addtail(&space_outliner->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  /* main region */
  region = MEM_cnew<ARegion>("main region for outliner");

  BLI_addtail(&space_outliner->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  return (SpaceLink *)space_outliner;
}

/* not spacelink itself */
static void outliner_free(SpaceLink *sl)
{
  SpaceOutliner *space_outliner = (SpaceOutliner *)sl;

  outliner_free_tree(&space_outliner->tree);
  if (space_outliner->treestore) {
    BLI_mempool_destroy(space_outliner->treestore);
  }

  MEM_delete(space_outliner->runtime);
}

/* spacetype; init callback */
static void outliner_init(wmWindowManager *UNUSED(wm), ScrArea *area)
{
  SpaceOutliner *space_outliner = static_cast<SpaceOutliner *>(area->spacedata.first);

  if (space_outliner->runtime == nullptr) {
    space_outliner->runtime = MEM_new<SpaceOutliner_Runtime>("SpaceOutliner_Runtime");
  }
}

static SpaceLink *outliner_duplicate(SpaceLink *sl)
{
  SpaceOutliner *space_outliner = (SpaceOutliner *)sl;
  SpaceOutliner *space_outliner_new = MEM_cnew<SpaceOutliner>(__func__, *space_outliner);

  BLI_listbase_clear(&space_outliner_new->tree);
  space_outliner_new->treestore = nullptr;

  space_outliner_new->sync_select_dirty = WM_OUTLINER_SYNC_SELECT_FROM_ALL;

  if (space_outliner->runtime) {
    /* Copy constructor handles details. */
    space_outliner_new->runtime = MEM_new<SpaceOutliner_Runtime>("SpaceOutliner_runtime dup",
                                                                 *space_outliner->runtime);
  }

  return (SpaceLink *)space_outliner_new;
}

static void outliner_id_remap(ScrArea *area, SpaceLink *slink, const struct IDRemapper *mappings)
{
  SpaceOutliner *space_outliner = (SpaceOutliner *)slink;

  if (!space_outliner->treestore) {
    return;
  }

  TreeStoreElem *tselem;
  BLI_mempool_iter iter;
  bool changed = false;
  bool unassigned = false;

  BLI_mempool_iternew(space_outliner->treestore, &iter);
  while ((tselem = static_cast<TreeStoreElem *>(BLI_mempool_iterstep(&iter)))) {
    switch (BKE_id_remapper_apply(mappings, &tselem->id, ID_REMAP_APPLY_DEFAULT)) {
      case ID_REMAP_RESULT_SOURCE_REMAPPED:
        changed = true;
        break;
      case ID_REMAP_RESULT_SOURCE_UNASSIGNED:
        changed = true;
        unassigned = true;
        break;
      case ID_REMAP_RESULT_SOURCE_UNAVAILABLE:
      case ID_REMAP_RESULT_SOURCE_NOT_MAPPABLE:
        break;
    }
  }

  /* Note that the Outliner may not be the active editor of the area, and hence not initialized.
   * So runtime data might not have been created yet. */
  if (space_outliner->runtime && space_outliner->runtime->tree_hash && changed) {
    /* rebuild hash table, because it depends on ids too */
    /* postpone a full rebuild because this can be called many times on-free */
    space_outliner->storeflag |= SO_TREESTORE_REBUILD;

    if (unassigned) {
      /* Redraw is needed when removing data for multiple outlines show the same data.
       * without this, the stale data won't get fully flushed when this outliner
       * is not the active outliner the user is interacting with. See T85976. */
      ED_area_tag_redraw(area);
    }
  }
}

static void outliner_deactivate(struct ScrArea *area)
{
  /* Remove hover highlights */
  SpaceOutliner *space_outliner = static_cast<SpaceOutliner *>(area->spacedata.first);
  outliner_flag_set(*space_outliner, TSE_HIGHLIGHTED_ANY, false);
  ED_region_tag_redraw_no_rebuild(BKE_area_find_region_type(area, RGN_TYPE_WINDOW));
}

void ED_spacetype_outliner(void)
{
  SpaceType *st = MEM_cnew<SpaceType>("spacetype time");
  ARegionType *art;

  st->spaceid = SPACE_OUTLINER;
  strncpy(st->name, "Outliner", BKE_ST_MAXNAME);

  st->create = outliner_create;
  st->free = outliner_free;
  st->init = outliner_init;
  st->duplicate = outliner_duplicate;
  st->operatortypes = outliner_operatortypes;
  st->keymap = outliner_keymap;
  st->dropboxes = outliner_dropboxes;
  st->id_remap = outliner_id_remap;
  st->deactivate = outliner_deactivate;
  st->context = outliner_context;

  /* regions: main window */
  art = MEM_cnew<ARegionType>("spacetype outliner region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;

  art->init = outliner_main_region_init;
  art->draw = outliner_main_region_draw;
  art->free = outliner_main_region_free;
  art->listener = outliner_main_region_listener;
  art->message_subscribe = outliner_main_region_message_subscribe;
  BLI_addhead(&st->regiontypes, art);

  /* regions: header */
  art = MEM_cnew<ARegionType>("spacetype outliner header region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;

  art->init = outliner_header_region_init;
  art->draw = outliner_header_region_draw;
  art->free = outliner_header_region_free;
  art->listener = outliner_header_region_listener;
  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(st);
}

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spclip
 */

#include <stdio.h>
#include <string.h>

#include "DNA_defaults.h"

#include "DNA_mask_types.h"
#include "DNA_movieclip_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h" /* for pivot point */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_lib_id.h"
#include "BKE_lib_remap.h"
#include "BKE_movieclip.h"
#include "BKE_screen.h"
#include "BKE_tracking.h"

#include "IMB_imbuf_types.h"

#include "ED_anim_api.h" /* for timeline cursor drawing */
#include "ED_clip.h"
#include "ED_mask.h"
#include "ED_screen.h"
#include "ED_space_api.h"
#include "ED_time_scrub_ui.h"
#include "ED_uvedit.h" /* just for ED_image_draw_cursor */

#include "IMB_imbuf.h"

#include "GPU_matrix.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_interface.h"
#include "UI_resources.h"
#include "UI_view2d.h"

#include "RNA_access.h"

#include "clip_intern.h" /* own include */

static void init_preview_region(const Scene *scene,
                                const ScrArea *area,
                                const SpaceClip *sc,
                                ARegion *region)
{
  region->regiontype = RGN_TYPE_PREVIEW;
  region->alignment = RGN_ALIGN_TOP;
  region->flag |= RGN_FLAG_HIDDEN;

  if (sc->view == SC_VIEW_DOPESHEET) {
    region->v2d.tot.xmin = -10.0f;
    region->v2d.tot.ymin = (float)(-area->winy) / 3.0f;
    region->v2d.tot.xmax = (float)(area->winx);
    region->v2d.tot.ymax = 0.0f;

    region->v2d.cur = region->v2d.tot;

    region->v2d.min[0] = 0.0f;
    region->v2d.min[1] = 0.0f;

    region->v2d.max[0] = MAXFRAMEF;
    region->v2d.max[1] = FLT_MAX;

    region->v2d.minzoom = 0.01f;
    region->v2d.maxzoom = 50;
    region->v2d.scroll = (V2D_SCROLL_BOTTOM | V2D_SCROLL_HORIZONTAL_HANDLES);
    region->v2d.scroll |= V2D_SCROLL_RIGHT;
    region->v2d.keepzoom = V2D_LOCKZOOM_Y;
    region->v2d.keepofs = V2D_KEEPOFS_Y;
    region->v2d.align = V2D_ALIGN_NO_POS_Y;
    region->v2d.flag = V2D_VIEWSYNC_AREA_VERTICAL;
  }
  else {
    region->v2d.tot.xmin = 0.0f;
    region->v2d.tot.ymin = -10.0f;
    region->v2d.tot.xmax = (float)scene->r.efra;
    region->v2d.tot.ymax = 10.0f;

    region->v2d.cur = region->v2d.tot;

    region->v2d.min[0] = FLT_MIN;
    region->v2d.min[1] = FLT_MIN;

    region->v2d.max[0] = MAXFRAMEF;
    region->v2d.max[1] = FLT_MAX;

    region->v2d.scroll = (V2D_SCROLL_BOTTOM | V2D_SCROLL_HORIZONTAL_HANDLES);
    region->v2d.scroll |= (V2D_SCROLL_RIGHT | V2D_SCROLL_VERTICAL_HANDLES);

    region->v2d.minzoom = 0.0f;
    region->v2d.maxzoom = 0.0f;
    region->v2d.keepzoom = 0;
    region->v2d.keepofs = 0;
    region->v2d.align = 0;
    region->v2d.flag = 0;

    region->v2d.keeptot = 0;
  }
}

static void reinit_preview_region(const bContext *C, ARegion *region)
{
  Scene *scene = CTX_data_scene(C);
  ScrArea *area = CTX_wm_area(C);
  SpaceClip *sc = CTX_wm_space_clip(C);

  if (sc->view == SC_VIEW_DOPESHEET) {
    if ((region->v2d.flag & V2D_VIEWSYNC_AREA_VERTICAL) == 0) {
      init_preview_region(scene, area, sc, region);
    }
  }
  else {
    if (region->v2d.flag & V2D_VIEWSYNC_AREA_VERTICAL) {
      init_preview_region(scene, area, sc, region);
    }
  }
}

static ARegion *ED_clip_has_preview_region(const bContext *C, ScrArea *area)
{
  ARegion *region, *arnew;

  region = BKE_area_find_region_type(area, RGN_TYPE_PREVIEW);
  if (region) {
    return region;
  }

  /* add subdiv level; after header */
  region = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);

  /* is error! */
  if (region == NULL) {
    return NULL;
  }

  arnew = MEM_callocN(sizeof(ARegion), "clip preview region");

  BLI_insertlinkbefore(&area->regionbase, region, arnew);
  init_preview_region(CTX_data_scene(C), area, CTX_wm_space_clip(C), arnew);

  return arnew;
}

static ARegion *ED_clip_has_channels_region(ScrArea *area)
{
  ARegion *region, *arnew;

  region = BKE_area_find_region_type(area, RGN_TYPE_CHANNELS);
  if (region) {
    return region;
  }

  /* add subdiv level; after header */
  region = BKE_area_find_region_type(area, RGN_TYPE_PREVIEW);

  /* is error! */
  if (region == NULL) {
    return NULL;
  }

  arnew = MEM_callocN(sizeof(ARegion), "clip channels region");

  BLI_insertlinkbefore(&area->regionbase, region, arnew);
  arnew->regiontype = RGN_TYPE_CHANNELS;
  arnew->alignment = RGN_ALIGN_LEFT;

  arnew->v2d.scroll = V2D_SCROLL_BOTTOM;
  arnew->v2d.flag = V2D_VIEWSYNC_AREA_VERTICAL;

  return arnew;
}

static void clip_scopes_tag_refresh(ScrArea *area)
{
  SpaceClip *sc = (SpaceClip *)area->spacedata.first;
  ARegion *region;

  if (sc->mode != SC_MODE_TRACKING) {
    return;
  }

  /* only while properties are visible */
  for (region = area->regionbase.first; region; region = region->next) {
    if (region->regiontype == RGN_TYPE_UI && region->flag & RGN_FLAG_HIDDEN) {
      return;
    }
  }

  sc->scopes.ok = false;
}

static void clip_scopes_check_gpencil_change(ScrArea *area)
{
  SpaceClip *sc = (SpaceClip *)area->spacedata.first;

  if (sc->gpencil_src == SC_GPENCIL_SRC_TRACK) {
    clip_scopes_tag_refresh(area);
  }
}

static void clip_area_sync_frame_from_scene(ScrArea *area, const Scene *scene)
{
  SpaceClip *space_clip = (SpaceClip *)area->spacedata.first;
  BKE_movieclip_user_set_frame(&space_clip->user, scene->r.cfra);
}

/* ******************** default callbacks for clip space ***************** */

static SpaceLink *clip_create(const ScrArea *area, const Scene *scene)
{
  ARegion *region;
  SpaceClip *sc;

  sc = DNA_struct_default_alloc(SpaceClip);

  /* header */
  region = MEM_callocN(sizeof(ARegion), "header for clip");

  BLI_addtail(&sc->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  /* tools view */
  region = MEM_callocN(sizeof(ARegion), "tools for clip");

  BLI_addtail(&sc->regionbase, region);
  region->regiontype = RGN_TYPE_TOOLS;
  region->alignment = RGN_ALIGN_LEFT;

  /* properties view */
  region = MEM_callocN(sizeof(ARegion), "properties for clip");

  BLI_addtail(&sc->regionbase, region);
  region->regiontype = RGN_TYPE_UI;
  region->alignment = RGN_ALIGN_RIGHT;

  /* channels view */
  region = MEM_callocN(sizeof(ARegion), "channels for clip");

  BLI_addtail(&sc->regionbase, region);
  region->regiontype = RGN_TYPE_CHANNELS;
  region->alignment = RGN_ALIGN_LEFT;

  region->v2d.scroll = V2D_SCROLL_BOTTOM;
  region->v2d.flag = V2D_VIEWSYNC_AREA_VERTICAL;

  /* preview view */
  region = MEM_callocN(sizeof(ARegion), "preview for clip");

  BLI_addtail(&sc->regionbase, region);
  init_preview_region(scene, area, sc, region);

  /* main region */
  region = MEM_callocN(sizeof(ARegion), "main region for clip");

  BLI_addtail(&sc->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  return (SpaceLink *)sc;
}

/* not spacelink itself */
static void clip_free(SpaceLink *sl)
{
  SpaceClip *sc = (SpaceClip *)sl;

  sc->clip = NULL;

  if (sc->scopes.track_preview) {
    IMB_freeImBuf(sc->scopes.track_preview);
  }

  if (sc->scopes.track_search) {
    IMB_freeImBuf(sc->scopes.track_search);
  }
}

/* spacetype; init callback */
static void clip_init(struct wmWindowManager *UNUSED(wm), ScrArea *area)
{
  ListBase *lb = WM_dropboxmap_find("Clip", SPACE_CLIP, 0);

  /* add drop boxes */
  WM_event_add_dropbox_handler(&area->handlers, lb);
}

static SpaceLink *clip_duplicate(SpaceLink *sl)
{
  SpaceClip *scn = MEM_dupallocN(sl);

  /* clear or remove stuff from old */
  scn->scopes.track_search = NULL;
  scn->scopes.track_preview = NULL;
  scn->scopes.ok = false;

  return (SpaceLink *)scn;
}

static void clip_listener(const wmSpaceTypeListenerParams *params)
{
  ScrArea *area = params->area;
  wmNotifier *wmn = params->notifier;
  const Scene *scene = params->scene;

  /* context changes */
  switch (wmn->category) {
    case NC_SCENE:
      switch (wmn->data) {
        case ND_FRAME:
          clip_scopes_tag_refresh(area);
          ATTR_FALLTHROUGH;

        case ND_FRAME_RANGE:
          ED_area_tag_redraw(area);
          break;
      }
      break;
    case NC_MOVIECLIP:
      switch (wmn->data) {
        case ND_DISPLAY:
        case ND_SELECT:
          clip_scopes_tag_refresh(area);
          ED_area_tag_redraw(area);
          break;
      }
      switch (wmn->action) {
        case NA_REMOVED:
        case NA_EDITED:
        case NA_EVALUATED:
          /* fall-through */

        case NA_SELECTED:
          clip_scopes_tag_refresh(area);
          ED_area_tag_redraw(area);
          break;
      }
      break;
    case NC_MASK:
      switch (wmn->data) {
        case ND_SELECT:
        case ND_DATA:
        case ND_DRAW:
          ED_area_tag_redraw(area);
          break;
      }
      switch (wmn->action) {
        case NA_SELECTED:
          ED_area_tag_redraw(area);
          break;
        case NA_EDITED:
          ED_area_tag_redraw(area);
          break;
      }
      break;
    case NC_GEOM:
      switch (wmn->data) {
        case ND_SELECT:
          clip_scopes_tag_refresh(area);
          ED_area_tag_redraw(area);
          break;
      }
      break;
    case NC_SCREEN:
      switch (wmn->data) {
        case ND_ANIMPLAY:
          ED_area_tag_redraw(area);
          break;
        case ND_LAYOUTSET:
          clip_area_sync_frame_from_scene(area, scene);
          break;
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_CLIP) {
        clip_scopes_tag_refresh(area);
        ED_area_tag_redraw(area);
      }
      break;
    case NC_GPENCIL:
      if (wmn->action == NA_EDITED) {
        clip_scopes_check_gpencil_change(area);
        ED_area_tag_redraw(area);
      }
      else if (wmn->data & ND_GPENCIL_EDITMODE) {
        ED_area_tag_redraw(area);
      }
      break;
    case NC_WM:
      switch (wmn->data) {
        case ND_FILEREAD:
        case ND_UNDO:
          clip_area_sync_frame_from_scene(area, scene);
          break;
      }
      break;
  }
}

static void clip_operatortypes(void)
{
  /* ** clip_ops.c ** */
  WM_operatortype_append(CLIP_OT_open);
  WM_operatortype_append(CLIP_OT_reload);
  WM_operatortype_append(CLIP_OT_view_pan);
  WM_operatortype_append(CLIP_OT_view_zoom);
  WM_operatortype_append(CLIP_OT_view_zoom_in);
  WM_operatortype_append(CLIP_OT_view_zoom_out);
  WM_operatortype_append(CLIP_OT_view_zoom_ratio);
  WM_operatortype_append(CLIP_OT_view_all);
  WM_operatortype_append(CLIP_OT_view_selected);
  WM_operatortype_append(CLIP_OT_view_center_cursor);
  WM_operatortype_append(CLIP_OT_change_frame);
  WM_operatortype_append(CLIP_OT_rebuild_proxy);
  WM_operatortype_append(CLIP_OT_mode_set);
#ifdef WITH_INPUT_NDOF
  WM_operatortype_append(CLIP_OT_view_ndof);
#endif
  WM_operatortype_append(CLIP_OT_prefetch);
  WM_operatortype_append(CLIP_OT_set_scene_frames);
  WM_operatortype_append(CLIP_OT_cursor_set);
  WM_operatortype_append(CLIP_OT_lock_selection_toggle);

  /* ** tracking_ops.c ** */

  /* navigation */
  WM_operatortype_append(CLIP_OT_frame_jump);

  /* set optical center to frame center */
  WM_operatortype_append(CLIP_OT_set_center_principal);

  /* selection */
  WM_operatortype_append(CLIP_OT_select);
  WM_operatortype_append(CLIP_OT_select_all);
  WM_operatortype_append(CLIP_OT_select_box);
  WM_operatortype_append(CLIP_OT_select_lasso);
  WM_operatortype_append(CLIP_OT_select_circle);
  WM_operatortype_append(CLIP_OT_select_grouped);

  /* markers */
  WM_operatortype_append(CLIP_OT_add_marker);
  WM_operatortype_append(CLIP_OT_add_marker_at_click);
  WM_operatortype_append(CLIP_OT_slide_marker);
  WM_operatortype_append(CLIP_OT_delete_track);
  WM_operatortype_append(CLIP_OT_delete_marker);

  /* track */
  WM_operatortype_append(CLIP_OT_track_markers);
  WM_operatortype_append(CLIP_OT_refine_markers);

  /* solving */
  WM_operatortype_append(CLIP_OT_solve_camera);
  WM_operatortype_append(CLIP_OT_clear_solution);

  WM_operatortype_append(CLIP_OT_disable_markers);
  WM_operatortype_append(CLIP_OT_hide_tracks);
  WM_operatortype_append(CLIP_OT_hide_tracks_clear);
  WM_operatortype_append(CLIP_OT_lock_tracks);

  WM_operatortype_append(CLIP_OT_set_solver_keyframe);

  /* orientation */
  WM_operatortype_append(CLIP_OT_set_origin);
  WM_operatortype_append(CLIP_OT_set_plane);
  WM_operatortype_append(CLIP_OT_set_axis);
  WM_operatortype_append(CLIP_OT_set_scale);
  WM_operatortype_append(CLIP_OT_set_solution_scale);
  WM_operatortype_append(CLIP_OT_apply_solution_scale);

  /* detect */
  WM_operatortype_append(CLIP_OT_detect_features);

  /* stabilization */
  WM_operatortype_append(CLIP_OT_stabilize_2d_add);
  WM_operatortype_append(CLIP_OT_stabilize_2d_remove);
  WM_operatortype_append(CLIP_OT_stabilize_2d_select);
  WM_operatortype_append(CLIP_OT_stabilize_2d_rotation_add);
  WM_operatortype_append(CLIP_OT_stabilize_2d_rotation_remove);
  WM_operatortype_append(CLIP_OT_stabilize_2d_rotation_select);

  /* clean-up */
  WM_operatortype_append(CLIP_OT_clear_track_path);
  WM_operatortype_append(CLIP_OT_join_tracks);
  WM_operatortype_append(CLIP_OT_average_tracks);
  WM_operatortype_append(CLIP_OT_track_copy_color);

  WM_operatortype_append(CLIP_OT_clean_tracks);

  /* object tracking */
  WM_operatortype_append(CLIP_OT_tracking_object_new);
  WM_operatortype_append(CLIP_OT_tracking_object_remove);

  /* clipboard */
  WM_operatortype_append(CLIP_OT_copy_tracks);
  WM_operatortype_append(CLIP_OT_paste_tracks);

  /* Plane tracker */
  WM_operatortype_append(CLIP_OT_create_plane_track);
  WM_operatortype_append(CLIP_OT_slide_plane_marker);

  WM_operatortype_append(CLIP_OT_keyframe_insert);
  WM_operatortype_append(CLIP_OT_keyframe_delete);

  WM_operatortype_append(CLIP_OT_new_image_from_plane_marker);
  WM_operatortype_append(CLIP_OT_update_image_from_plane_marker);

  /* ** clip_graph_ops.c  ** */

  /* graph editing */

  /* selection */
  WM_operatortype_append(CLIP_OT_graph_select);
  WM_operatortype_append(CLIP_OT_graph_select_box);
  WM_operatortype_append(CLIP_OT_graph_select_all_markers);

  WM_operatortype_append(CLIP_OT_graph_delete_curve);
  WM_operatortype_append(CLIP_OT_graph_delete_knot);
  WM_operatortype_append(CLIP_OT_graph_view_all);
  WM_operatortype_append(CLIP_OT_graph_center_current_frame);

  WM_operatortype_append(CLIP_OT_graph_disable_markers);

  /* ** clip_dopesheet_ops.c  ** */

  WM_operatortype_append(CLIP_OT_dopesheet_select_channel);
  WM_operatortype_append(CLIP_OT_dopesheet_view_all);
}

static void clip_keymap(struct wmKeyConfig *keyconf)
{
  /* ******** Global hotkeys available for all regions ******** */
  WM_keymap_ensure(keyconf, "Clip", SPACE_CLIP, 0);

  /* ******** Hotkeys available for main region only ******** */
  WM_keymap_ensure(keyconf, "Clip Editor", SPACE_CLIP, 0);
  //  keymap->poll = ED_space_clip_tracking_poll;

  /* ******** Hotkeys available for preview region only ******** */
  WM_keymap_ensure(keyconf, "Clip Graph Editor", SPACE_CLIP, 0);

  /* ******** Hotkeys available for channels region only ******** */
  WM_keymap_ensure(keyconf, "Clip Dopesheet Editor", SPACE_CLIP, 0);
}

/* DO NOT make this static, this hides the symbol and breaks API generation script. */
extern const char *clip_context_dir[]; /* quiet warning. */
const char *clip_context_dir[] = {"edit_movieclip", "edit_mask", NULL};

static int /*eContextResult*/ clip_context(const bContext *C,
                                           const char *member,
                                           bContextDataResult *result)
{
  SpaceClip *sc = CTX_wm_space_clip(C);

  if (CTX_data_dir(member)) {
    CTX_data_dir_set(result, clip_context_dir);

    return CTX_RESULT_OK;
  }
  if (CTX_data_equals(member, "edit_movieclip")) {
    if (sc->clip) {
      CTX_data_id_pointer_set(result, &sc->clip->id);
    }
    return CTX_RESULT_OK;
  }
  if (CTX_data_equals(member, "edit_mask")) {
    if (sc->mask_info.mask) {
      CTX_data_id_pointer_set(result, &sc->mask_info.mask->id);
    }
    return CTX_RESULT_OK;
  }

  return CTX_RESULT_MEMBER_NOT_FOUND;
}

/* dropboxes */
static bool clip_drop_poll(bContext *UNUSED(C), wmDrag *drag, const wmEvent *UNUSED(event))
{
  if (drag->type == WM_DRAG_PATH) {
    /* rule might not work? */
    if (ELEM(drag->icon, 0, ICON_FILE_IMAGE, ICON_FILE_MOVIE, ICON_FILE_BLANK)) {
      return true;
    }
  }

  return false;
}

static void clip_drop_copy(bContext *UNUSED(C), wmDrag *drag, wmDropBox *drop)
{
  PointerRNA itemptr;
  char dir[FILE_MAX], file[FILE_MAX];

  BLI_split_dirfile(drag->path, dir, file, sizeof(dir), sizeof(file));

  RNA_string_set(drop->ptr, "directory", dir);

  RNA_collection_clear(drop->ptr, "files");
  RNA_collection_add(drop->ptr, "files", &itemptr);
  RNA_string_set(&itemptr, "name", file);
}

/* area+region dropbox definition */
static void clip_dropboxes(void)
{
  ListBase *lb = WM_dropboxmap_find("Clip", SPACE_CLIP, 0);

  WM_dropbox_add(lb, "CLIP_OT_open", clip_drop_poll, clip_drop_copy, NULL, NULL);
}

static bool clip_set_region_visible(const bContext *C,
                                    ARegion *region,
                                    const bool is_visible,
                                    const short alignment,
                                    const bool view_all_on_show)
{
  bool view_changed = false;

  if (is_visible) {
    if (region && (region->flag & RGN_FLAG_HIDDEN)) {
      region->flag &= ~RGN_FLAG_HIDDEN;
      region->v2d.flag &= ~V2D_IS_INIT;
      if (view_all_on_show) {
        region->v2d.cur = region->v2d.tot;
      }
      view_changed = true;
    }
    if (region && region->alignment != alignment) {
      region->alignment = alignment;
      view_changed = true;
    }
  }
  else {
    if (region && !(region->flag & RGN_FLAG_HIDDEN)) {
      region->flag |= RGN_FLAG_HIDDEN;
      region->v2d.flag &= ~V2D_IS_INIT;
      WM_event_remove_handlers((bContext *)C, &region->handlers);
      view_changed = true;
    }
    if (region && region->alignment != RGN_ALIGN_NONE) {
      region->alignment = RGN_ALIGN_NONE;
      view_changed = true;
    }
  }

  return view_changed;
}

static void clip_refresh(const bContext *C, ScrArea *area)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *window = CTX_wm_window(C);
  Scene *scene = CTX_data_scene(C);
  SpaceClip *sc = (SpaceClip *)area->spacedata.first;
  ARegion *region_main = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);
  ARegion *region_tools = BKE_area_find_region_type(area, RGN_TYPE_TOOLS);
  ARegion *region_preview = ED_clip_has_preview_region(C, area);
  ARegion *region_properties = ED_clip_has_properties_region(area);
  ARegion *region_channels = ED_clip_has_channels_region(area);
  bool main_visible = false, preview_visible = false, tools_visible = false;
  bool properties_visible = false, channels_visible = false;
  bool view_changed = false;

  switch (sc->view) {
    case SC_VIEW_CLIP:
      main_visible = true;
      preview_visible = false;
      tools_visible = true;
      properties_visible = true;
      channels_visible = false;
      break;
    case SC_VIEW_GRAPH:
      main_visible = false;
      preview_visible = true;
      tools_visible = false;
      properties_visible = false;
      channels_visible = false;

      reinit_preview_region(C, region_preview);
      break;
    case SC_VIEW_DOPESHEET:
      main_visible = false;
      preview_visible = true;
      tools_visible = false;
      properties_visible = false;
      channels_visible = true;

      reinit_preview_region(C, region_preview);
      break;
  }

  view_changed |= clip_set_region_visible(C, region_main, main_visible, RGN_ALIGN_NONE, false);
  view_changed |= clip_set_region_visible(
      C, region_properties, properties_visible, RGN_ALIGN_RIGHT, false);
  view_changed |= clip_set_region_visible(C, region_tools, tools_visible, RGN_ALIGN_LEFT, false);
  view_changed |= clip_set_region_visible(
      C, region_preview, preview_visible, RGN_ALIGN_NONE, true);
  view_changed |= clip_set_region_visible(
      C, region_channels, channels_visible, RGN_ALIGN_LEFT, false);

  if (view_changed) {
    ED_area_init(wm, window, area);
    ED_area_tag_redraw(area);
  }

  BKE_movieclip_user_set_frame(&sc->user, scene->r.cfra);
}

static void CLIP_GGT_navigate(wmGizmoGroupType *gzgt)
{
  VIEW2D_GGT_navigate_impl(gzgt, "CLIP_GGT_navigate");
}

static void clip_gizmos(void)
{
  wmGizmoMapType *gzmap_type = WM_gizmomaptype_ensure(
      &(const struct wmGizmoMapType_Params){SPACE_CLIP, RGN_TYPE_WINDOW});

  WM_gizmogrouptype_append_and_link(gzmap_type, CLIP_GGT_navigate);
}

/********************* main region ********************/

/* sets up the fields of the View2D from zoom and offset */
static void movieclip_main_area_set_view2d(const bContext *C, ARegion *region)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  float x1, y1, w, h, aspx, aspy;
  int width, height, winx, winy;

  ED_space_clip_get_size(sc, &width, &height);
  ED_space_clip_get_aspect(sc, &aspx, &aspy);

  w = width * aspx;
  h = height * aspy;

  winx = BLI_rcti_size_x(&region->winrct) + 1;
  winy = BLI_rcti_size_y(&region->winrct) + 1;

  region->v2d.tot.xmin = 0;
  region->v2d.tot.ymin = 0;
  region->v2d.tot.xmax = w;
  region->v2d.tot.ymax = h;

  region->v2d.mask.xmin = region->v2d.mask.ymin = 0;
  region->v2d.mask.xmax = winx;
  region->v2d.mask.ymax = winy;

  /* which part of the image space do we see? */
  x1 = region->winrct.xmin + (winx - sc->zoom * w) / 2.0f;
  y1 = region->winrct.ymin + (winy - sc->zoom * h) / 2.0f;

  x1 -= sc->zoom * sc->xof;
  y1 -= sc->zoom * sc->yof;

  /* relative display right */
  region->v2d.cur.xmin = (region->winrct.xmin - (float)x1) / sc->zoom;
  region->v2d.cur.xmax = region->v2d.cur.xmin + ((float)winx / sc->zoom);

  /* relative display left */
  region->v2d.cur.ymin = (region->winrct.ymin - (float)y1) / sc->zoom;
  region->v2d.cur.ymax = region->v2d.cur.ymin + ((float)winy / sc->zoom);

  /* normalize 0.0..1.0 */
  region->v2d.cur.xmin /= w;
  region->v2d.cur.xmax /= w;
  region->v2d.cur.ymin /= h;
  region->v2d.cur.ymax /= h;
}

/* add handlers, stuff you only do once or on area/region changes */
static void clip_main_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  /* NOTE: don't use `UI_view2d_region_reinit(&region->v2d, ...)`
   * since the space clip manages own v2d in #movieclip_main_area_set_view2d */

  /* mask polls mode */
  keymap = WM_keymap_ensure(wm->defaultconf, "Mask Editing", 0, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);

  /* own keymap */
  keymap = WM_keymap_ensure(wm->defaultconf, "Clip", SPACE_CLIP, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);

  keymap = WM_keymap_ensure(wm->defaultconf, "Clip Editor", SPACE_CLIP, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);
}

static void clip_main_region_draw(const bContext *C, ARegion *region)
{
  /* draw entirely, view changes should be handled here */
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  float aspx, aspy, zoomx, zoomy, x, y;
  int width, height;
  bool show_cursor = false;

  /* If tracking is in progress, we should synchronize the frame from the clip-user
   * (#MovieClipUser.framenr) so latest tracked frame would be shown. */
  if (clip && clip->tracking_context) {
    BKE_autotrack_context_sync_user(clip->tracking_context, &sc->user);
  }

  if (sc->flag & SC_LOCK_SELECTION) {
    ImBuf *tmpibuf = NULL;

    if (clip && clip->tracking.stabilization.flag & TRACKING_2D_STABILIZATION) {
      tmpibuf = ED_space_clip_get_stable_buffer(sc, NULL, NULL, NULL);
    }

    if (ED_clip_view_selection(C, region, 0)) {
      sc->xof += sc->xlockof;
      sc->yof += sc->ylockof;
    }

    if (tmpibuf) {
      IMB_freeImBuf(tmpibuf);
    }
  }

  /* clear and setup matrix */
  UI_ThemeClearColor(TH_BACK);

  /* data... */
  movieclip_main_area_set_view2d(C, region);

  /* callback */
  ED_region_draw_cb_draw(C, region, REGION_DRAW_PRE_VIEW);

  clip_draw_main(C, sc, region);

  /* TODO(sergey): would be nice to find a way to de-duplicate all this space conversions */
  UI_view2d_view_to_region_fl(&region->v2d, 0.0f, 0.0f, &x, &y);
  ED_space_clip_get_size(sc, &width, &height);
  ED_space_clip_get_zoom(sc, region, &zoomx, &zoomy);
  ED_space_clip_get_aspect(sc, &aspx, &aspy);

  if (sc->mode == SC_MODE_MASKEDIT) {
    Mask *mask = CTX_data_edit_mask(C);
    if (mask && clip) {
      ScrArea *area = CTX_wm_area(C);
      int mask_width, mask_height;
      ED_mask_get_size(area, &mask_width, &mask_height);
      ED_mask_draw_region(CTX_data_expect_evaluated_depsgraph(C),
                          mask,
                          region,
                          sc->mask_info.draw_flag,
                          sc->mask_info.draw_type,
                          sc->mask_info.overlay_mode,
                          sc->mask_info.blend_factor,
                          mask_width,
                          mask_height,
                          aspx,
                          aspy,
                          true,
                          true,
                          sc->stabmat,
                          C);
    }
  }

  show_cursor |= sc->mode == SC_MODE_MASKEDIT;
  show_cursor |= sc->around == V3D_AROUND_CURSOR;

  if (show_cursor) {
    GPU_matrix_push();
    GPU_matrix_translate_2f(x, y);
    GPU_matrix_scale_2f(zoomx, zoomy);
    GPU_matrix_mul(sc->stabmat);
    GPU_matrix_scale_2f(width, height);
    ED_image_draw_cursor(region, sc->cursor);
    GPU_matrix_pop();
  }

  clip_draw_cache_and_notes(C, sc, region);

  if (sc->flag & SC_SHOW_ANNOTATION) {
    /* Grease Pencil */
    clip_draw_grease_pencil((bContext *)C, true);
  }

  /* callback */
  /* TODO(sergey): For being consistent with space image the projection needs to be configured
   * the way how the commented out code does it. This works correct for tracking data, but it
   * causes wrong aspect correction for mask editor (see T84990). */
  // GPU_matrix_push_projection();
  // wmOrtho2(region->v2d.cur.xmin, region->v2d.cur.xmax, region->v2d.cur.ymin,
  //          region->v2d.cur.ymax);
  ED_region_draw_cb_draw(C, region, REGION_DRAW_POST_VIEW);
  // GPU_matrix_pop_projection();

  /* reset view matrix */
  UI_view2d_view_restore(C);

  if (sc->flag & SC_SHOW_ANNOTATION) {
    /* draw Grease Pencil - screen space only */
    clip_draw_grease_pencil((bContext *)C, false);
  }

  WM_gizmomap_draw(region->gizmo_map, C, WM_GIZMOMAP_DRAWSTEP_2D);
}

static void clip_main_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  wmNotifier *wmn = params->notifier;

  /* context changes */
  switch (wmn->category) {
    case NC_GPENCIL:
      if (wmn->action == NA_EDITED) {
        ED_region_tag_redraw(region);
      }
      else if (wmn->data & ND_GPENCIL_EDITMODE) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

/****************** preview region ******************/

static void clip_preview_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_CUSTOM, region->winx, region->winy);

  /* own keymap */

  keymap = WM_keymap_ensure(wm->defaultconf, "Clip", SPACE_CLIP, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);

  keymap = WM_keymap_ensure(wm->defaultconf, "Clip Time Scrub", SPACE_CLIP, RGN_TYPE_PREVIEW);
  WM_event_add_keymap_handler_poll(&region->handlers, keymap, ED_time_scrub_event_in_region);

  keymap = WM_keymap_ensure(wm->defaultconf, "Clip Graph Editor", SPACE_CLIP, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);

  keymap = WM_keymap_ensure(wm->defaultconf, "Clip Dopesheet Editor", SPACE_CLIP, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);
}

static void graph_region_draw(const bContext *C, ARegion *region)
{
  View2D *v2d = &region->v2d;
  SpaceClip *sc = CTX_wm_space_clip(C);
  Scene *scene = CTX_data_scene(C);
  short cfra_flag = 0;

  if (sc->flag & SC_LOCK_TIMECURSOR) {
    ED_clip_graph_center_current_frame(scene, region);
  }

  /* clear and setup matrix */
  UI_ThemeClearColor(TH_BACK);

  UI_view2d_view_ortho(v2d);

  /* data... */
  clip_draw_graph(sc, region, scene);

  /* current frame indicator line */
  if (sc->flag & SC_SHOW_SECONDS) {
    cfra_flag |= DRAWCFRA_UNIT_SECONDS;
  }
  ANIM_draw_cfra(C, v2d, cfra_flag);

  /* reset view matrix */
  UI_view2d_view_restore(C);

  /* time-scrubbing */
  ED_time_scrub_draw(region, scene, sc->flag & SC_SHOW_SECONDS, true);

  /* current frame indicator */
  ED_time_scrub_draw_current_frame(region, scene, sc->flag & SC_SHOW_SECONDS);

  /* scrollers */
  UI_view2d_scrollers_draw(v2d, NULL);

  /* scale indicators */
  {
    rcti rect;
    BLI_rcti_init(
        &rect, 0, 15 * UI_DPI_FAC, 15 * UI_DPI_FAC, region->winy - UI_TIME_SCRUB_MARGIN_Y);
    UI_view2d_draw_scale_y__values(region, v2d, &rect, TH_TEXT);
  }
}

static void dopesheet_region_draw(const bContext *C, ARegion *region)
{
  Scene *scene = CTX_data_scene(C);
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  View2D *v2d = &region->v2d;
  short cfra_flag = 0;

  if (clip) {
    BKE_tracking_dopesheet_update(&clip->tracking);
  }

  /* clear and setup matrix */
  UI_ThemeClearColor(TH_BACK);

  UI_view2d_view_ortho(v2d);

  /* time grid */
  UI_view2d_draw_lines_x__discrete_frames_or_seconds(v2d, scene, sc->flag & SC_SHOW_SECONDS, true);

  /* data... */
  clip_draw_dopesheet_main(sc, region, scene);

  /* current frame indicator line */
  if (sc->flag & SC_SHOW_SECONDS) {
    cfra_flag |= DRAWCFRA_UNIT_SECONDS;
  }
  ANIM_draw_cfra(C, v2d, cfra_flag);

  /* reset view matrix */
  UI_view2d_view_restore(C);

  /* time-scrubbing */
  ED_time_scrub_draw(region, scene, sc->flag & SC_SHOW_SECONDS, true);

  /* current frame indicator */
  ED_time_scrub_draw_current_frame(region, scene, sc->flag & SC_SHOW_SECONDS);

  /* scrollers */
  UI_view2d_scrollers_draw(v2d, NULL);
}

static void clip_preview_region_draw(const bContext *C, ARegion *region)
{
  SpaceClip *sc = CTX_wm_space_clip(C);

  if (sc->view == SC_VIEW_GRAPH) {
    graph_region_draw(C, region);
  }
  else if (sc->view == SC_VIEW_DOPESHEET) {
    dopesheet_region_draw(C, region);
  }
}

static void clip_preview_region_listener(const wmRegionListenerParams *UNUSED(params))
{
}

/****************** channels region ******************/

static void clip_channels_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  /* ensure the 2d view sync works - main region has bottom scroller */
  region->v2d.scroll = V2D_SCROLL_BOTTOM;

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_LIST, region->winx, region->winy);

  keymap = WM_keymap_ensure(wm->defaultconf, "Clip Dopesheet Editor", SPACE_CLIP, 0);
  WM_event_add_keymap_handler_v2d_mask(&region->handlers, keymap);
}

static void clip_channels_region_draw(const bContext *C, ARegion *region)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  View2D *v2d = &region->v2d;

  if (clip) {
    BKE_tracking_dopesheet_update(&clip->tracking);
  }

  /* clear and setup matrix */
  UI_ThemeClearColor(TH_BACK);

  UI_view2d_view_ortho(v2d);

  /* data... */
  clip_draw_dopesheet_channels(C, region);

  /* reset view matrix */
  UI_view2d_view_restore(C);
}

static void clip_channels_region_listener(const wmRegionListenerParams *UNUSED(params))
{
}

/****************** header region ******************/

/* add handlers, stuff you only do once or on area/region changes */
static void clip_header_region_init(wmWindowManager *UNUSED(wm), ARegion *region)
{
  ED_region_header_init(region);
}

static void clip_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

static void clip_header_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  wmNotifier *wmn = params->notifier;

  /* context changes */
  switch (wmn->category) {
    case NC_SCENE:
      switch (wmn->data) {
        /* for proportional editmode only */
        case ND_TOOLSETTINGS:
          /* TODO: should do this when in mask mode only but no data available. */
          // if (sc->mode == SC_MODE_MASKEDIT)
          {
            ED_region_tag_redraw(region);
            break;
          }
      }
      break;
  }
}

/****************** tools region ******************/

/* add handlers, stuff you only do once or on area/region changes */
static void clip_tools_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  ED_region_panels_init(wm, region);

  keymap = WM_keymap_ensure(wm->defaultconf, "Clip", SPACE_CLIP, 0);
  WM_event_add_keymap_handler(&region->handlers, keymap);
}

static void clip_tools_region_draw(const bContext *C, ARegion *region)
{
  ED_region_panels(C, region);
}

/****************** tool properties region ******************/

static void clip_props_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  wmNotifier *wmn = params->notifier;

  /* context changes */
  switch (wmn->category) {
    case NC_WM:
      if (wmn->data == ND_HISTORY) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SCENE:
      if (wmn->data == ND_MODE) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_CLIP) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_GPENCIL:
      if (wmn->action == NA_EDITED) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

/****************** properties region ******************/

/* add handlers, stuff you only do once or on area/region changes */
static void clip_properties_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  ED_region_panels_init(wm, region);

  keymap = WM_keymap_ensure(wm->defaultconf, "Clip", SPACE_CLIP, 0);
  WM_event_add_keymap_handler(&region->handlers, keymap);
}

static void clip_properties_region_draw(const bContext *C, ARegion *region)
{
  SpaceClip *sc = CTX_wm_space_clip(C);

  BKE_movieclip_update_scopes(sc->clip, &sc->user, &sc->scopes);

  ED_region_panels(C, region);
}

static void clip_properties_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  wmNotifier *wmn = params->notifier;

  /* context changes */
  switch (wmn->category) {
    case NC_GPENCIL:
      if (ELEM(wmn->data, ND_DATA, ND_GPENCIL_EDITMODE)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_BRUSH:
      if (wmn->action == NA_EDITED) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

/********************* registration ********************/

static void clip_id_remap(ScrArea *UNUSED(area),
                          SpaceLink *slink,
                          const struct IDRemapper *mappings)
{
  SpaceClip *sclip = (SpaceClip *)slink;

  if (!BKE_id_remapper_has_mapping_for(mappings, FILTER_ID_MC | FILTER_ID_MSK)) {
    return;
  }

  BKE_id_remapper_apply(mappings, (ID **)&sclip->clip, ID_REMAP_APPLY_ENSURE_REAL);
  BKE_id_remapper_apply(mappings, (ID **)&sclip->mask_info.mask, ID_REMAP_APPLY_ENSURE_REAL);
}

void ED_spacetype_clip(void)
{
  SpaceType *st = MEM_callocN(sizeof(SpaceType), "spacetype clip");
  ARegionType *art;

  st->spaceid = SPACE_CLIP;
  strncpy(st->name, "Clip", BKE_ST_MAXNAME);

  st->create = clip_create;
  st->free = clip_free;
  st->init = clip_init;
  st->duplicate = clip_duplicate;
  st->operatortypes = clip_operatortypes;
  st->keymap = clip_keymap;
  st->listener = clip_listener;
  st->context = clip_context;
  st->gizmos = clip_gizmos;
  st->dropboxes = clip_dropboxes;
  st->refresh = clip_refresh;
  st->id_remap = clip_id_remap;

  /* regions: main window */
  art = MEM_callocN(sizeof(ARegionType), "spacetype clip region");
  art->regionid = RGN_TYPE_WINDOW;
  art->init = clip_main_region_init;
  art->draw = clip_main_region_draw;
  art->listener = clip_main_region_listener;
  art->keymapflag = ED_KEYMAP_GIZMO | ED_KEYMAP_FRAMES | ED_KEYMAP_UI | ED_KEYMAP_GPENCIL;

  BLI_addhead(&st->regiontypes, art);

  /* preview */
  art = MEM_callocN(sizeof(ARegionType), "spacetype clip region preview");
  art->regionid = RGN_TYPE_PREVIEW;
  art->prefsizey = 240;
  art->init = clip_preview_region_init;
  art->draw = clip_preview_region_draw;
  art->listener = clip_preview_region_listener;
  art->keymapflag = ED_KEYMAP_FRAMES | ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;

  BLI_addhead(&st->regiontypes, art);

  /* regions: properties */
  art = MEM_callocN(sizeof(ARegionType), "spacetype clip region properties");
  art->regionid = RGN_TYPE_UI;
  art->prefsizex = UI_SIDEBAR_PANEL_WIDTH;
  art->keymapflag = ED_KEYMAP_FRAMES | ED_KEYMAP_UI;
  art->init = clip_properties_region_init;
  art->draw = clip_properties_region_draw;
  art->listener = clip_properties_region_listener;
  BLI_addhead(&st->regiontypes, art);
  ED_clip_buttons_register(art);

  /* regions: tools */
  art = MEM_callocN(sizeof(ARegionType), "spacetype clip region tools");
  art->regionid = RGN_TYPE_TOOLS;
  art->prefsizex = UI_SIDEBAR_PANEL_WIDTH;
  art->keymapflag = ED_KEYMAP_FRAMES | ED_KEYMAP_UI;
  art->listener = clip_props_region_listener;
  art->init = clip_tools_region_init;
  art->draw = clip_tools_region_draw;

  BLI_addhead(&st->regiontypes, art);

  /* regions: header */
  art = MEM_callocN(sizeof(ARegionType), "spacetype clip region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_FRAMES | ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;

  art->init = clip_header_region_init;
  art->draw = clip_header_region_draw;
  art->listener = clip_header_region_listener;

  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(st);

  /* channels */
  art = MEM_callocN(sizeof(ARegionType), "spacetype clip channels region");
  art->regionid = RGN_TYPE_CHANNELS;
  art->prefsizex = UI_COMPACT_PANEL_WIDTH;
  art->keymapflag = ED_KEYMAP_FRAMES | ED_KEYMAP_UI;
  art->listener = clip_channels_region_listener;
  art->init = clip_channels_region_init;
  art->draw = clip_channels_region_draw;

  BLI_addhead(&st->regiontypes, art);

  /* regions: hud */
  art = ED_area_type_hud(st->spaceid);
  BLI_addhead(&st->regiontypes, art);
}

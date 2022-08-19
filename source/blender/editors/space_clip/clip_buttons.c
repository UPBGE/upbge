/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spclip
 */

#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_context.h"
#include "BKE_movieclip.h"
#include "BKE_screen.h"
#include "BKE_tracking.h"

#include "DEG_depsgraph.h"

#include "ED_clip.h"
#include "ED_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"

#include "WM_api.h"
#include "WM_types.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "clip_intern.h" /* own include */

/* Panels */

static bool metadata_panel_context_poll(const bContext *C, PanelType *UNUSED(pt))
{
  return ED_space_clip_poll((bContext *)C);
}

static void metadata_panel_context_draw(const bContext *C, Panel *panel)
{
  SpaceClip *space_clip = CTX_wm_space_clip(C);
  /* NOTE: This might not be exactly the same image buffer as shown in the
   * clip editor itself, since that might be coming from proxy, or being
   * postprocessed (stabilized or undistored).
   * Ideally we need to query metadata from an original image or movie without
   * reading actual pixels to speed up the process. */
  ImBuf *ibuf = ED_space_clip_get_buffer(space_clip);
  if (ibuf != NULL) {
    ED_region_image_metadata_panel_draw(ibuf, panel->layout);
    IMB_freeImBuf(ibuf);
  }
}

void ED_clip_buttons_register(ARegionType *art)
{
  PanelType *pt;

  pt = MEM_callocN(sizeof(PanelType), "spacetype clip panel metadata");
  strcpy(pt->idname, "CLIP_PT_metadata");
  strcpy(pt->label, N_("Metadata"));
  strcpy(pt->category, "Footage");
  strcpy(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->poll = metadata_panel_context_poll;
  pt->draw = metadata_panel_context_draw;
  pt->flag |= PANEL_TYPE_DEFAULT_CLOSED;
  BLI_addtail(&art->paneltypes, pt);
}

/********************* MovieClip Template ************************/

void uiTemplateMovieClip(
    uiLayout *layout, bContext *C, PointerRNA *ptr, const char *propname, bool compact)
{
  if (!ptr->data) {
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop) {
    printf(
        "%s: property not found: %s.%s\n", __func__, RNA_struct_identifier(ptr->type), propname);
    return;
  }

  if (RNA_property_type(prop) != PROP_POINTER) {
    printf("%s: expected pointer property for %s.%s\n",
           __func__,
           RNA_struct_identifier(ptr->type),
           propname);
    return;
  }

  PointerRNA clipptr = RNA_property_pointer_get(ptr, prop);
  MovieClip *clip = clipptr.data;

  uiLayoutSetContextPointer(layout, "edit_movieclip", &clipptr);

  if (!compact) {
    uiTemplateID(layout,
                 C,
                 ptr,
                 propname,
                 NULL,
                 "CLIP_OT_open",
                 NULL,
                 UI_TEMPLATE_ID_FILTER_ALL,
                 false,
                 NULL);
  }

  if (clip) {
    uiLayout *row = uiLayoutRow(layout, false);
    uiBlock *block = uiLayoutGetBlock(row);
    uiDefBut(block, UI_BTYPE_LABEL, 0, IFACE_("File Path:"), 0, 19, 145, 19, NULL, 0, 0, 0, 0, "");

    row = uiLayoutRow(layout, false);
    uiLayout *split = uiLayoutSplit(row, 0.0f, false);
    row = uiLayoutRow(split, true);

    uiItemR(row, &clipptr, "filepath", 0, "", ICON_NONE);
    uiItemO(row, "", ICON_FILE_REFRESH, "clip.reload");

    uiLayout *col = uiLayoutColumn(layout, false);
    uiTemplateColorspaceSettings(col, &clipptr, "colorspace_settings");
  }
}

/********************* Track Template ************************/

void uiTemplateTrack(uiLayout *layout, PointerRNA *ptr, const char *propname)
{
  if (!ptr->data) {
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop) {
    printf(
        "%s: property not found: %s.%s\n", __func__, RNA_struct_identifier(ptr->type), propname);
    return;
  }

  if (RNA_property_type(prop) != PROP_POINTER) {
    printf("%s: expected pointer property for %s.%s\n",
           __func__,
           RNA_struct_identifier(ptr->type),
           propname);
    return;
  }

  PointerRNA scopesptr = RNA_property_pointer_get(ptr, prop);
  MovieClipScopes *scopes = (MovieClipScopes *)scopesptr.data;

  if (scopes->track_preview_height < UI_UNIT_Y) {
    scopes->track_preview_height = UI_UNIT_Y;
  }
  else if (scopes->track_preview_height > UI_UNIT_Y * 20) {
    scopes->track_preview_height = UI_UNIT_Y * 20;
  }

  uiLayout *col = uiLayoutColumn(layout, true);
  uiBlock *block = uiLayoutGetBlock(col);

  uiDefBut(block,
           UI_BTYPE_TRACK_PREVIEW,
           0,
           "",
           0,
           0,
           UI_UNIT_X * 10,
           scopes->track_preview_height,
           scopes,
           0,
           0,
           0,
           0,
           "");

  /* Resize grip. */
  uiDefIconButI(block,
                UI_BTYPE_GRIP,
                0,
                ICON_GRIP,
                0,
                0,
                UI_UNIT_X * 10,
                (short)(UI_UNIT_Y * 0.8f),
                &scopes->track_preview_height,
                UI_UNIT_Y,
                UI_UNIT_Y * 20.0f,
                0.0f,
                0.0f,
                "");
}

/********************* Marker Template ************************/

#define B_MARKER_POS 3
#define B_MARKER_OFFSET 4
#define B_MARKER_PAT_DIM 5
#define B_MARKER_SEARCH_POS 6
#define B_MARKER_SEARCH_DIM 7
#define B_MARKER_FLAG 8

typedef struct {
  /** compact mode */
  int compact;

  MovieClip *clip;
  /** user of clip */
  MovieClipUser *user;
  MovieTrackingTrack *track;
  MovieTrackingMarker *marker;

  /** current frame number */
  int framenr;
  /** position of marker in pixel coords */
  float marker_pos[2];
  /** position and dimensions of marker pattern in pixel coords */
  float marker_pat[2];
  /** offset of "parenting" point */
  float track_offset[2];
  /** position and dimensions of marker search in pixel coords */
  float marker_search_pos[2], marker_search[2];
  /** marker's flags */
  int marker_flag;
} MarkerUpdateCb;

static void to_pixel_space(float r[2], const float a[2], int width, int height)
{
  copy_v2_v2(r, a);
  r[0] *= width;
  r[1] *= height;
}

static void marker_update_cb(bContext *C, void *arg_cb, void *UNUSED(arg))
{
  MarkerUpdateCb *cb = (MarkerUpdateCb *)arg_cb;

  if (!cb->compact) {
    return;
  }

  int clip_framenr = BKE_movieclip_remap_scene_to_clip_frame(cb->clip, cb->framenr);
  MovieTrackingMarker *marker = BKE_tracking_marker_ensure(cb->track, clip_framenr);
  marker->flag = cb->marker_flag;

  WM_event_add_notifier(C, NC_MOVIECLIP | NA_EDITED, NULL);
}

static void marker_block_handler(bContext *C, void *arg_cb, int event)
{
  MarkerUpdateCb *cb = (MarkerUpdateCb *)arg_cb;
  int width, height;
  bool ok = false;

  BKE_movieclip_get_size(cb->clip, cb->user, &width, &height);

  int clip_framenr = BKE_movieclip_remap_scene_to_clip_frame(cb->clip, cb->framenr);
  MovieTrackingMarker *marker = BKE_tracking_marker_ensure(cb->track, clip_framenr);

  if (event == B_MARKER_POS) {
    marker->pos[0] = cb->marker_pos[0] / width;
    marker->pos[1] = cb->marker_pos[1] / height;

    /* to update position of "parented" objects */
    DEG_id_tag_update(&cb->clip->id, 0);
    WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, NULL);

    ok = true;
  }
  else if (event == B_MARKER_PAT_DIM) {
    float dim[2], pat_dim[2], pat_min[2], pat_max[2];

    BKE_tracking_marker_pattern_minmax(cb->marker, pat_min, pat_max);

    sub_v2_v2v2(pat_dim, pat_max, pat_min);

    dim[0] = cb->marker_pat[0] / width;
    dim[1] = cb->marker_pat[1] / height;

    float scale_x = dim[0] / pat_dim[0];
    float scale_y = dim[1] / pat_dim[1];

    for (int a = 0; a < 4; a++) {
      cb->marker->pattern_corners[a][0] *= scale_x;
      cb->marker->pattern_corners[a][1] *= scale_y;
    }

    BKE_tracking_marker_clamp_search_size(cb->marker);

    ok = true;
  }
  else if (event == B_MARKER_SEARCH_POS) {
    float delta[2], side[2];

    sub_v2_v2v2(side, cb->marker->search_max, cb->marker->search_min);
    mul_v2_fl(side, 0.5f);

    delta[0] = cb->marker_search_pos[0] / width;
    delta[1] = cb->marker_search_pos[1] / height;

    sub_v2_v2v2(cb->marker->search_min, delta, side);
    add_v2_v2v2(cb->marker->search_max, delta, side);

    BKE_tracking_marker_clamp_search_position(cb->marker);

    ok = true;
  }
  else if (event == B_MARKER_SEARCH_DIM) {
    float dim[2], search_dim[2];

    sub_v2_v2v2(search_dim, cb->marker->search_max, cb->marker->search_min);

    dim[0] = cb->marker_search[0] / width;
    dim[1] = cb->marker_search[1] / height;

    sub_v2_v2(dim, search_dim);
    mul_v2_fl(dim, 0.5f);

    cb->marker->search_min[0] -= dim[0];
    cb->marker->search_min[1] -= dim[1];

    cb->marker->search_max[0] += dim[0];
    cb->marker->search_max[1] += dim[1];

    BKE_tracking_marker_clamp_search_size(cb->marker);

    ok = true;
  }
  else if (event == B_MARKER_FLAG) {
    marker->flag = cb->marker_flag;

    ok = true;
  }
  else if (event == B_MARKER_OFFSET) {
    float offset[2], delta[2];

    offset[0] = cb->track_offset[0] / width;
    offset[1] = cb->track_offset[1] / height;

    sub_v2_v2v2(delta, offset, cb->track->offset);
    copy_v2_v2(cb->track->offset, offset);

    for (int i = 0; i < cb->track->markersnr; i++) {
      sub_v2_v2(cb->track->markers[i].pos, delta);
    }

    /* to update position of "parented" objects */
    DEG_id_tag_update(&cb->clip->id, 0);
    WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, NULL);

    ok = true;
  }

  if (ok) {
    WM_event_add_notifier(C, NC_MOVIECLIP | NA_EDITED, cb->clip);
  }
}

void uiTemplateMarker(uiLayout *layout,
                      PointerRNA *ptr,
                      const char *propname,
                      PointerRNA *userptr,
                      PointerRNA *trackptr,
                      bool compact)
{
  if (!ptr->data) {
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop) {
    printf(
        "%s: property not found: %s.%s\n", __func__, RNA_struct_identifier(ptr->type), propname);
    return;
  }

  if (RNA_property_type(prop) != PROP_POINTER) {
    printf("%s: expected pointer property for %s.%s\n",
           __func__,
           RNA_struct_identifier(ptr->type),
           propname);
    return;
  }

  PointerRNA clipptr = RNA_property_pointer_get(ptr, prop);
  MovieClip *clip = (MovieClip *)clipptr.data;
  MovieClipUser *user = userptr->data;
  MovieTrackingTrack *track = trackptr->data;

  int clip_framenr = BKE_movieclip_remap_scene_to_clip_frame(clip, user->framenr);
  MovieTrackingMarker *marker = BKE_tracking_marker_get(track, clip_framenr);

  MarkerUpdateCb *cb = MEM_callocN(sizeof(MarkerUpdateCb), "uiTemplateMarker update_cb");
  cb->compact = compact;
  cb->clip = clip;
  cb->user = user;
  cb->track = track;
  cb->marker = marker;
  cb->marker_flag = marker->flag;
  cb->framenr = user->framenr;

  if (compact) {
    const char *tip;
    uiBlock *block = uiLayoutGetBlock(layout);

    if (cb->marker_flag & MARKER_DISABLED) {
      tip = TIP_("Marker is disabled at current frame");
    }
    else {
      tip = TIP_("Marker is enabled at current frame");
    }

    uiBut *bt = uiDefIconButBitI(block,
                                 UI_BTYPE_TOGGLE_N,
                                 MARKER_DISABLED,
                                 0,
                                 ICON_HIDE_OFF,
                                 0,
                                 0,
                                 UI_UNIT_X,
                                 UI_UNIT_Y,
                                 &cb->marker_flag,
                                 0,
                                 0,
                                 1,
                                 0,
                                 tip);
    UI_but_funcN_set(bt, marker_update_cb, cb, NULL);
    UI_but_drawflag_enable(bt, UI_BUT_ICON_REVERSE);
  }
  else {
    int width, height;

    BKE_movieclip_get_size(clip, user, &width, &height);

    if (track->flag & TRACK_LOCKED) {
      uiLayoutSetActive(layout, false);
      uiBlock *block = uiLayoutAbsoluteBlock(layout);
      uiDefBut(block,
               UI_BTYPE_LABEL,
               0,
               IFACE_("Track is locked"),
               0,
               0,
               UI_UNIT_X * 15.0f,
               UI_UNIT_Y,
               NULL,
               0,
               0,
               0,
               0,
               "");

      return;
    }

    float pat_min[2], pat_max[2];
    float pat_dim[2], search_dim[2], search_pos[2];

    BKE_tracking_marker_pattern_minmax(marker, pat_min, pat_max);

    sub_v2_v2v2(pat_dim, pat_max, pat_min);
    sub_v2_v2v2(search_dim, marker->search_max, marker->search_min);

    add_v2_v2v2(search_pos, marker->search_max, marker->search_min);
    mul_v2_fl(search_pos, 0.5);

    to_pixel_space(cb->marker_pos, marker->pos, width, height);
    to_pixel_space(cb->marker_pat, pat_dim, width, height);
    to_pixel_space(cb->marker_search, search_dim, width, height);
    to_pixel_space(cb->marker_search_pos, search_pos, width, height);
    to_pixel_space(cb->track_offset, track->offset, width, height);

    cb->marker_flag = marker->flag;

    uiBlock *block = uiLayoutAbsoluteBlock(layout);
    UI_block_func_handle_set(block, marker_block_handler, cb);
    UI_block_funcN_set(block, marker_update_cb, cb, NULL);

    const char *tip;
    int step = 100;
    int digits = 2;

    if (cb->marker_flag & MARKER_DISABLED) {
      tip = TIP_("Marker is disabled at current frame");
    }
    else {
      tip = TIP_("Marker is enabled at current frame");
    }

    uiDefButBitI(block,
                 UI_BTYPE_CHECKBOX_N,
                 MARKER_DISABLED,
                 B_MARKER_FLAG,
                 IFACE_("Enabled"),
                 0.5 * UI_UNIT_X,
                 9.5 * UI_UNIT_Y,
                 7.25 * UI_UNIT_X,
                 UI_UNIT_Y,
                 &cb->marker_flag,
                 0,
                 0,
                 0,
                 0,
                 tip);

    uiLayout *col = uiLayoutColumn(layout, true);
    uiLayoutSetActive(col, (cb->marker_flag & MARKER_DISABLED) == 0);

    block = uiLayoutAbsoluteBlock(col);
    UI_block_align_begin(block);

    uiDefBut(block,
             UI_BTYPE_LABEL,
             0,
             IFACE_("Position:"),
             0,
             10 * UI_UNIT_Y,
             15 * UI_UNIT_X,
             UI_UNIT_Y,
             NULL,
             0,
             0,
             0,
             0,
             "");
    uiBut *bt = uiDefButF(block,
                          UI_BTYPE_NUM,
                          B_MARKER_POS,
                          IFACE_("X:"),
                          0.5 * UI_UNIT_X,
                          9 * UI_UNIT_Y,
                          7.25 * UI_UNIT_X,
                          UI_UNIT_Y,
                          &cb->marker_pos[0],
                          -10 * width,
                          10.0 * width,
                          0,
                          0,
                          TIP_("X-position of marker at frame in screen coordinates"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_POS,
                   IFACE_("Y:"),
                   8.25 * UI_UNIT_X,
                   9 * UI_UNIT_Y,
                   7.25 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->marker_pos[1],
                   -10 * height,
                   10.0 * height,
                   0,
                   0,
                   TIP_("Y-position of marker at frame in screen coordinates"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);

    uiDefBut(block,
             UI_BTYPE_LABEL,
             0,
             IFACE_("Offset:"),
             0,
             8 * UI_UNIT_Y,
             15 * UI_UNIT_X,
             UI_UNIT_Y,
             NULL,
             0,
             0,
             0,
             0,
             "");
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_OFFSET,
                   IFACE_("X:"),
                   0.5 * UI_UNIT_X,
                   7 * UI_UNIT_Y,
                   7.25 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->track_offset[0],
                   -10 * width,
                   10.0 * width,
                   0,
                   0,
                   TIP_("X-offset to parenting point"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_OFFSET,
                   IFACE_("Y:"),
                   8.25 * UI_UNIT_X,
                   7 * UI_UNIT_Y,
                   7.25 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->track_offset[1],
                   -10 * height,
                   10.0 * height,
                   0,
                   0,
                   TIP_("Y-offset to parenting point"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);

    uiDefBut(block,
             UI_BTYPE_LABEL,
             0,
             IFACE_("Pattern Area:"),
             0,
             6 * UI_UNIT_Y,
             15 * UI_UNIT_X,
             UI_UNIT_Y,
             NULL,
             0,
             0,
             0,
             0,
             "");
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_PAT_DIM,
                   IFACE_("Width:"),
                   0.5 * UI_UNIT_X,
                   5 * UI_UNIT_Y,
                   15 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->marker_pat[0],
                   3.0f,
                   10.0 * width,
                   0,
                   0,
                   TIP_("Width of marker's pattern in screen coordinates"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_PAT_DIM,
                   IFACE_("Height:"),
                   0.5 * UI_UNIT_X,
                   4 * UI_UNIT_Y,
                   15 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->marker_pat[1],
                   3.0f,
                   10.0 * height,
                   0,
                   0,
                   TIP_("Height of marker's pattern in screen coordinates"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);

    uiDefBut(block,
             UI_BTYPE_LABEL,
             0,
             IFACE_("Search Area:"),
             0,
             3 * UI_UNIT_Y,
             15 * UI_UNIT_X,
             UI_UNIT_Y,
             NULL,
             0,
             0,
             0,
             0,
             "");
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_SEARCH_POS,
                   IFACE_("X:"),
                   0.5 * UI_UNIT_X,
                   2 * UI_UNIT_Y,
                   7.25 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->marker_search_pos[0],
                   -width,
                   width,
                   0,
                   0,
                   TIP_("X-position of search at frame relative to marker's position"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_SEARCH_POS,
                   IFACE_("Y:"),
                   8.25 * UI_UNIT_X,
                   2 * UI_UNIT_Y,
                   7.25 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->marker_search_pos[1],
                   -height,
                   height,
                   0,
                   0,
                   TIP_("Y-position of search at frame relative to marker's position"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_SEARCH_DIM,
                   IFACE_("Width:"),
                   0.5 * UI_UNIT_X,
                   1 * UI_UNIT_Y,
                   15 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->marker_search[0],
                   3.0f,
                   10.0 * width,
                   0,
                   0,
                   TIP_("Width of marker's search in screen coordinates"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   B_MARKER_SEARCH_DIM,
                   IFACE_("Height:"),
                   0.5 * UI_UNIT_X,
                   0 * UI_UNIT_Y,
                   15 * UI_UNIT_X,
                   UI_UNIT_Y,
                   &cb->marker_search[1],
                   3.0f,
                   10.0 * height,
                   0,
                   0,
                   TIP_("Height of marker's search in screen coordinates"));
    UI_but_number_step_size_set(bt, step);
    UI_but_number_precision_set(bt, digits);

    UI_block_align_end(block);
  }
}

/********************* Footage Information Template ************************/

void uiTemplateMovieclipInformation(uiLayout *layout,
                                    PointerRNA *ptr,
                                    const char *propname,
                                    PointerRNA *userptr)
{
  if (!ptr->data) {
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop) {
    printf(
        "%s: property not found: %s.%s\n", __func__, RNA_struct_identifier(ptr->type), propname);
    return;
  }

  if (RNA_property_type(prop) != PROP_POINTER) {
    printf("%s: expected pointer property for %s.%s\n",
           __func__,
           RNA_struct_identifier(ptr->type),
           propname);
    return;
  }

  PointerRNA clipptr = RNA_property_pointer_get(ptr, prop);
  MovieClip *clip = (MovieClip *)clipptr.data;
  MovieClipUser *user = userptr->data;

  uiLayout *col = uiLayoutColumn(layout, false);
  uiLayoutSetAlignment(col, UI_LAYOUT_ALIGN_RIGHT);

  ImBuf *ibuf = BKE_movieclip_get_ibuf_flag(clip, user, clip->flag, MOVIECLIP_CACHE_SKIP);

  int width, height;
  /* Display frame dimensions, channels number and buffer type. */
  BKE_movieclip_get_size(clip, user, &width, &height);

  char str[1024];
  size_t ofs = 0;
  ofs += BLI_snprintf_rlen(str + ofs, sizeof(str) - ofs, TIP_("%d x %d"), width, height);

  if (ibuf) {
    if (ibuf->rect_float) {
      if (ibuf->channels != 4) {
        ofs += BLI_snprintf_rlen(
            str + ofs, sizeof(str) - ofs, TIP_(", %d float channel(s)"), ibuf->channels);
      }
      else if (ibuf->planes == R_IMF_PLANES_RGBA) {
        ofs += BLI_strncpy_rlen(str + ofs, TIP_(", RGBA float"), sizeof(str) - ofs);
      }
      else {
        ofs += BLI_strncpy_rlen(str + ofs, TIP_(", RGB float"), sizeof(str) - ofs);
      }
    }
    else {
      if (ibuf->planes == R_IMF_PLANES_RGBA) {
        ofs += BLI_strncpy_rlen(str + ofs, TIP_(", RGBA byte"), sizeof(str) - ofs);
      }
      else {
        ofs += BLI_strncpy_rlen(str + ofs, TIP_(", RGB byte"), sizeof(str) - ofs);
      }
    }

    if (clip->anim != NULL) {
      short frs_sec;
      float frs_sec_base;
      if (IMB_anim_get_fps(clip->anim, &frs_sec, &frs_sec_base, true)) {
        ofs += BLI_snprintf_rlen(
            str + ofs, sizeof(str) - ofs, TIP_(", %.2f fps"), (float)frs_sec / frs_sec_base);
      }
    }
  }
  else {
    ofs += BLI_strncpy_rlen(str + ofs, TIP_(", failed to load"), sizeof(str) - ofs);
  }

  uiItemL(col, str, ICON_NONE);

  /* Display current frame number. */
  int framenr = BKE_movieclip_remap_scene_to_clip_frame(clip, user->framenr);
  if (framenr <= clip->len) {
    BLI_snprintf(str, sizeof(str), TIP_("Frame: %d / %d"), framenr, clip->len);
  }
  else {
    BLI_snprintf(str, sizeof(str), TIP_("Frame: - / %d"), clip->len);
  }
  uiItemL(col, str, ICON_NONE);

  /* Display current file name if it's a sequence clip. */
  if (clip->source == MCLIP_SRC_SEQUENCE) {
    char filepath[FILE_MAX];
    const char *file;

    if (framenr <= clip->len) {
      BKE_movieclip_filename_for_frame(clip, user, filepath);
      file = BLI_path_slash_rfind(filepath);
    }
    else {
      file = "-";
    }

    BLI_snprintf(str, sizeof(str), TIP_("File: %s"), file);

    uiItemL(col, str, ICON_NONE);
  }

  IMB_freeImBuf(ibuf);
}

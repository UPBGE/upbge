/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spclip
 */

#include "DNA_scene_types.h"

#include "BLI_math.h"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_tracking.h"

#include "DEG_depsgraph.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_clip.h"
#include "ED_screen.h"
#include "ED_select_utils.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "UI_view2d.h"

#include "clip_intern.h" /* own include */

/******************** common graph-editing utilities ********************/

static bool ED_space_clip_graph_poll(bContext *C)
{
  if (ED_space_clip_tracking_poll(C)) {
    SpaceClip *sc = CTX_wm_space_clip(C);

    return sc->view == SC_VIEW_GRAPH;
  }

  return false;
}

static bool clip_graph_knots_poll(bContext *C)
{
  if (ED_space_clip_graph_poll(C)) {
    SpaceClip *sc = CTX_wm_space_clip(C);

    return (sc->flag & (SC_SHOW_GRAPH_TRACKS_MOTION | SC_SHOW_GRAPH_TRACKS_ERROR)) != 0;
  }
  return false;
}

typedef struct {
  int action;
} SelectUserData;

static void toggle_selection_cb(void *userdata, MovieTrackingMarker *marker)
{
  SelectUserData *data = (SelectUserData *)userdata;

  switch (data->action) {
    case SEL_SELECT:
      marker->flag |= MARKER_GRAPH_SEL;
      break;
    case SEL_DESELECT:
      marker->flag &= ~MARKER_GRAPH_SEL;
      break;
    case SEL_INVERT:
      marker->flag ^= MARKER_GRAPH_SEL;
      break;
  }
}

/******************** mouse select operator ********************/

typedef struct {
  SpaceClip *sc;
  eClipCurveValueSource value_source;
  bool has_prev; /* if there's valid coordinate of previous point of curve segment */

  float min_dist_sq; /* minimal distance between mouse and currently found entity */
  float mouse_co[2]; /* mouse coordinate */
  float prev_co[2];  /* coordinate of previous point of segment */
  float min_co[2];   /* coordinate of entity with minimal distance */

  MovieTrackingTrack *track;   /* nearest found track */
  MovieTrackingMarker *marker; /* nearest found marker */
} MouseSelectUserData;

static void find_nearest_tracking_segment_cb(void *userdata,
                                             MovieTrackingTrack *track,
                                             MovieTrackingMarker *UNUSED(marker),
                                             eClipCurveValueSource value_source,
                                             int scene_framenr,
                                             float val)
{
  MouseSelectUserData *data = userdata;
  const float co[2] = {scene_framenr, val};

  if (!clip_graph_value_visible(data->sc, value_source)) {
    return;
  }

  if (data->has_prev) {
    float dist_sq = dist_squared_to_line_segment_v2(data->mouse_co, data->prev_co, co);

    if (data->track == NULL || dist_sq < data->min_dist_sq) {
      data->track = track;
      data->min_dist_sq = dist_sq;
      data->value_source = value_source;
      copy_v2_v2(data->min_co, co);
    }
  }

  data->has_prev = true;
  copy_v2_v2(data->prev_co, co);
}

static void find_nearest_tracking_segment_end_cb(void *userdata,
                                                 eClipCurveValueSource UNUSED(source_value))
{
  MouseSelectUserData *data = userdata;

  data->has_prev = false;
}

static void find_nearest_tracking_knot_cb(void *userdata,
                                          MovieTrackingTrack *track,
                                          MovieTrackingMarker *marker,
                                          eClipCurveValueSource value_source,
                                          int scene_framenr,
                                          float val)
{
  MouseSelectUserData *data = userdata;
  const float mdiff[2] = {scene_framenr - data->mouse_co[0], val - data->mouse_co[1]};
  float dist_sq = len_squared_v2(mdiff);

  if (!clip_graph_value_visible(data->sc, value_source)) {
    return;
  }

  if (data->marker == NULL || dist_sq < data->min_dist_sq) {
    const float co[2] = {scene_framenr, val};

    data->track = track;
    data->marker = marker;
    data->min_dist_sq = dist_sq;
    data->value_source = value_source;
    copy_v2_v2(data->min_co, co);
  }
}

static void mouse_select_init_data(bContext *C, MouseSelectUserData *userdata, const float co[2])
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  memset(userdata, 0, sizeof(MouseSelectUserData));
  userdata->sc = sc;
  userdata->min_dist_sq = FLT_MAX;
  copy_v2_v2(userdata->mouse_co, co);
}

static bool mouse_select_knot(bContext *C, const float co[2], bool extend)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  ARegion *region = CTX_wm_region(C);
  View2D *v2d = &region->v2d;
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingTrack *act_track = BKE_tracking_track_get_active(tracking);
  static const int delta = 6;

  if (act_track) {
    MouseSelectUserData userdata;

    mouse_select_init_data(C, &userdata, co);
    clip_graph_tracking_values_iterate_track(
        sc, act_track, &userdata, find_nearest_tracking_knot_cb, NULL, NULL);

    if (userdata.marker) {
      int x1, y1, x2, y2;

      if (UI_view2d_view_to_region_clip(v2d, co[0], co[1], &x1, &y1) &&
          UI_view2d_view_to_region_clip(v2d, userdata.min_co[0], userdata.min_co[1], &x2, &y2) &&
          (abs(x2 - x1) <= delta && abs(y2 - y1) <= delta)) {
        if (!extend) {
          SelectUserData selectdata = {SEL_DESELECT};

          clip_graph_tracking_iterate(sc,
                                      (sc->flag & SC_SHOW_GRAPH_SEL_ONLY) != 0,
                                      (sc->flag & SC_SHOW_GRAPH_HIDDEN) != 0,
                                      &selectdata,
                                      toggle_selection_cb);
        }

        if (userdata.value_source == CLIP_VALUE_SOURCE_SPEED_X) {
          if (extend && (userdata.marker->flag & MARKER_GRAPH_SEL_X) != 0) {
            userdata.marker->flag &= ~MARKER_GRAPH_SEL_X;
          }
          else {
            userdata.marker->flag |= MARKER_GRAPH_SEL_X;
          }
        }
        else if (userdata.value_source == CLIP_VALUE_SOURCE_SPEED_Y) {
          if (extend && (userdata.marker->flag & MARKER_GRAPH_SEL_Y) != 0) {
            userdata.marker->flag &= ~MARKER_GRAPH_SEL_Y;
          }
          else {
            userdata.marker->flag |= MARKER_GRAPH_SEL_Y;
          }
        }

        return true;
      }
    }
  }

  return false;
}

static bool mouse_select_curve(bContext *C, const float co[2], bool extend)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingTrack *act_track = BKE_tracking_track_get_active(tracking);
  MouseSelectUserData userdata;

  mouse_select_init_data(C, &userdata, co);
  clip_graph_tracking_values_iterate(sc,
                                     (sc->flag & SC_SHOW_GRAPH_SEL_ONLY) != 0,
                                     (sc->flag & SC_SHOW_GRAPH_HIDDEN) != 0,
                                     &userdata,
                                     find_nearest_tracking_segment_cb,
                                     NULL,
                                     find_nearest_tracking_segment_end_cb);

  if (userdata.track) {
    if (extend) {
      if (act_track == userdata.track) {
        /* currently only single curve can be selected
         * (selected curve represents active track) */
        act_track = NULL;
      }
    }
    else if (act_track != userdata.track) {
      SelectUserData selectdata = {SEL_DESELECT};
      MovieTrackingObject *object = BKE_tracking_object_get_active(tracking);

      tracking->act_track = userdata.track;
      if ((sc->flag & SC_SHOW_GRAPH_SEL_ONLY) == 0) {
        ListBase *tracksbase = BKE_tracking_object_get_tracks(tracking, object);
        BKE_tracking_track_select(tracksbase, userdata.track, TRACK_AREA_ALL, false);
      }

      /* deselect all knots on newly selected curve */
      clip_graph_tracking_iterate(sc,
                                  (sc->flag & SC_SHOW_GRAPH_SEL_ONLY) != 0,
                                  (sc->flag & SC_SHOW_GRAPH_HIDDEN) != 0,
                                  &selectdata,
                                  toggle_selection_cb);
    }

    return true;
  }

  return false;
}

static int mouse_select(bContext *C, float co[2], bool extend)
{
  bool sel = false;

  /* first try to select knot on selected curves */
  sel = mouse_select_knot(C, co, extend);

  if (!sel) {
    /* if there's no close enough knot to mouse position, select nearest curve */
    sel = mouse_select_curve(C, co, extend);
  }

  if (sel) {
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, NULL);
  }

  return OPERATOR_FINISHED;
}

static int select_exec(bContext *C, wmOperator *op)
{
  float co[2];
  bool extend = RNA_boolean_get(op->ptr, "extend");

  RNA_float_get_array(op->ptr, "location", co);

  return mouse_select(C, co, extend);
}

static int select_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  float co[2];

  UI_view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &co[0], &co[1]);
  RNA_float_set_array(op->ptr, "location", co);

  return select_exec(C, op);
}

void CLIP_OT_graph_select(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select";
  ot->description = "Select graph curves";
  ot->idname = "CLIP_OT_graph_select";

  /* api callbacks */
  ot->exec = select_exec;
  ot->invoke = select_invoke;
  ot->poll = clip_graph_knots_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* properties */
  RNA_def_float_vector(ot->srna,
                       "location",
                       2,
                       NULL,
                       -FLT_MAX,
                       FLT_MAX,
                       "Location",
                       "Mouse location to select nearest entity",
                       -100.0f,
                       100.0f);
  prop = RNA_def_boolean(ot->srna,
                         "extend",
                         0,
                         "Extend",
                         "Extend selection rather than clearing the existing selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/********************** box select operator *********************/

typedef struct BoxSelectuserData {
  rctf rect;
  bool select, extend, changed;
} BoxSelectuserData;

static void box_select_cb(void *userdata,
                          MovieTrackingTrack *UNUSED(track),
                          MovieTrackingMarker *marker,
                          eClipCurveValueSource value_source,
                          int scene_framenr,
                          float val)
{
  BoxSelectuserData *data = (BoxSelectuserData *)userdata;
  if (!ELEM(value_source, CLIP_VALUE_SOURCE_SPEED_X, CLIP_VALUE_SOURCE_SPEED_Y)) {
    return;
  }

  if (BLI_rctf_isect_pt(&data->rect, scene_framenr, val)) {
    int flag = 0;

    if (value_source == CLIP_VALUE_SOURCE_SPEED_X) {
      flag = MARKER_GRAPH_SEL_X;
    }
    else {
      flag = MARKER_GRAPH_SEL_Y;
    }

    if (data->select) {
      marker->flag |= flag;
    }
    else {
      marker->flag &= ~flag;
    }
    data->changed = true;
  }
  else if (!data->extend) {
    marker->flag &= ~MARKER_GRAPH_SEL;
  }
}

static int box_select_graph_exec(bContext *C, wmOperator *op)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  ARegion *region = CTX_wm_region(C);

  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingTrack *act_track = BKE_tracking_track_get_active(tracking);
  BoxSelectuserData userdata;
  rctf rect;

  if (act_track == NULL) {
    return OPERATOR_CANCELLED;
  }

  /* get rectangle from operator */
  WM_operator_properties_border_to_rctf(op, &rect);
  UI_view2d_region_to_view_rctf(&region->v2d, &rect, &userdata.rect);

  userdata.changed = false;
  userdata.select = !RNA_boolean_get(op->ptr, "deselect");
  userdata.extend = RNA_boolean_get(op->ptr, "extend");

  clip_graph_tracking_values_iterate_track(sc, act_track, &userdata, box_select_cb, NULL, NULL);

  if (userdata.changed) {
    WM_event_add_notifier(C, NC_GEOM | ND_SELECT, NULL);

    return OPERATOR_FINISHED;
  }

  return OPERATOR_CANCELLED;
}

void CLIP_OT_graph_select_box(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Box Select";
  ot->description = "Select curve points using box selection";
  ot->idname = "CLIP_OT_graph_select_box";

  /* api callbacks */
  ot->invoke = WM_gesture_box_invoke;
  ot->exec = box_select_graph_exec;
  ot->modal = WM_gesture_box_modal;
  ot->poll = clip_graph_knots_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_gesture_box_select(ot);
}

/********************** select all operator *********************/

static int graph_select_all_markers_exec(bContext *C, wmOperator *op)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingTrack *act_track = BKE_tracking_track_get_active(tracking);
  MovieTrackingMarker *marker;
  int action = RNA_enum_get(op->ptr, "action");
  int a;

  if (!act_track) {
    return OPERATOR_CANCELLED;
  }

  if (action == SEL_TOGGLE) {
    action = SEL_SELECT;

    for (a = 0; a < act_track->markersnr; a++) {
      marker = &act_track->markers[a];

      if (marker->flag & MARKER_GRAPH_SEL) {
        action = SEL_DESELECT;
        break;
      }
    }
  }

  for (a = 0; a < act_track->markersnr; a++) {
    marker = &act_track->markers[a];

    switch (action) {
      case SEL_SELECT:
        marker->flag |= MARKER_GRAPH_SEL;
        break;
      case SEL_DESELECT:
        marker->flag &= ~MARKER_GRAPH_SEL;
        break;
      case SEL_INVERT:
        marker->flag ^= MARKER_GRAPH_SEL;
        break;
    }
  }

  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, NULL);

  return OPERATOR_FINISHED;
}

void CLIP_OT_graph_select_all_markers(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "(De)select All Markers";
  ot->description = "Change selection of all markers of active track";
  ot->idname = "CLIP_OT_graph_select_all_markers";

  /* api callbacks */
  ot->exec = graph_select_all_markers_exec;
  ot->poll = clip_graph_knots_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_select_all(ot);
}

/******************** delete curve operator ********************/

static int delete_curve_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingTrack *act_track = BKE_tracking_track_get_active(tracking);

  if (!act_track) {
    return OPERATOR_CANCELLED;
  }

  clip_delete_track(C, clip, act_track);

  return OPERATOR_FINISHED;
}

void CLIP_OT_graph_delete_curve(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Curve";
  ot->description = "Delete track corresponding to the selected curve";
  ot->idname = "CLIP_OT_graph_delete_curve";

  /* api callbacks */
  ot->invoke = WM_operator_confirm;
  ot->exec = delete_curve_exec;
  ot->poll = clip_graph_knots_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/******************** delete knot operator ********************/

static int delete_knot_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingTrack *act_track = BKE_tracking_track_get_active(tracking);

  if (act_track) {
    int a = 0;

    while (a < act_track->markersnr) {
      MovieTrackingMarker *marker = &act_track->markers[a];

      if (marker->flag & MARKER_GRAPH_SEL) {
        clip_delete_marker(C, clip, act_track, marker);
      }
      else {
        a++;
      }
    }
  }

  return OPERATOR_FINISHED;
}

void CLIP_OT_graph_delete_knot(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Knot";
  ot->description = "Delete curve knots";
  ot->idname = "CLIP_OT_graph_delete_knot";

  /* api callbacks */
  ot->exec = delete_knot_exec;
  ot->poll = clip_graph_knots_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/******************** view all operator ********************/

typedef struct {
  float min, max;
} ViewAllUserData;

static void view_all_cb(void *userdata,
                        MovieTrackingTrack *UNUSED(track),
                        MovieTrackingMarker *UNUSED(marker),
                        eClipCurveValueSource UNUSED(value_source),
                        int UNUSED(scene_framenr),
                        float val)
{
  ViewAllUserData *data = (ViewAllUserData *)userdata;

  if (val < data->min) {
    data->min = val;
  }

  if (val > data->max) {
    data->max = val;
  }
}

static int view_all_exec(bContext *C, wmOperator *UNUSED(op))
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceClip *sc = CTX_wm_space_clip(C);
  View2D *v2d = &region->v2d;
  ViewAllUserData userdata;
  float extra;

  userdata.max = -FLT_MAX;
  userdata.min = FLT_MAX;

  clip_graph_tracking_values_iterate(sc,
                                     (sc->flag & SC_SHOW_GRAPH_SEL_ONLY) != 0,
                                     (sc->flag & SC_SHOW_GRAPH_HIDDEN) != 0,
                                     &userdata,
                                     view_all_cb,
                                     NULL,
                                     NULL);

  /* set extents of view to start/end frames */
  v2d->cur.xmin = (float)scene->r.sfra;
  v2d->cur.xmax = (float)scene->r.efra;

  if (userdata.min < userdata.max) {
    v2d->cur.ymin = userdata.min;
    v2d->cur.ymax = userdata.max;
  }
  else {
    v2d->cur.ymin = -10;
    v2d->cur.ymax = 10;
  }

  /* we need an extra "buffer" factor on either side so that the endpoints are visible */
  extra = 0.01f * BLI_rctf_size_x(&v2d->cur);
  v2d->cur.xmin -= extra;
  v2d->cur.xmax += extra;

  extra = 0.01f * BLI_rctf_size_y(&v2d->cur);
  v2d->cur.ymin -= extra;
  v2d->cur.ymax += extra;

  ED_region_tag_redraw(region);

  return OPERATOR_FINISHED;
}

void CLIP_OT_graph_view_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Frame All";
  ot->description = "View all curves in editor";
  ot->idname = "CLIP_OT_graph_view_all";

  /* api callbacks */
  ot->exec = view_all_exec;
  ot->poll = ED_space_clip_graph_poll;
}

/******************** jump to current frame operator ********************/

void ED_clip_graph_center_current_frame(Scene *scene, ARegion *region)
{
  View2D *v2d = &region->v2d;
  float extra = BLI_rctf_size_x(&v2d->cur) / 2.0f;

  /* set extents of view to start/end frames */
  v2d->cur.xmin = (float)scene->r.cfra - extra;
  v2d->cur.xmax = (float)scene->r.cfra + extra;
}

static int center_current_frame_exec(bContext *C, wmOperator *UNUSED(op))
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);

  ED_clip_graph_center_current_frame(scene, region);

  ED_region_tag_redraw(region);

  return OPERATOR_FINISHED;
}

void CLIP_OT_graph_center_current_frame(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Center Current Frame";
  ot->description = "Scroll view so current frame would be centered";
  ot->idname = "CLIP_OT_graph_center_current_frame";

  /* api callbacks */
  ot->exec = center_current_frame_exec;
  ot->poll = ED_space_clip_graph_poll;
}

/********************** disable markers operator *********************/

static int graph_disable_markers_exec(bContext *C, wmOperator *op)
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingTrack *act_track = BKE_tracking_track_get_active(tracking);
  MovieTrackingMarker *marker;
  int action = RNA_enum_get(op->ptr, "action");
  int a;

  if (!act_track || (act_track->flag & TRACK_LOCKED)) {
    return OPERATOR_CANCELLED;
  }

  for (a = 0; a < act_track->markersnr; a++) {
    marker = &act_track->markers[a];

    if (marker->flag & MARKER_GRAPH_SEL) {
      if (action == 0) {
        marker->flag |= MARKER_DISABLED;
      }
      else if (action == 1) {
        marker->flag &= ~MARKER_DISABLED;
      }
      else {
        marker->flag ^= MARKER_DISABLED;
      }
    }
  }

  DEG_id_tag_update(&clip->id, 0);

  WM_event_add_notifier(C, NC_MOVIECLIP | NA_EVALUATED, clip);

  return OPERATOR_FINISHED;
}

void CLIP_OT_graph_disable_markers(wmOperatorType *ot)
{
  static const EnumPropertyItem actions_items[] = {
      {0, "DISABLE", 0, "Disable", "Disable selected markers"},
      {1, "ENABLE", 0, "Enable", "Enable selected markers"},
      {2, "TOGGLE", 0, "Toggle", "Toggle disabled flag for selected markers"},
      {0, NULL, 0, NULL, NULL},
  };

  /* identifiers */
  ot->name = "Disable Markers";
  ot->description = "Disable/enable selected markers";
  ot->idname = "CLIP_OT_graph_disable_markers";

  /* api callbacks */
  ot->exec = graph_disable_markers_exec;
  ot->poll = ED_space_clip_graph_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_enum(ot->srna, "action", actions_items, 0, "Action", "Disable action to execute");
}

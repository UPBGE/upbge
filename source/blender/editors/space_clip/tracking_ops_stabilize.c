/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup spclip
 */

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_utildefines.h"

#include "BKE_context.h"
#include "BKE_tracking.h"

#include "DEG_depsgraph.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_clip.h"

#include "clip_intern.h"

/********************* add 2d stabilization tracks operator ********************/

static bool stabilize_2d_poll(bContext *C)
{
  if (ED_space_clip_tracking_poll(C)) {
    SpaceClip *sc = CTX_wm_space_clip(C);
    MovieClip *clip = ED_space_clip_get_clip(sc);
    MovieTrackingObject *tracking_object = BKE_tracking_object_get_active(&clip->tracking);
    return (tracking_object->flag & TRACKING_OBJECT_CAMERA) != 0;
  }
  return 0;
}

static int stabilize_2d_add_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  ListBase *tracksbase = BKE_tracking_get_active_tracks(tracking);
  MovieTrackingStabilization *stab = &tracking->stabilization;

  bool update = false;
  for (MovieTrackingTrack *track = tracksbase->first; track != NULL; track = track->next) {
    if (TRACK_VIEW_SELECTED(sc, track) && (track->flag & TRACK_USE_2D_STAB) == 0) {
      track->flag |= TRACK_USE_2D_STAB;
      stab->tot_track++;
      update = true;
    }
  }

  if (update) {
    DEG_id_tag_update(&clip->id, 0);
    WM_event_add_notifier(C, NC_MOVIECLIP | ND_DISPLAY, clip);
  }

  return OPERATOR_FINISHED;
}

void CLIP_OT_stabilize_2d_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Stabilization Tracks";
  ot->description = "Add selected tracks to 2D translation stabilization";
  ot->idname = "CLIP_OT_stabilize_2d_add";

  /* api callbacks */
  ot->exec = stabilize_2d_add_exec;
  ot->poll = stabilize_2d_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/******************* remove 2d stabilization tracks operator ******************/

static int stabilize_2d_remove_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingStabilization *stab = &tracking->stabilization;
  ListBase *tracksbase = BKE_tracking_get_active_tracks(tracking);
  int a = 0;
  bool update = false;

  for (MovieTrackingTrack *track = tracksbase->first; track != NULL; track = track->next) {
    if (track->flag & TRACK_USE_2D_STAB) {
      if (a == stab->act_track) {
        track->flag &= ~TRACK_USE_2D_STAB;
        stab->act_track--;
        stab->tot_track--;
        if (stab->act_track < 0) {
          stab->act_track = 0;
        }
        update = true;
        break;
      }
      a++;
    }
  }

  if (update) {
    DEG_id_tag_update(&clip->id, 0);
    WM_event_add_notifier(C, NC_MOVIECLIP | ND_DISPLAY, clip);
  }

  return OPERATOR_FINISHED;
}

void CLIP_OT_stabilize_2d_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Stabilization Track";
  ot->description = "Remove selected track from translation stabilization";
  ot->idname = "CLIP_OT_stabilize_2d_remove";

  /* api callbacks */
  ot->exec = stabilize_2d_remove_exec;
  ot->poll = stabilize_2d_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/******************* select 2d stabilization tracks operator ******************/

static int stabilize_2d_select_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  ListBase *tracksbase = BKE_tracking_get_active_tracks(tracking);
  bool update = false;

  for (MovieTrackingTrack *track = tracksbase->first; track != NULL; track = track->next) {
    if (track->flag & TRACK_USE_2D_STAB) {
      BKE_tracking_track_flag_set(track, TRACK_AREA_ALL, SELECT);
      update = true;
    }
  }

  if (update) {
    WM_event_add_notifier(C, NC_MOVIECLIP | ND_SELECT, clip);
  }

  return OPERATOR_FINISHED;
}

void CLIP_OT_stabilize_2d_select(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Stabilization Tracks";
  ot->description = "Select tracks which are used for translation stabilization";
  ot->idname = "CLIP_OT_stabilize_2d_select";

  /* api callbacks */
  ot->exec = stabilize_2d_select_exec;
  ot->poll = stabilize_2d_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/********************** add 2d stabilization tracks for rotation operator ****************/

static int stabilize_2d_rotation_add_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  ListBase *tracksbase = BKE_tracking_get_active_tracks(tracking);
  MovieTrackingStabilization *stab = &tracking->stabilization;

  bool update = false;
  for (MovieTrackingTrack *track = tracksbase->first; track != NULL; track = track->next) {
    if (TRACK_VIEW_SELECTED(sc, track) && (track->flag & TRACK_USE_2D_STAB_ROT) == 0) {
      track->flag |= TRACK_USE_2D_STAB_ROT;
      stab->tot_rot_track++;
      update = true;
    }
  }

  if (update) {
    DEG_id_tag_update(&clip->id, 0);
    WM_event_add_notifier(C, NC_MOVIECLIP | ND_DISPLAY, clip);
  }

  return OPERATOR_FINISHED;
}

void CLIP_OT_stabilize_2d_rotation_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Stabilization Rotation Tracks";
  ot->description = "Add selected tracks to 2D rotation stabilization";
  ot->idname = "CLIP_OT_stabilize_2d_rotation_add";

  /* api callbacks */
  ot->exec = stabilize_2d_rotation_add_exec;
  ot->poll = stabilize_2d_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/********************** remove 2d stabilization tracks for rotation operator *************/

static int stabilize_2d_rotation_remove_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  MovieTrackingStabilization *stab = &tracking->stabilization;
  ListBase *tracksbase = BKE_tracking_get_active_tracks(tracking);
  int a = 0;
  bool update = false;

  for (MovieTrackingTrack *track = tracksbase->first; track != NULL; track = track->next) {
    if (track->flag & TRACK_USE_2D_STAB_ROT) {
      if (a == stab->act_rot_track) {
        track->flag &= ~TRACK_USE_2D_STAB_ROT;
        stab->act_rot_track--;
        stab->tot_rot_track--;
        if (stab->act_rot_track < 0) {
          stab->act_rot_track = 0;
        }
        update = true;
        break;
      }
      a++;
    }
  }

  if (update) {
    DEG_id_tag_update(&clip->id, 0);
    WM_event_add_notifier(C, NC_MOVIECLIP | ND_DISPLAY, clip);
  }

  return OPERATOR_FINISHED;
}

void CLIP_OT_stabilize_2d_rotation_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Stabilization Rotation Track";
  ot->description = "Remove selected track from rotation stabilization";
  ot->idname = "CLIP_OT_stabilize_2d_rotation_remove";

  /* api callbacks */
  ot->exec = stabilize_2d_rotation_remove_exec;
  ot->poll = stabilize_2d_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/********************** select 2d stabilization rotation tracks operator *****************/

static int stabilize_2d_rotation_select_exec(bContext *C, wmOperator *UNUSED(op))
{
  SpaceClip *sc = CTX_wm_space_clip(C);
  MovieClip *clip = ED_space_clip_get_clip(sc);
  MovieTracking *tracking = &clip->tracking;
  ListBase *tracksbase = BKE_tracking_get_active_tracks(tracking);
  bool update = false;

  for (MovieTrackingTrack *track = tracksbase->first; track != NULL; track = track->next) {
    if (track->flag & TRACK_USE_2D_STAB_ROT) {
      BKE_tracking_track_flag_set(track, TRACK_AREA_ALL, SELECT);
      update = true;
    }
  }

  if (update) {
    WM_event_add_notifier(C, NC_MOVIECLIP | ND_SELECT, clip);
  }

  return OPERATOR_FINISHED;
}

void CLIP_OT_stabilize_2d_rotation_select(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Stabilization Rotation Tracks";
  ot->description = "Select tracks which are used for rotation stabilization";
  ot->idname = "CLIP_OT_stabilize_2d_rotation_select";

  /* api callbacks */
  ot->exec = stabilize_2d_rotation_select_exec;
  ot->poll = stabilize_2d_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

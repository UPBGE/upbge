/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation, Joshua Leung. All rights reserved. */

/** \file
 * \ingroup spnla
 */

#include <stdio.h>
#include <string.h>

#include "DNA_anim_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"

#include "BKE_context.h"
#include "BKE_nla.h"
#include "BKE_screen.h"

#include "ED_anim_api.h"
#include "ED_keyframes_edit.h"
#include "ED_screen.h"
#include "ED_select_utils.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_interface.h"
#include "UI_view2d.h"

#include "nla_intern.h" /* own include */

/* ******************** Utilities ***************************************** */

/* Convert SELECT_* flags to ACHANNEL_SETFLAG_* flags */
static short selmodes_to_flagmodes(short sel)
{
  /* convert selection modes to selection modes */
  switch (sel) {
    case SELECT_SUBTRACT:
      return ACHANNEL_SETFLAG_CLEAR;

    case SELECT_INVERT:
      return ACHANNEL_SETFLAG_INVERT;

    case SELECT_ADD:
    default:
      return ACHANNEL_SETFLAG_ADD;
  }
}

/* ******************** Deselect All Operator ***************************** */
/* This operator works in one of three ways:
 * 1) (de)select all (AKEY) - test if select all or deselect all
 * 2) invert all (CTRL-IKEY) - invert selection of all keyframes
 * 3) (de)select all - no testing is done; only for use internal tools as normal function...
 */

enum {
  DESELECT_STRIPS_NOTEST = 0,
  DESELECT_STRIPS_TEST,
  DESELECT_STRIPS_CLEARACTIVE,
} /*eDeselectNlaStrips*/;

/* Deselects strips in the NLA Editor
 * - This is called by the deselect all operator, as well as other ones!
 *
 * - test: check if select or deselect all (1) or clear all active (2)
 * - sel: how to select keyframes
 * 0 = deselect
 * 1 = select
 * 2 = invert
 */
static void deselect_nla_strips(bAnimContext *ac, short test, short sel)
{
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;
  short smode;

  /* determine type-based settings */
  /* FIXME: double check whether ANIMFILTER_LIST_VISIBLE is needed! */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FCURVESONLY);

  /* filter data */
  ANIM_animdata_filter(ac, &anim_data, filter, ac->data, ac->datatype);

  /* See if we should be selecting or deselecting */
  if (test == DESELECT_STRIPS_TEST) {
    for (ale = anim_data.first; ale; ale = ale->next) {
      NlaTrack *nlt = (NlaTrack *)ale->data;
      NlaStrip *strip;

      /* if any strip is selected, break out, since we should now be deselecting */
      for (strip = nlt->strips.first; strip; strip = strip->next) {
        if (strip->flag & NLASTRIP_FLAG_SELECT) {
          sel = SELECT_SUBTRACT;
          break;
        }
      }

      if (sel == SELECT_SUBTRACT) {
        break;
      }
    }
  }

  /* convert selection modes to selection modes */
  smode = selmodes_to_flagmodes(sel);

  /* Now set the flags */
  for (ale = anim_data.first; ale; ale = ale->next) {
    NlaTrack *nlt = (NlaTrack *)ale->data;
    NlaStrip *strip;

    /* apply same selection to all strips */
    for (strip = nlt->strips.first; strip; strip = strip->next) {
      /* set selection */
      if (test != DESELECT_STRIPS_CLEARACTIVE) {
        ACHANNEL_SET_FLAG(strip, smode, NLASTRIP_FLAG_SELECT);
      }

      /* clear active flag */
      /* TODO: for clear active,
       * do we want to limit this to only doing this on a certain set of tracks though? */
      strip->flag &= ~NLASTRIP_FLAG_ACTIVE;
    }
  }

  /* Cleanup */
  ANIM_animdata_freelist(&anim_data);
}

/* ------------------- */

static int nlaedit_deselectall_exec(bContext *C, wmOperator *op)
{
  bAnimContext ac;

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* 'standard' behavior - check if selected, then apply relevant selection */
  const int action = RNA_enum_get(op->ptr, "action");
  switch (action) {
    case SEL_TOGGLE:
      deselect_nla_strips(&ac, DESELECT_STRIPS_TEST, SELECT_ADD);
      break;
    case SEL_SELECT:
      deselect_nla_strips(&ac, DESELECT_STRIPS_NOTEST, SELECT_ADD);
      break;
    case SEL_DESELECT:
      deselect_nla_strips(&ac, DESELECT_STRIPS_NOTEST, SELECT_SUBTRACT);
      break;
    case SEL_INVERT:
      deselect_nla_strips(&ac, DESELECT_STRIPS_NOTEST, SELECT_INVERT);
      break;
    default:
      BLI_assert(0);
      break;
  }

  /* set notifier that things have changed */
  WM_event_add_notifier(C, NC_ANIMATION | ND_NLA | NA_SELECTED, NULL);

  return OPERATOR_FINISHED;
}

void NLA_OT_select_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "(De)select All";
  ot->idname = "NLA_OT_select_all";
  ot->description = "Select or deselect all NLA-Strips";

  /* api callbacks */
  ot->exec = nlaedit_deselectall_exec;
  ot->poll = nlaop_poll_tweakmode_off;

  /* flags */
  ot->flag = OPTYPE_REGISTER /*|OPTYPE_UNDO*/;

  /* properties */
  WM_operator_properties_select_all(ot);
}

/* ******************** Box Select Operator **************************** */
/**
 * This operator currently works in one of three ways:
 * - BKEY     - 1: all strips within region are selected #NLAEDIT_BOX_ALLSTRIPS.
 * - ALT-BKEY - depending on which axis of the region was larger.
 *   - 2: x-axis, so select all frames within frame range #NLAEDIT_BOXSEL_FRAMERANGE.
 *   - 3: y-axis, so select all frames within channels that region included
 *     #NLAEDIT_BOXSEL_CHANNELS.
 */

/* defines for box_select mode */
enum {
  NLA_BOXSEL_ALLSTRIPS = 0,
  NLA_BOXSEL_FRAMERANGE,
  NLA_BOXSEL_CHANNELS,
} /* eNLAEDIT_BoxSelect_Mode */;

static void box_select_nla_strips(bAnimContext *ac, rcti rect, short mode, short selectmode)
{
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;

  SpaceNla *snla = (SpaceNla *)ac->sl;
  View2D *v2d = &ac->region->v2d;
  rctf rectf;

  /* convert border-region to view coordinates */
  UI_view2d_region_to_view(v2d, rect.xmin, rect.ymin + 2, &rectf.xmin, &rectf.ymin);
  UI_view2d_region_to_view(v2d, rect.xmax, rect.ymax - 2, &rectf.xmax, &rectf.ymax);

  /* filter data */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE | ANIMFILTER_LIST_CHANNELS |
            ANIMFILTER_FCURVESONLY);
  ANIM_animdata_filter(ac, &anim_data, filter, ac->data, ac->datatype);

  /* convert selection modes to selection modes */
  selectmode = selmodes_to_flagmodes(selectmode);

  /* loop over data, doing box select */
  float ymax = NLACHANNEL_FIRST_TOP(ac);
  for (ale = anim_data.first; ale; ale = ale->next, ymax -= NLACHANNEL_STEP(snla)) {
    float ymin = ymax - NLACHANNEL_HEIGHT(snla);

    /* perform vertical suitability check (if applicable) */
    if ((mode == NLA_BOXSEL_FRAMERANGE) || !((ymax < rectf.ymin) || (ymin > rectf.ymax))) {
      /* loop over data selecting (only if NLA-Track) */
      if (ale->type == ANIMTYPE_NLATRACK) {
        NlaTrack *nlt = (NlaTrack *)ale->data;
        NlaStrip *strip;

        /* only select strips if they fall within the required ranges (if applicable) */
        for (strip = nlt->strips.first; strip; strip = strip->next) {
          if ((mode == NLA_BOXSEL_CHANNELS) ||
              BKE_nlastrip_within_bounds(strip, rectf.xmin, rectf.xmax)) {
            /* set selection */
            ACHANNEL_SET_FLAG(strip, selectmode, NLASTRIP_FLAG_SELECT);

            /* clear active flag */
            strip->flag &= ~NLASTRIP_FLAG_ACTIVE;
          }
        }
      }
    }
  }

  /* cleanup */
  ANIM_animdata_freelist(&anim_data);
}

/* ------------------- */

static void nlaedit_strip_at_region_position(
    bAnimContext *ac, float region_x, float region_y, bAnimListElem **r_ale, NlaStrip **r_strip)
{
  *r_ale = NULL;
  *r_strip = NULL;

  SpaceNla *snla = (SpaceNla *)ac->sl;
  View2D *v2d = &ac->region->v2d;

  float view_x, view_y;
  int channel_index;
  UI_view2d_region_to_view(v2d, region_x, region_y, &view_x, &view_y);
  UI_view2d_listview_view_to_cell(
      0, NLACHANNEL_STEP(snla), 0, NLACHANNEL_FIRST_TOP(ac), view_x, view_y, NULL, &channel_index);

  ListBase anim_data = {NULL, NULL};
  int filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE | ANIMFILTER_LIST_CHANNELS |
                ANIMFILTER_FCURVESONLY);
  ANIM_animdata_filter(ac, &anim_data, filter, ac->data, ac->datatype);

  /* x-range to check is +/- 7 (in screen/region-space) on either side of mouse click
   * (that is the size of keyframe icons, so user should be expecting similar tolerances)
   */
  float xmin = UI_view2d_region_to_view_x(v2d, region_x - 7);
  float xmax = UI_view2d_region_to_view_x(v2d, region_x + 7);

  bAnimListElem *ale = BLI_findlink(&anim_data, channel_index);
  if (ale != NULL) {
    if (ale->type == ANIMTYPE_NLATRACK) {
      NlaTrack *nlt = (NlaTrack *)ale->data;

      LISTBASE_FOREACH (NlaStrip *, strip, &nlt->strips) {
        if (BKE_nlastrip_within_bounds(strip, xmin, xmax)) {
          *r_ale = ale;
          *r_strip = strip;

          BLI_remlink(&anim_data, ale);
        }
      }
    }
  }

  ANIM_animdata_freelist(&anim_data);
}

static bool nlaedit_mouse_is_over_strip(bAnimContext *ac, const int mval[2])
{
  bAnimListElem *ale;
  NlaStrip *strip;
  nlaedit_strip_at_region_position(ac, mval[0], mval[1], &ale, &strip);

  if (ale != NULL) {
    BLI_assert(strip != NULL);
    MEM_freeN(ale);
    return true;
  }
  return false;
}

static int nlaedit_box_select_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  bAnimContext ac;
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  bool tweak = RNA_boolean_get(op->ptr, "tweak");
  if (tweak && nlaedit_mouse_is_over_strip(&ac, event->mval)) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  return WM_gesture_box_invoke(C, op, event);
}

static int nlaedit_box_select_exec(bContext *C, wmOperator *op)
{
  bAnimContext ac;
  rcti rect;
  short mode = 0;

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  const eSelectOp sel_op = RNA_enum_get(op->ptr, "mode");
  const int selectmode = (sel_op != SEL_OP_SUB) ? SELECT_ADD : SELECT_SUBTRACT;
  if (SEL_OP_USE_PRE_DESELECT(sel_op)) {
    deselect_nla_strips(&ac, DESELECT_STRIPS_TEST, SELECT_SUBTRACT);
  }

  /* get settings from operator */
  WM_operator_properties_border_to_rcti(op, &rect);

  /* selection 'mode' depends on whether box_select region only matters on one axis */
  if (RNA_boolean_get(op->ptr, "axis_range")) {
    /* mode depends on which axis of the range is larger to determine which axis to use.
     * - Checking this in region-space is fine,
     *   as it's fundamentally still going to be a different rect size.
     * - The frame-range select option is favored over the channel one (x over y),
     *   as frame-range one is often.
     *   Used for tweaking timing when "blocking", while channels is not that useful.
     */
    if (BLI_rcti_size_x(&rect) >= BLI_rcti_size_y(&rect)) {
      mode = NLA_BOXSEL_FRAMERANGE;
    }
    else {
      mode = NLA_BOXSEL_CHANNELS;
    }
  }
  else {
    mode = NLA_BOXSEL_ALLSTRIPS;
  }

  /* apply box_select action */
  box_select_nla_strips(&ac, rect, mode, selectmode);

  /* set notifier that things have changed */
  WM_event_add_notifier(C, NC_ANIMATION | ND_NLA | NA_SELECTED, NULL);

  return OPERATOR_FINISHED;
}

void NLA_OT_select_box(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Box Select";
  ot->idname = "NLA_OT_select_box";
  ot->description = "Use box selection to grab NLA-Strips";

  /* api callbacks */
  ot->invoke = nlaedit_box_select_invoke;
  ot->exec = nlaedit_box_select_exec;
  ot->modal = WM_gesture_box_modal;
  ot->cancel = WM_gesture_box_cancel;

  ot->poll = nlaop_poll_tweakmode_off;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna, "axis_range", 0, "Axis Range", "");

  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "tweak", 0, "Tweak", "Operator has been activated using a click-drag event");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  WM_operator_properties_gesture_box(ot);
  WM_operator_properties_select_operation_simple(ot);
}

/* ******************** Select Left/Right Operator ************************* */
/* Select keyframes left/right of the current frame indicator */

/* defines for left-right select tool */
static const EnumPropertyItem prop_nlaedit_leftright_select_types[] = {
    {NLAEDIT_LRSEL_TEST, "CHECK", 0, "Check if Select Left or Right", ""},
    {NLAEDIT_LRSEL_LEFT, "LEFT", 0, "Before Current Frame", ""},
    {NLAEDIT_LRSEL_RIGHT, "RIGHT", 0, "After Current Frame", ""},
    {0, NULL, 0, NULL, NULL},
};

/* ------------------- */

static void nlaedit_select_leftright(bContext *C,
                                     bAnimContext *ac,
                                     short leftright,
                                     short select_mode)
{
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;

  Scene *scene = ac->scene;
  float xmin, xmax;

  /* if currently in tweak-mode, exit tweak-mode first */
  if (scene->flag & SCE_NLA_EDIT_ON) {
    WM_operator_name_call(C, "NLA_OT_tweakmode_exit", WM_OP_EXEC_DEFAULT, NULL, NULL);
  }

  /* if select mode is replace, deselect all keyframes (and channels) first */
  if (select_mode == SELECT_REPLACE) {
    select_mode = SELECT_ADD;

    /* - deselect all other keyframes, so that just the newly selected remain
     * - channels aren't deselected, since we don't re-select any as a consequence
     */
    deselect_nla_strips(ac, 0, SELECT_SUBTRACT);
  }

  /* get range, and get the right flag-setting mode */
  if (leftright == NLAEDIT_LRSEL_LEFT) {
    xmin = MINAFRAMEF;
    xmax = (float)(scene->r.cfra + 0.1f);
  }
  else {
    xmin = (float)(scene->r.cfra - 0.1f);
    xmax = MAXFRAMEF;
  }

  select_mode = selmodes_to_flagmodes(select_mode);

  /* filter data */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE | ANIMFILTER_FCURVESONLY);
  ANIM_animdata_filter(ac, &anim_data, filter, ac->data, ac->datatype);

  /* select strips on the side where most data occurs */
  for (ale = anim_data.first; ale; ale = ale->next) {
    NlaTrack *nlt = (NlaTrack *)ale->data;
    NlaStrip *strip;

    /* check each strip to see if it is appropriate */
    for (strip = nlt->strips.first; strip; strip = strip->next) {
      if (BKE_nlastrip_within_bounds(strip, xmin, xmax)) {
        ACHANNEL_SET_FLAG(strip, select_mode, NLASTRIP_FLAG_SELECT);
      }
    }
  }

  /* Cleanup */
  ANIM_animdata_freelist(&anim_data);
}

/* ------------------- */

static int nlaedit_select_leftright_exec(bContext *C, wmOperator *op)
{
  bAnimContext ac;
  short leftright = RNA_enum_get(op->ptr, "mode");
  short selectmode;

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* select mode is either replace (deselect all, then add) or add/extend */
  if (RNA_boolean_get(op->ptr, "extend")) {
    selectmode = SELECT_INVERT;
  }
  else {
    selectmode = SELECT_REPLACE;
  }

  /* if "test" mode is set, we don't have any info to set this with */
  if (leftright == NLAEDIT_LRSEL_TEST) {
    return OPERATOR_CANCELLED;
  }

  /* do the selecting now */
  nlaedit_select_leftright(C, &ac, leftright, selectmode);

  /* set notifier that keyframe selection (and channels too) have changed */
  WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME | NA_SELECTED, NULL);
  WM_event_add_notifier(C, NC_ANIMATION | ND_ANIMCHAN | NA_SELECTED, NULL);

  return OPERATOR_FINISHED;
}

static int nlaedit_select_leftright_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  bAnimContext ac;
  short leftright = RNA_enum_get(op->ptr, "mode");

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* handle mode-based testing */
  if (leftright == NLAEDIT_LRSEL_TEST) {
    Scene *scene = ac.scene;
    ARegion *region = ac.region;
    View2D *v2d = &region->v2d;
    float x;

    /* determine which side of the current frame mouse is on */
    x = UI_view2d_region_to_view_x(v2d, event->mval[0]);
    if (x < scene->r.cfra) {
      RNA_enum_set(op->ptr, "mode", NLAEDIT_LRSEL_LEFT);
    }
    else {
      RNA_enum_set(op->ptr, "mode", NLAEDIT_LRSEL_RIGHT);
    }
  }

  /* perform selection */
  return nlaedit_select_leftright_exec(C, op);
}

void NLA_OT_select_leftright(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select Left/Right";
  ot->idname = "NLA_OT_select_leftright";
  ot->description = "Select strips to the left or the right of the current frame";

  /* api callbacks */
  ot->invoke = nlaedit_select_leftright_invoke;
  ot->exec = nlaedit_select_leftright_exec;
  ot->poll = ED_operator_nla_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(
      ot->srna, "mode", prop_nlaedit_leftright_select_types, NLAEDIT_LRSEL_TEST, "Mode", "");
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna, "extend", 0, "Extend Select", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* ******************** Mouse-Click Select Operator *********************** */

/* select strip directly under mouse */
static int mouse_nla_strips(bContext *C,
                            bAnimContext *ac,
                            const int mval[2],
                            short select_mode,
                            const bool deselect_all,
                            bool wait_to_deselect_others)
{
  Scene *scene = ac->scene;

  bAnimListElem *ale = NULL;
  NlaStrip *strip = NULL;
  int ret_value = OPERATOR_FINISHED;

  nlaedit_strip_at_region_position(ac, mval[0], mval[1], &ale, &strip);

  /* if currently in tweak-mode, exit tweak-mode before changing selection states
   * now that we've found our target...
   */
  if (scene->flag & SCE_NLA_EDIT_ON) {
    WM_operator_name_call(C, "NLA_OT_tweakmode_exit", WM_OP_EXEC_DEFAULT, NULL, NULL);
  }

  if (select_mode != SELECT_REPLACE) {
    wait_to_deselect_others = false;
  }

  /* For replacing selection, if we have something to select, we have to clear existing selection.
   * The same goes if we found nothing to select, and deselect_all is true
   * (deselect on nothing behavior). */
  if ((strip != NULL && select_mode == SELECT_REPLACE) || (strip == NULL && deselect_all)) {
    /* reset selection mode for next steps */
    select_mode = SELECT_ADD;

    if (strip && wait_to_deselect_others && (strip->flag & DESELECT_STRIPS_CLEARACTIVE)) {
      ret_value = OPERATOR_RUNNING_MODAL;
    }
    else {
      /* deselect all strips */
      deselect_nla_strips(ac, 0, SELECT_SUBTRACT);

      /* deselect all other channels first */
      ANIM_anim_channels_select_set(ac, ACHANNEL_SETFLAG_CLEAR);
    }
  }

  /* only select strip if we clicked on a valid channel and hit something */
  if (ale != NULL) {
    /* select the strip accordingly (if a matching one was found) */
    if (strip != NULL) {
      select_mode = selmodes_to_flagmodes(select_mode);
      ACHANNEL_SET_FLAG(strip, select_mode, NLASTRIP_FLAG_SELECT);

      /* if we selected it, we can make it active too
       * - we always need to clear the active strip flag though...
       * - as well as selecting its track...
       */
      deselect_nla_strips(ac, DESELECT_STRIPS_CLEARACTIVE, 0);

      if (strip->flag & NLASTRIP_FLAG_SELECT) {
        strip->flag |= NLASTRIP_FLAG_ACTIVE;

        /* Highlight NLA-Track */
        if (ale->type == ANIMTYPE_NLATRACK) {
          NlaTrack *nlt = (NlaTrack *)ale->data;

          nlt->flag |= NLATRACK_SELECTED;
          int filter = ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE |
                       ANIMFILTER_LIST_CHANNELS;
          ANIM_set_active_channel(ac, ac->data, ac->datatype, filter, nlt, ANIMTYPE_NLATRACK);
        }
      }
    }

    /* free this channel */
    MEM_freeN(ale);
  }

  return ret_value;
}

/* ------------------- */

/* handle clicking */
static int nlaedit_clickselect_exec(bContext *C, wmOperator *op)
{
  bAnimContext ac;
  int ret_value;

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* select mode is either replace (deselect all, then add) or add/extend */
  const short selectmode = RNA_boolean_get(op->ptr, "extend") ? SELECT_INVERT : SELECT_REPLACE;
  const bool deselect_all = RNA_boolean_get(op->ptr, "deselect_all");
  const bool wait_to_deselect_others = RNA_boolean_get(op->ptr, "wait_to_deselect_others");
  int mval[2];
  mval[0] = RNA_int_get(op->ptr, "mouse_x");
  mval[1] = RNA_int_get(op->ptr, "mouse_y");

  /* select strips based upon mouse position */
  ret_value = mouse_nla_strips(C, &ac, mval, selectmode, deselect_all, wait_to_deselect_others);

  /* set notifier that things have changed */
  WM_event_add_notifier(C, NC_ANIMATION | ND_NLA | NA_SELECTED, NULL);

  /* for tweak grab to work */
  return ret_value | OPERATOR_PASS_THROUGH;
}

void NLA_OT_click_select(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select";
  ot->idname = "NLA_OT_click_select";
  ot->description = "Handle clicks to select NLA Strips";

  /* callbacks */
  ot->poll = ED_operator_nla_active;
  ot->exec = nlaedit_clickselect_exec;
  ot->invoke = WM_generic_select_invoke;
  ot->modal = WM_generic_select_modal;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_generic_select(ot);
  prop = RNA_def_boolean(ot->srna, "extend", 0, "Extend Select", ""); /* SHIFTKEY */
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "deselect_all",
                         false,
                         "Deselect On Nothing",
                         "Deselect all when nothing under the cursor");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* *********************************************** */

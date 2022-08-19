/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation, Joshua Leung. All rights reserved. */

/** \file
 * \ingroup spnla
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"

#include "BKE_anim_data.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_nla.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "ED_anim_api.h"
#include "ED_keyframes_edit.h"
#include "ED_object.h"
#include "ED_screen.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_interface.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "UI_view2d.h"

#include "nla_intern.h" /* own include */

/* *********************************************** */
/* Operators for NLA channels-list which need to be different
 * from the standard Animation Editor ones */

/* ******************** Mouse-Click Operator *********************** */
/* Depending on the channel that was clicked on, the mouse click will activate whichever
 * part of the channel is relevant.
 *
 * NOTE: eventually,
 * this should probably be phased out when many of these things are replaced with buttons
 * --> Most channels are now selection only.
 */

static int mouse_nla_channels(bContext *C, bAnimContext *ac, int channel_index, short selectmode)
{
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;

  int notifierFlags = 0;

  /* get the channel that was clicked on */
  /* filter channels */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE | ANIMFILTER_LIST_CHANNELS |
            ANIMFILTER_FCURVESONLY);
  ANIM_animdata_filter(ac, &anim_data, filter, ac->data, ac->datatype);

  /* get channel from index */
  ale = BLI_findlink(&anim_data, channel_index);
  if (ale == NULL) {
    /* channel not found */
    if (G.debug & G_DEBUG) {
      printf("Error: animation channel (index = %d) not found in mouse_anim_channels()\n",
             channel_index);
    }

    ANIM_animdata_freelist(&anim_data);
    return 0;
  }

  /* action to take depends on what channel we've got */
  /* WARNING: must keep this in sync with the equivalent function in anim_channels_edit.c */
  switch (ale->type) {
    case ANIMTYPE_SCENE: {
      Scene *sce = (Scene *)ale->data;
      AnimData *adt = sce->adt;

      /* set selection status */
      if (selectmode == SELECT_INVERT) {
        /* swap select */
        sce->flag ^= SCE_DS_SELECTED;
        if (adt) {
          adt->flag ^= ADT_UI_SELECTED;
        }
      }
      else {
        sce->flag |= SCE_DS_SELECTED;
        if (adt) {
          adt->flag |= ADT_UI_SELECTED;
        }
      }

      notifierFlags |= (ND_ANIMCHAN | NA_SELECTED);
      break;
    }
    case ANIMTYPE_OBJECT: {
      ViewLayer *view_layer = ac->view_layer;
      Base *base = (Base *)ale->data;
      Object *ob = base->object;
      AnimData *adt = ob->adt;

      if (nlaedit_is_tweakmode_on(ac) == 0 && (base->flag & BASE_SELECTABLE)) {
        /* set selection status */
        if (selectmode == SELECT_INVERT) {
          /* swap select */
          ED_object_base_select(base, BA_INVERT);

          if (adt) {
            adt->flag ^= ADT_UI_SELECTED;
          }
        }
        else {
          /* deselect all */
          /* TODO: should this deselect all other types of channels too? */
          LISTBASE_FOREACH (Base *, b, &view_layer->object_bases) {
            ED_object_base_select(b, BA_DESELECT);
            if (b->object->adt) {
              b->object->adt->flag &= ~(ADT_UI_SELECTED | ADT_UI_ACTIVE);
            }
          }

          /* select object now */
          ED_object_base_select(base, BA_SELECT);
          if (adt) {
            adt->flag |= ADT_UI_SELECTED;
          }
        }

        /* change active object - regardless of whether it is now selected [T37883] */
        ED_object_base_activate_with_mode_exit_if_needed(C, base); /* adds notifier */

        if ((adt) && (adt->flag & ADT_UI_SELECTED)) {
          adt->flag |= ADT_UI_ACTIVE;
        }

        /* notifiers - channel was selected */
        notifierFlags |= (ND_ANIMCHAN | NA_SELECTED);
      }
      break;
    }
    case ANIMTYPE_FILLACTD: /* Action Expander */
    case ANIMTYPE_DSMAT:    /* Datablock AnimData Expanders */
    case ANIMTYPE_DSLAM:
    case ANIMTYPE_DSCAM:
    case ANIMTYPE_DSCACHEFILE:
    case ANIMTYPE_DSCUR:
    case ANIMTYPE_DSSKEY:
    case ANIMTYPE_DSWOR:
    case ANIMTYPE_DSNTREE:
    case ANIMTYPE_DSPART:
    case ANIMTYPE_DSMBALL:
    case ANIMTYPE_DSARM:
    case ANIMTYPE_DSMESH:
    case ANIMTYPE_DSTEX:
    case ANIMTYPE_DSLAT:
    case ANIMTYPE_DSLINESTYLE:
    case ANIMTYPE_DSSPK:
    case ANIMTYPE_DSGPENCIL:
    case ANIMTYPE_PALETTE:
    case ANIMTYPE_DSHAIR:
    case ANIMTYPE_DSPOINTCLOUD:
    case ANIMTYPE_DSVOLUME:
    case ANIMTYPE_DSSIMULATION: {
      /* sanity checking... */
      if (ale->adt) {
        /* select/deselect */
        if (selectmode == SELECT_INVERT) {
          /* inverse selection status of this AnimData block only */
          ale->adt->flag ^= ADT_UI_SELECTED;
        }
        else {
          /* select AnimData block by itself */
          ANIM_anim_channels_select_set(ac, ACHANNEL_SETFLAG_CLEAR);
          ale->adt->flag |= ADT_UI_SELECTED;
        }

        /* set active? */
        if ((ale->adt) && (ale->adt->flag & ADT_UI_SELECTED)) {
          ale->adt->flag |= ADT_UI_ACTIVE;
        }
      }

      notifierFlags |= (ND_ANIMCHAN | NA_SELECTED);
      break;
    }
    case ANIMTYPE_NLATRACK: {
      NlaTrack *nlt = (NlaTrack *)ale->data;

      if (nlaedit_is_tweakmode_on(ac) == 0) {
        /* set selection */
        if (selectmode == SELECT_INVERT) {
          /* inverse selection status of this F-Curve only */
          nlt->flag ^= NLATRACK_SELECTED;
        }
        else {
          /* select F-Curve by itself */
          ANIM_anim_channels_select_set(ac, ACHANNEL_SETFLAG_CLEAR);
          nlt->flag |= NLATRACK_SELECTED;
        }

        /* if NLA-Track is selected now,
         * make NLA-Track the 'active' one in the visible list */
        if (nlt->flag & NLATRACK_SELECTED) {
          ANIM_set_active_channel(ac, ac->data, ac->datatype, filter, nlt, ANIMTYPE_NLATRACK);
        }

        /* notifier flags - channel was selected */
        notifierFlags |= (ND_ANIMCHAN | NA_SELECTED);
      }
      break;
    }
    case ANIMTYPE_NLAACTION: {
      AnimData *adt = BKE_animdata_from_id(ale->id);

      /* NOTE: rest of NLA-Action name doubles for operating on the AnimData block
       * - this is useful when there's no clear divider, and makes more sense in
       *   the case of users trying to use this to change actions
       * - in tweak-mode, clicking here gets us out of tweak-mode, as changing selection
       *   while in tweak-mode is really evil!
       * - we disable "solo" flags too, to make it easier to work with stashed actions
       *   with less trouble
       */
      if (nlaedit_is_tweakmode_on(ac)) {
        /* Exit tweak-mode immediately. */
        nlaedit_disable_tweakmode(ac, true);

        /* changes to NLA-Action occurred */
        notifierFlags |= ND_NLA_ACTCHANGE;
        ale->update |= ANIM_UPDATE_DEPS;
      }
      else {
        /* select/deselect */
        if (selectmode == SELECT_INVERT) {
          /* inverse selection status of this AnimData block only */
          adt->flag ^= ADT_UI_SELECTED;
        }
        else {
          /* select AnimData block by itself */
          ANIM_anim_channels_select_set(ac, ACHANNEL_SETFLAG_CLEAR);
          adt->flag |= ADT_UI_SELECTED;
        }

        /* set active? */
        if (adt->flag & ADT_UI_SELECTED) {
          adt->flag |= ADT_UI_ACTIVE;
        }

        notifierFlags |= (ND_ANIMCHAN | NA_SELECTED);
      }
      break;
    }
    default:
      if (G.debug & G_DEBUG) {
        printf("Error: Invalid channel type in mouse_nla_channels()\n");
      }
      break;
  }

  /* free channels */
  ANIM_animdata_update(ac, &anim_data);
  ANIM_animdata_freelist(&anim_data);

  /* return the notifier-flags set */
  return notifierFlags;
}

/* ------------------- */

/* handle clicking */
static int nlachannels_mouseclick_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  bAnimContext ac;
  SpaceNla *snla;
  ARegion *region;
  View2D *v2d;
  int channel_index;
  int notifierFlags = 0;
  short selectmode;
  float x, y;

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* get useful pointers from animation context data */
  snla = (SpaceNla *)ac.sl;
  region = ac.region;
  v2d = &region->v2d;

  /* select mode is either replace (deselect all, then add) or add/extend */
  if (RNA_boolean_get(op->ptr, "extend")) {
    selectmode = SELECT_INVERT;
  }
  else {
    selectmode = SELECT_REPLACE;
  }

  /* Figure out which channel user clicked in. */
  UI_view2d_region_to_view(v2d, event->mval[0], event->mval[1], &x, &y);
  UI_view2d_listview_view_to_cell(NLACHANNEL_NAMEWIDTH,
                                  NLACHANNEL_STEP(snla),
                                  0,
                                  NLACHANNEL_FIRST_TOP(&ac),
                                  x,
                                  y,
                                  NULL,
                                  &channel_index);

  /* handle mouse-click in the relevant channel then */
  notifierFlags = mouse_nla_channels(C, &ac, channel_index, selectmode);

  /* set notifier that things have changed */
  WM_event_add_notifier(C, NC_ANIMATION | notifierFlags, NULL);

  return OPERATOR_FINISHED;
}

void NLA_OT_channels_click(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Mouse Click on NLA Channels";
  ot->idname = "NLA_OT_channels_click";
  ot->description = "Handle clicks to select NLA channels";

  /* api callbacks */
  ot->invoke = nlachannels_mouseclick_invoke;
  ot->poll = ED_operator_nla_active;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* props */
  prop = RNA_def_boolean(ot->srna, "extend", 0, "Extend Select", ""); /* SHIFTKEY */
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* *********************************************** */
/* Special Operators */

/* ******************** Action Push Down ******************************** */

static int nlachannels_pushdown_exec(bContext *C, wmOperator *op)
{
  bAnimContext ac;
  ID *id = NULL;
  AnimData *adt = NULL;
  int channel_index = RNA_int_get(op->ptr, "channel_index");

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* get anim-channel to use (or more specifically, the animdata block behind it) */
  if (channel_index == -1) {
    PointerRNA adt_ptr = {NULL};

    /* active animdata block */
    if (nla_panel_context(C, &adt_ptr, NULL, NULL) == 0 || (adt_ptr.data == NULL)) {
      BKE_report(op->reports,
                 RPT_ERROR,
                 "No active AnimData block to use "
                 "(select a data-block expander first or set the appropriate flags on an AnimData "
                 "block)");
      return OPERATOR_CANCELLED;
    }

    id = adt_ptr.owner_id;
    adt = adt_ptr.data;
  }
  else {
    /* indexed channel */
    ListBase anim_data = {NULL, NULL};
    bAnimListElem *ale;
    int filter;

    /* filter channels */
    filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE | ANIMFILTER_LIST_CHANNELS |
              ANIMFILTER_FCURVESONLY);
    ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

    /* get channel from index */
    ale = BLI_findlink(&anim_data, channel_index);
    if (ale == NULL) {
      BKE_reportf(op->reports, RPT_ERROR, "No animation channel found at index %d", channel_index);
      ANIM_animdata_freelist(&anim_data);
      return OPERATOR_CANCELLED;
    }
    if (ale->type != ANIMTYPE_NLAACTION) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "Animation channel at index %d is not a NLA 'Active Action' channel",
                  channel_index);
      ANIM_animdata_freelist(&anim_data);
      return OPERATOR_CANCELLED;
    }

    /* grab AnimData from the channel */
    adt = ale->adt;
    id = ale->id;

    /* we don't need anything here anymore, so free it all */
    ANIM_animdata_freelist(&anim_data);
  }

  /* double-check that we are free to push down here... */
  if (adt == NULL) {
    BKE_report(op->reports, RPT_WARNING, "Internal Error - AnimData block is not valid");
    return OPERATOR_CANCELLED;
  }
  if (nlaedit_is_tweakmode_on(&ac)) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Cannot push down actions while tweaking a strip's action, exit tweak mode first");
    return OPERATOR_CANCELLED;
  }
  if (adt->action == NULL) {
    BKE_report(op->reports, RPT_WARNING, "No active action to push down");
    return OPERATOR_CANCELLED;
  }

  /* 'push-down' action - only usable when not in Tweak-mode. */
  BKE_nla_action_pushdown(adt, ID_IS_OVERRIDE_LIBRARY(id));

  struct Main *bmain = CTX_data_main(C);
  DEG_id_tag_update_ex(bmain, id, ID_RECALC_ANIMATION);

  /* The action needs updating too, as FCurve modifiers are to be reevaluated. They won't extend
   * beyond the NLA strip after pushing down to the NLA. */
  DEG_id_tag_update_ex(bmain, &adt->action->id, ID_RECALC_ANIMATION);

  /* set notifier that things have changed */
  WM_event_add_notifier(C, NC_ANIMATION | ND_NLA_ACTCHANGE, NULL);
  return OPERATOR_FINISHED;
}

void NLA_OT_action_pushdown(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Push Down Action";
  ot->idname = "NLA_OT_action_pushdown";
  ot->description = "Push action down onto the top of the NLA stack as a new strip";

  /* callbacks */
  ot->exec = nlachannels_pushdown_exec;
  ot->poll = nlaop_poll_tweakmode_off;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_int(ot->srna,
                         "channel_index",
                         -1,
                         -1,
                         INT_MAX,
                         "Channel Index",
                         "Index of NLA action channel to perform pushdown operation on",
                         0,
                         INT_MAX);
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE);
}

/* ******************** Action Unlink ******************************** */

static bool nla_action_unlink_poll(bContext *C)
{
  if (ED_operator_nla_active(C)) {
    PointerRNA adt_ptr;
    return (nla_panel_context(C, &adt_ptr, NULL, NULL) && (adt_ptr.data != NULL));
  }

  /* something failed... */
  return false;
}

static int nla_action_unlink_exec(bContext *C, wmOperator *op)
{
  PointerRNA adt_ptr;
  AnimData *adt;

  /* check context and also validity of pointer */
  if (!nla_panel_context(C, &adt_ptr, NULL, NULL)) {
    return OPERATOR_CANCELLED;
  }

  /* get animdata */
  adt = adt_ptr.data;
  if (adt == NULL) {
    return OPERATOR_CANCELLED;
  }

  /* do unlinking */
  if (adt->action) {
    bool force_delete = RNA_boolean_get(op->ptr, "force_delete");
    ED_animedit_unlink_action(C, adt_ptr.owner_id, adt, adt->action, op->reports, force_delete);
  }

  return OPERATOR_FINISHED;
}

static int nla_action_unlink_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* NOTE: this is hardcoded to match the behavior for the unlink button
   * (in interface_templates.c) */
  RNA_boolean_set(op->ptr, "force_delete", event->modifier & KM_SHIFT);
  return nla_action_unlink_exec(C, op);
}

void NLA_OT_action_unlink(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Unlink Action";
  ot->idname = "NLA_OT_action_unlink";
  ot->description = "Unlink this action from the active action slot (and/or exit Tweak Mode)";

  /* callbacks */
  ot->invoke = nla_action_unlink_invoke;
  ot->exec = nla_action_unlink_exec;
  ot->poll = nla_action_unlink_poll;

  /* properties */
  prop = RNA_def_boolean(ot->srna,
                         "force_delete",
                         false,
                         "Force Delete",
                         "Clear Fake User and remove copy stashed in this data-block's NLA stack");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* ******************** Add Tracks Operator ***************************** */
/* Add NLA Tracks to the same AnimData block as a selected track, or above the selected tracks */

bool nlaedit_add_tracks_existing(bAnimContext *ac, bool above_sel)
{
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;
  AnimData *lastAdt = NULL;
  bool added = false;

  /* get a list of the (selected) NLA Tracks being shown in the NLA */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE | ANIMFILTER_SEL |
            ANIMFILTER_NODUPLIS | ANIMFILTER_FCURVESONLY);
  ANIM_animdata_filter(ac, &anim_data, filter, ac->data, ac->datatype);

  /* add tracks... */
  for (ale = anim_data.first; ale; ale = ale->next) {
    if (ale->type == ANIMTYPE_NLATRACK) {
      NlaTrack *nlt = (NlaTrack *)ale->data;
      AnimData *adt = ale->adt;

      const bool is_liboverride = ID_IS_OVERRIDE_LIBRARY(ale->id);

      /* check if just adding a new track above this one,
       * or whether we're adding a new one to the top of the stack that this one belongs to
       */
      if (above_sel) {
        /* just add a new one above this one */
        BKE_nlatrack_add(adt, nlt, is_liboverride);
        ale->update = ANIM_UPDATE_DEPS;
        added = true;
      }
      else if ((lastAdt == NULL) || (adt != lastAdt)) {
        /* add one track to the top of the owning AnimData's stack,
         * then don't add anymore to this stack */
        BKE_nlatrack_add(adt, NULL, is_liboverride);
        lastAdt = adt;
        ale->update = ANIM_UPDATE_DEPS;
        added = true;
      }
    }
  }

  /* free temp data */
  ANIM_animdata_update(ac, &anim_data);
  ANIM_animdata_freelist(&anim_data);

  return added;
}

bool nlaedit_add_tracks_empty(bAnimContext *ac)
{
  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;
  bool added = false;

  /* get a list of the selected AnimData blocks in the NLA */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE | ANIMFILTER_ANIMDATA |
            ANIMFILTER_SEL | ANIMFILTER_NODUPLIS | ANIMFILTER_FCURVESONLY);
  ANIM_animdata_filter(ac, &anim_data, filter, ac->data, ac->datatype);

  /* check if selected AnimData blocks are empty, and add tracks if so... */
  for (ale = anim_data.first; ale; ale = ale->next) {
    AnimData *adt = ale->adt;

    /* sanity check */
    BLI_assert(adt->flag & ADT_UI_SELECTED);

    /* ensure it is empty */
    if (BLI_listbase_is_empty(&adt->nla_tracks)) {
      /* add new track to this AnimData block then */
      BKE_nlatrack_add(adt, NULL, ID_IS_OVERRIDE_LIBRARY(ale->id));
      ale->update = ANIM_UPDATE_DEPS;
      added = true;
    }
  }

  /* cleanup */
  ANIM_animdata_update(ac, &anim_data);
  ANIM_animdata_freelist(&anim_data);

  return added;
}

/* ----- */

static int nlaedit_add_tracks_exec(bContext *C, wmOperator *op)
{
  bAnimContext ac;
  bool above_sel = RNA_boolean_get(op->ptr, "above_selected");
  bool op_done = false;

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* perform adding in two passes - existing first so that we don't double up for empty */
  op_done |= nlaedit_add_tracks_existing(&ac, above_sel);
  op_done |= nlaedit_add_tracks_empty(&ac);

  /* done? */
  if (op_done) {
    DEG_relations_tag_update(CTX_data_main(C));

    /* set notifier that things have changed */
    WM_event_add_notifier(C, NC_ANIMATION | ND_NLA | NA_ADDED, NULL);

    /* done */
    return OPERATOR_FINISHED;
  }

  /* failed to add any tracks */
  BKE_report(
      op->reports, RPT_WARNING, "Select an existing NLA Track or an empty action line first");

  /* not done */
  return OPERATOR_CANCELLED;
}

void NLA_OT_tracks_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Tracks";
  ot->idname = "NLA_OT_tracks_add";
  ot->description = "Add NLA-Tracks above/after the selected tracks";

  /* api callbacks */
  ot->exec = nlaedit_add_tracks_exec;
  ot->poll = nlaop_poll_tweakmode_off;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna,
                  "above_selected",
                  0,
                  "Above Selected",
                  "Add a new NLA Track above every existing selected one");
}

/* ******************** Delete Tracks Operator ***************************** */
/* Delete selected NLA Tracks */

static int nlaedit_delete_tracks_exec(bContext *C, wmOperator *UNUSED(op))
{
  bAnimContext ac;

  ListBase anim_data = {NULL, NULL};
  bAnimListElem *ale;
  int filter;

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* get a list of the AnimData blocks being shown in the NLA */
  filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_LIST_VISIBLE | ANIMFILTER_SEL |
            ANIMFILTER_NODUPLIS | ANIMFILTER_FCURVESONLY);
  ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

  /* delete tracks */
  for (ale = anim_data.first; ale; ale = ale->next) {
    if (ale->type == ANIMTYPE_NLATRACK) {
      NlaTrack *nlt = (NlaTrack *)ale->data;
      AnimData *adt = ale->adt;

      if (BKE_nlatrack_is_nonlocal_in_liboverride(ale->id, nlt)) {
        /* No deletion of non-local tracks of override data. */
        continue;
      }

      /* if track is currently 'solo', then AnimData should have its
       * 'has solo' flag disabled
       */
      if (nlt->flag & NLATRACK_SOLO) {
        adt->flag &= ~ADT_NLA_SOLO_TRACK;
      }

      /* call delete on this track - deletes all strips too */
      BKE_nlatrack_free(&adt->nla_tracks, nlt, true);
      ale->update = ANIM_UPDATE_DEPS;
    }
  }

  /* free temp data */
  ANIM_animdata_update(&ac, &anim_data);
  ANIM_animdata_freelist(&anim_data);

  DEG_relations_tag_update(ac.bmain);

  /* set notifier that things have changed */
  WM_event_add_notifier(C, NC_ANIMATION | ND_NLA | NA_REMOVED, NULL);

  /* done */
  return OPERATOR_FINISHED;
}

void NLA_OT_tracks_delete(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Delete Tracks";
  ot->idname = "NLA_OT_tracks_delete";
  ot->description = "Delete selected NLA-Tracks and the strips they contain";

  /* api callbacks */
  ot->exec = nlaedit_delete_tracks_exec;
  ot->poll = nlaop_poll_tweakmode_off;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* *********************************************** */
/* AnimData Related Operators */

/* ******************** Include Objects Operator ***************************** */
/* Include selected objects in NLA Editor, by giving them AnimData blocks
 * NOTE: This doesn't help for non-object AnimData, where we do not have any effective
 *       selection mechanism in place. Unfortunately, this means that non-object AnimData
 *       once again becomes a second-class citizen here. However, at least for the most
 *       common use case, we now have a nice shortcut again.
 */

static int nlaedit_objects_add_exec(bContext *C, wmOperator *UNUSED(op))
{
  bAnimContext ac;
  SpaceNla *snla;

  /* get editor data */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return OPERATOR_CANCELLED;
  }

  /* ensure that filters are set so that the effect will be immediately visible */
  snla = (SpaceNla *)ac.sl;
  if (snla && snla->ads) {
    snla->ads->filterflag &= ~ADS_FILTER_NLA_NOACT;
  }

  /* operate on selected objects... */
  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    /* ensure that object has AnimData... that's all */
    BKE_animdata_ensure_id(&ob->id);
  }
  CTX_DATA_END;

  /* set notifier that things have changed */
  WM_event_add_notifier(C, NC_ANIMATION | ND_NLA | NA_EDITED, NULL);

  /* done */
  return OPERATOR_FINISHED;
}

void NLA_OT_selected_objects_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Include Selected Objects";
  ot->idname = "NLA_OT_selected_objects_add";
  ot->description = "Make selected objects appear in NLA Editor by adding Animation Data";

  /* api callbacks */
  ot->exec = nlaedit_objects_add_exec;
  ot->poll = nlaop_poll_tweakmode_off;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* *********************************************** */

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 Blender Foundation, Joshua Leung. */

/** \file
 * \ingroup edarmature
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_anim_data.h"
#include "BKE_idprop.h"
#include "BKE_layer.h"
#include "BKE_object.h"

#include "BKE_context.h"

#include "DEG_depsgraph.h"

#include "RNA_access.h"
#include "RNA_path.h"
#include "RNA_prototypes.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_armature.h"
#include "ED_keyframing.h"

#include "armature_intern.h"

/* *********************************************** */
/* Contents of this File:
 *
 * This file contains methods shared between Pose Slide and Pose Lib;
 * primarily the functions in question concern Animato <-> Pose
 * convenience functions, such as applying/getting pose values
 * and/or inserting keyframes for these.
 */
/* *********************************************** */
/* FCurves <-> PoseChannels Links */

/* helper for poseAnim_mapping_get() -> get the relevant F-Curves per PoseChannel */
static void fcurves_to_pchan_links_get(ListBase *pfLinks,
                                       Object *ob,
                                       bAction *act,
                                       bPoseChannel *pchan)
{
  ListBase curves = {NULL, NULL};
  int transFlags = action_get_item_transforms(act, ob, pchan, &curves);

  pchan->flag &= ~(POSE_LOC | POSE_ROT | POSE_SIZE | POSE_BBONE_SHAPE);

  /* check if any transforms found... */
  if (transFlags) {
    /* make new linkage data */
    tPChanFCurveLink *pfl = MEM_callocN(sizeof(tPChanFCurveLink), "tPChanFCurveLink");
    PointerRNA ptr;

    pfl->ob = ob;
    pfl->fcurves = curves;
    pfl->pchan = pchan;

    /* get the RNA path to this pchan - this needs to be freed! */
    RNA_pointer_create((ID *)ob, &RNA_PoseBone, pchan, &ptr);
    pfl->pchan_path = RNA_path_from_ID_to_struct(&ptr);

    /* add linkage data to operator data */
    BLI_addtail(pfLinks, pfl);

    /* set pchan's transform flags */
    if (transFlags & ACT_TRANS_LOC) {
      pchan->flag |= POSE_LOC;
    }
    if (transFlags & ACT_TRANS_ROT) {
      pchan->flag |= POSE_ROT;
    }
    if (transFlags & ACT_TRANS_SCALE) {
      pchan->flag |= POSE_SIZE;
    }
    if (transFlags & ACT_TRANS_BBONE) {
      pchan->flag |= POSE_BBONE_SHAPE;
    }

    /* store current transforms */
    copy_v3_v3(pfl->oldloc, pchan->loc);
    copy_v3_v3(pfl->oldrot, pchan->eul);
    copy_v3_v3(pfl->oldscale, pchan->size);
    copy_qt_qt(pfl->oldquat, pchan->quat);
    copy_v3_v3(pfl->oldaxis, pchan->rotAxis);
    pfl->oldangle = pchan->rotAngle;

    /* store current bbone values */
    pfl->roll1 = pchan->roll1;
    pfl->roll2 = pchan->roll2;
    pfl->curve_in_x = pchan->curve_in_x;
    pfl->curve_in_z = pchan->curve_in_z;
    pfl->curve_out_x = pchan->curve_out_x;
    pfl->curve_out_z = pchan->curve_out_z;
    pfl->ease1 = pchan->ease1;
    pfl->ease2 = pchan->ease2;

    copy_v3_v3(pfl->scale_in, pchan->scale_in);
    copy_v3_v3(pfl->scale_out, pchan->scale_out);

    /* make copy of custom properties */
    if (pchan->prop && (transFlags & ACT_TRANS_PROP)) {
      pfl->oldprops = IDP_CopyProperty(pchan->prop);
    }
  }
}

Object *poseAnim_object_get(Object *ob_)
{
  Object *ob = BKE_object_pose_armature_get(ob_);
  if (!ELEM(NULL, ob, ob->data, ob->adt, ob->adt->action)) {
    return ob;
  }
  return NULL;
}

void poseAnim_mapping_get(bContext *C, ListBase *pfLinks)
{
  /* for each Pose-Channel which gets affected, get the F-Curves for that channel
   * and set the relevant transform flags...
   */
  Object *prev_ob, *ob_pose_armature;

  prev_ob = NULL;
  ob_pose_armature = NULL;
  CTX_DATA_BEGIN_WITH_ID (C, bPoseChannel *, pchan, selected_pose_bones, Object *, ob) {
    if (ob != prev_ob) {
      prev_ob = ob;
      ob_pose_armature = poseAnim_object_get(ob);
    }

    if (ob_pose_armature == NULL) {
      continue;
    }

    fcurves_to_pchan_links_get(pfLinks, ob_pose_armature, ob_pose_armature->adt->action, pchan);
  }
  CTX_DATA_END;

  /* if no PoseChannels were found, try a second pass, doing visible ones instead
   * i.e. if nothing selected, do whole pose
   */
  if (BLI_listbase_is_empty(pfLinks)) {
    prev_ob = NULL;
    ob_pose_armature = NULL;
    CTX_DATA_BEGIN_WITH_ID (C, bPoseChannel *, pchan, visible_pose_bones, Object *, ob) {
      if (ob != prev_ob) {
        prev_ob = ob;
        ob_pose_armature = poseAnim_object_get(ob);
      }

      if (ob_pose_armature == NULL) {
        continue;
      }

      fcurves_to_pchan_links_get(pfLinks, ob_pose_armature, ob_pose_armature->adt->action, pchan);
    }
    CTX_DATA_END;
  }
}

void poseAnim_mapping_free(ListBase *pfLinks)
{
  tPChanFCurveLink *pfl, *pfln = NULL;

  /* free the temp pchan links and their data */
  for (pfl = pfLinks->first; pfl; pfl = pfln) {
    pfln = pfl->next;

    /* free custom properties */
    if (pfl->oldprops) {
      IDP_FreeProperty(pfl->oldprops);
    }

    /* free list of F-Curve reference links */
    BLI_freelistN(&pfl->fcurves);

    /* free pchan RNA Path */
    MEM_freeN(pfl->pchan_path);

    /* free link itself */
    BLI_freelinkN(pfLinks, pfl);
  }
}

/* ------------------------- */

void poseAnim_mapping_refresh(bContext *C, Scene *UNUSED(scene), Object *ob)
{
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);

  AnimData *adt = BKE_animdata_from_id(&ob->id);
  if (adt && adt->action) {
    DEG_id_tag_update(&adt->action->id, ID_RECALC_ANIMATION_NO_FLUSH);
  }
}

void poseAnim_mapping_reset(ListBase *pfLinks)
{
  tPChanFCurveLink *pfl;

  /* iterate over each pose-channel affected, restoring all channels to their original values */
  for (pfl = pfLinks->first; pfl; pfl = pfl->next) {
    bPoseChannel *pchan = pfl->pchan;

    /* just copy all the values over regardless of whether they changed or not */
    copy_v3_v3(pchan->loc, pfl->oldloc);
    copy_v3_v3(pchan->eul, pfl->oldrot);
    copy_v3_v3(pchan->size, pfl->oldscale);
    copy_qt_qt(pchan->quat, pfl->oldquat);
    copy_v3_v3(pchan->rotAxis, pfl->oldaxis);
    pchan->rotAngle = pfl->oldangle;

    /* store current bbone values */
    pchan->roll1 = pfl->roll1;
    pchan->roll2 = pfl->roll2;
    pchan->curve_in_x = pfl->curve_in_x;
    pchan->curve_in_z = pfl->curve_in_z;
    pchan->curve_out_x = pfl->curve_out_x;
    pchan->curve_out_z = pfl->curve_out_z;
    pchan->ease1 = pfl->ease1;
    pchan->ease2 = pfl->ease2;

    copy_v3_v3(pchan->scale_in, pfl->scale_in);
    copy_v3_v3(pchan->scale_out, pfl->scale_out);

    /* just overwrite values of properties from the stored copies (there should be some) */
    if (pfl->oldprops) {
      IDP_SyncGroupValues(pfl->pchan->prop, pfl->oldprops);
    }
  }
}

void poseAnim_mapping_autoKeyframe(bContext *C, Scene *scene, ListBase *pfLinks, float cframe)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  bool skip = true;

  FOREACH_OBJECT_IN_MODE_BEGIN (view_layer, v3d, OB_ARMATURE, OB_MODE_POSE, ob) {
    ob->id.tag &= ~LIB_TAG_DOIT;
    ob = poseAnim_object_get(ob);

    /* Ensure validity of the settings from the context. */
    if (ob == NULL) {
      continue;
    }

    if (autokeyframe_cfra_can_key(scene, &ob->id)) {
      ob->id.tag |= LIB_TAG_DOIT;
      skip = false;
    }
  }
  FOREACH_OBJECT_IN_MODE_END;

  if (skip) {
    return;
  }

  /* Insert keyframes as necessary if auto-key-framing. */
  KeyingSet *ks = ANIM_get_keyingset_for_autokeying(scene, ANIM_KS_WHOLE_CHARACTER_ID);
  ListBase dsources = {NULL, NULL};
  tPChanFCurveLink *pfl;

  /* iterate over each pose-channel affected, tagging bones to be keyed */
  /* XXX: here we already have the information about what transforms exist, though
   * it might be easier to just overwrite all using normal mechanisms
   */
  for (pfl = pfLinks->first; pfl; pfl = pfl->next) {
    bPoseChannel *pchan = pfl->pchan;

    if ((pfl->ob->id.tag & LIB_TAG_DOIT) == 0) {
      continue;
    }

    /* Add data-source override for the PoseChannel, to be used later. */
    ANIM_relative_keyingset_add_source(&dsources, &pfl->ob->id, &RNA_PoseBone, pchan);
  }

  /* insert keyframes for all relevant bones in one go */
  ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, cframe);
  BLI_freelistN(&dsources);

  /* do the bone paths
   * - only do this if keyframes should have been added
   * - do not calculate unless there are paths already to update...
   */
  FOREACH_OBJECT_IN_MODE_BEGIN (view_layer, v3d, OB_ARMATURE, OB_MODE_POSE, ob) {
    if (ob->id.tag & LIB_TAG_DOIT) {
      if (ob->pose->avs.path_bakeflag & MOTIONPATH_BAKE_HAS_PATHS) {
        // ED_pose_clear_paths(C, ob); /* XXX for now, don't need to clear. */
        /* TODO(sergey): Should ensure we can use more narrow update range here. */
        ED_pose_recalculate_paths(C, scene, ob, POSE_PATH_CALC_RANGE_FULL);
      }
    }
  }
  FOREACH_OBJECT_IN_MODE_END;
}

/* ------------------------- */

LinkData *poseAnim_mapping_getNextFCurve(ListBase *fcuLinks, LinkData *prev, const char *path)
{
  LinkData *first = (prev) ? prev->next : (fcuLinks) ? fcuLinks->first : NULL;
  LinkData *ld;

  /* check each link to see if the linked F-Curve has a matching path */
  for (ld = first; ld; ld = ld->next) {
    FCurve *fcu = (FCurve *)ld->data;

    /* check if paths match */
    if (STREQ(path, fcu->rna_path)) {
      return ld;
    }
  }

  /* none found */
  return NULL;
}

/* *********************************************** */

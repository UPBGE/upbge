/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 Blender Foundation. */

/** \file
 * \ingroup edarmature
 */

#include <math.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_dlrbTree.h"
#include "BLI_string_utils.h"

#include "BLT_translation.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_animsys.h"
#include "BKE_armature.h"
#include "BKE_fcurve.h"
#include "BKE_idprop.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_object.h"

#include "BKE_context.h"
#include "BKE_report.h"

#include "DEG_depsgraph.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"
#include "RNA_prototypes.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "ED_anim_api.h"
#include "ED_armature.h"
#include "ED_keyframes_edit.h"
#include "ED_keyframes_keylist.h"
#include "ED_keyframing.h"
#include "ED_object.h"
#include "ED_screen.h"

#include "armature_intern.h"

/* ******* XXX ********** */

static void action_set_activemarker(void *UNUSED(a), void *UNUSED(b), void *UNUSED(c))
{
}

/* ************************************************************* */
/**
 * Pose-Library Tool for Blender
 * =============================
 *
 * Overview:
 *  This tool allows animators to store a set of frequently used poses to dump into
 *  the active action to help in "budget" productions to quickly block out new actions.
 *  It acts as a kind of "glorified clipboard for poses", allowing for naming of poses.
 *
 * Features:
 * - Pose-libs are simply normal Actions.
 * - Each "pose" is simply a set of key-frames that occur on a particular frame.
 *   - A set of #TimeMarker that belong to each Action, help 'label' where a 'pose' can be
 *     found in the Action.
 * - The Scroll-wheel or PageUp/Down buttons when used in a special mode or after pressing/holding
 *   [a modifier] key, cycles through the poses available for the active pose's pose-lib,
 *   allowing the animator to preview what action best suits that pose.
 */
/* ************************************************************* */

/* gets the first available frame in poselib to store a pose on
 * - frames start from 1, and a pose should occur on every frame... 0 is error!
 */
static int poselib_get_free_index(bAction *act)
{
  TimeMarker *marker;
  int low = 0, high = 0;
  bool changed = false;

  /* sanity checks */
  if (ELEM(NULL, act, act->markers.first)) {
    return 1;
  }

  /* As poses are not stored in chronological order, we must iterate over this list
   * a few times until we don't make any new discoveries (mostly about the lower bound).
   * Prevents problems with deleting then trying to add new poses T27412.
   */
  do {
    changed = false;

    for (marker = act->markers.first; marker; marker = marker->next) {
      /* only increase low if value is 1 greater than low, to find "gaps" where
       * poses were removed from the poselib
       */
      if (marker->frame == (low + 1)) {
        low++;
        changed = true;
      }

      /* value replaces high if it is the highest value encountered yet */
      if (marker->frame > high) {
        high = marker->frame;
        changed = true;
      }
    }
  } while (changed != 0);

  /* - if low is not equal to high, then low+1 is a gap
   * - if low is equal to high, then high+1 is the next index (add at end)
   */
  if (low < high) {
    return (low + 1);
  }
  return (high + 1);
}

/* returns the active pose for a poselib */
static TimeMarker *poselib_get_active_pose(bAction *act)
{
  if ((act) && (act->active_marker)) {
    return BLI_findlink(&act->markers, act->active_marker - 1);
  }
  return NULL;
}

/* Get object that Pose Lib should be found on */
/* XXX C can be zero */
static Object *get_poselib_object(bContext *C)
{
  ScrArea *area;

  /* sanity check */
  if (C == NULL) {
    return NULL;
  }

  area = CTX_wm_area(C);

  if (area && (area->spacetype == SPACE_PROPERTIES)) {
    return ED_object_context(C);
  }
  return BKE_object_pose_armature_get(CTX_data_active_object(C));
}

/* Poll callback for operators that require existing PoseLib data (with poses) to work */
static bool has_poselib_pose_data_poll(bContext *C)
{
  Object *ob = get_poselib_object(C);
  return (ob && ob->poselib);
}

/* Poll callback for operators that require existing PoseLib data (with poses)
 * as they need to do some editing work on those poses (i.e. not on lib-linked actions)
 */
static bool has_poselib_pose_data_for_editing_poll(bContext *C)
{
  Object *ob = get_poselib_object(C);
  return (ob && ob->poselib && BKE_id_is_editable(CTX_data_main(C), &ob->poselib->id));
}

/* ----------------------------------- */

/* Initialize a new poselib (whether it is needed or not) */
static bAction *poselib_init_new(Main *bmain, Object *ob)
{
  /* sanity checks - only for armatures */
  if (ELEM(NULL, ob, ob->pose)) {
    return NULL;
  }

  /* init object's poselib action (unlink old one if there) */
  if (ob->poselib) {
    id_us_min(&ob->poselib->id);
  }

  ob->poselib = BKE_action_add(bmain, "PoseLib");
  ob->poselib->idroot = ID_OB;

  return ob->poselib;
}

/* Initialize a new poselib (checks if that needs to happen) */
static bAction *poselib_validate(Main *bmain, Object *ob)
{
  if (ELEM(NULL, ob, ob->pose)) {
    return NULL;
  }
  if (ob->poselib == NULL) {
    return poselib_init_new(bmain, ob);
  }
  return ob->poselib;
}

/* ************************************************************* */
/* Pose Lib UI Operators */

static int poselib_new_exec(bContext *C, wmOperator *UNUSED(op))
{
  Main *bmain = CTX_data_main(C);
  Object *ob = get_poselib_object(C);

  /* sanity checks */
  if (ob == NULL) {
    return OPERATOR_CANCELLED;
  }

  /* new method here deals with the rest... */
  poselib_init_new(bmain, ob);

  /* notifier here might evolve? */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, NULL);

  return OPERATOR_FINISHED;
}

void POSELIB_OT_new(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "New Legacy Pose Library";
  ot->idname = "POSELIB_OT_new";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Add New Legacy Pose Library to active Object";

  /* callbacks */
  ot->exec = poselib_new_exec;
  ot->poll = ED_operator_posemode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ------------------------------------------------ */

static int poselib_unlink_exec(bContext *C, wmOperator *UNUSED(op))
{
  Object *ob = get_poselib_object(C);

  /* sanity checks */
  if (ELEM(NULL, ob, ob->poselib)) {
    return OPERATOR_CANCELLED;
  }

  /* there should be a poselib (we just checked above!), so just lower its user count and remove */
  id_us_min(&ob->poselib->id);
  ob->poselib = NULL;

  /* notifier here might evolve? */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, NULL);

  return OPERATOR_FINISHED;
}

void POSELIB_OT_unlink(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Unlink Legacy Pose Library";
  ot->idname = "POSELIB_OT_unlink";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Remove Legacy Pose Library from active Object";

  /* callbacks */
  ot->exec = poselib_unlink_exec;
  ot->poll = has_poselib_pose_data_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ************************************************************* */
/* Pose Editing Operators */

/* This tool automagically generates/validates poselib data so that it corresponds to the data
 * in the action. This is for use in making existing actions usable as poselibs.
 */
static int poselib_sanitize_exec(bContext *C, wmOperator *op)
{
  Object *ob = get_poselib_object(C);
  bAction *act = (ob) ? ob->poselib : NULL;
  TimeMarker *marker, *markern;

  /* validate action */
  if (act == NULL) {
    BKE_report(op->reports, RPT_WARNING, "No action to validate");
    return OPERATOR_CANCELLED;
  }

  /* determine which frames have keys */
  struct AnimKeylist *keylist = ED_keylist_create();
  action_to_keylist(NULL, act, keylist, 0);

  /* for each key, make sure there is a corresponding pose */
  LISTBASE_FOREACH (const ActKeyColumn *, ak, ED_keylist_listbase(keylist)) {
    /* check if any pose matches this */
    /* TODO: don't go looking through the list like this every time... */
    for (marker = act->markers.first; marker; marker = marker->next) {
      if (IS_EQ((double)marker->frame, (double)ak->cfra)) {
        marker->flag = -1;
        break;
      }
    }

    /* add new if none found */
    if (marker == NULL) {
      /* add pose to poselib */
      marker = MEM_callocN(sizeof(TimeMarker), "ActionMarker");

      BLI_snprintf(marker->name, sizeof(marker->name), "F%d Pose", (int)ak->cfra);

      marker->frame = (int)ak->cfra;
      marker->flag = -1;

      BLI_addtail(&act->markers, marker);
    }
  }

  /* remove all untagged poses (unused), and remove all tags */
  for (marker = act->markers.first; marker; marker = markern) {
    markern = marker->next;

    if (marker->flag != -1) {
      BLI_freelinkN(&act->markers, marker);
    }
    else {
      marker->flag = 0;
    }
  }

  /* free temp memory */
  ED_keylist_free(keylist);

  /* send notifiers for this - using keyframe editing notifiers, since action
   * may be being shown in anim editors as active action
   */
  WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME | NA_EDITED, NULL);

  return OPERATOR_FINISHED;
}

void POSELIB_OT_action_sanitize(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Sanitize Legacy Pose Library Action";
  ot->idname = "POSELIB_OT_action_sanitize";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Make action suitable for use as a Legacy Pose Library";

  /* callbacks */
  ot->exec = poselib_sanitize_exec;
  ot->poll = has_poselib_pose_data_for_editing_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ------------------------------------------ */

/* Poll callback for adding poses to a PoseLib */
static bool poselib_add_poll(bContext *C)
{
  /* There are 2 cases we need to be careful with:
   *  1) When this operator is invoked from a hotkey, there may be no PoseLib yet
   *  2) If a PoseLib already exists, we can't edit the action if it is a lib-linked
   *     actions, as data will be lost when saving the file
   */
  if (ED_operator_posemode(C)) {
    Object *ob = get_poselib_object(C);
    if (ob) {
      if ((ob->poselib == NULL) || BKE_id_is_editable(CTX_data_main(C), &ob->poselib->id)) {
        return true;
      }
    }
  }
  return false;
}

static void poselib_add_menu_invoke__replacemenu(bContext *C, uiLayout *layout, void *UNUSED(arg))
{
  Object *ob = get_poselib_object(C);
  bAction *act = ob->poselib; /* never NULL */
  TimeMarker *marker;

  wmOperatorType *ot = WM_operatortype_find("POSELIB_OT_pose_add", 1);

  BLI_assert(ot != NULL);

  /* set the operator execution context correctly */
  uiLayoutSetOperatorContext(layout, WM_OP_EXEC_DEFAULT);

  /* add each marker to this menu */
  for (marker = act->markers.first; marker; marker = marker->next) {
    PointerRNA props_ptr;
    uiItemFullO_ptr(
        layout, ot, marker->name, ICON_ARMATURE_DATA, NULL, WM_OP_EXEC_DEFAULT, 0, &props_ptr);
    RNA_int_set(&props_ptr, "frame", marker->frame);
    RNA_string_set(&props_ptr, "name", marker->name);
  }
}

static int poselib_add_menu_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
  Scene *scene = CTX_data_scene(C);
  Object *ob = get_poselib_object(C);
  bPose *pose = (ob) ? ob->pose : NULL;
  uiPopupMenu *pup;
  uiLayout *layout;

  /* sanity check */
  if (ELEM(NULL, ob, pose)) {
    return OPERATOR_CANCELLED;
  }

  /* start building */
  pup = UI_popup_menu_begin(C, op->type->name, ICON_NONE);
  layout = UI_popup_menu_layout(pup);
  uiLayoutSetOperatorContext(layout, WM_OP_EXEC_DEFAULT);

  /* add new (adds to the first unoccupied frame) */
  uiItemIntO(layout,
             IFACE_("Add New"),
             ICON_NONE,
             "POSELIB_OT_pose_add",
             "frame",
             poselib_get_free_index(ob->poselib));

  /* check if we have any choices to add a new pose in any other way */
  if ((ob->poselib) && (ob->poselib->markers.first)) {
    /* add new (on current frame) */
    uiItemIntO(layout,
               IFACE_("Add New (Current Frame)"),
               ICON_NONE,
               "POSELIB_OT_pose_add",
               "frame",
               scene->r.cfra);

    /* Replace existing - sub-menu. */
    uiItemMenuF(
        layout, IFACE_("Replace Existing..."), 0, poselib_add_menu_invoke__replacemenu, NULL);
  }

  UI_popup_menu_end(C, pup);

  /* this operator is only for a menu, not used further */
  return OPERATOR_INTERFACE;
}

static int poselib_add_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = get_poselib_object(C);
  bAction *act = poselib_validate(bmain, ob);
  bPose *pose = (ob) ? ob->pose : NULL;
  TimeMarker *marker;
  KeyingSet *ks;
  int frame = RNA_int_get(op->ptr, "frame");
  char name[64];

  /* sanity check (invoke should have checked this anyway) */
  if (ELEM(NULL, ob, pose)) {
    return OPERATOR_CANCELLED;
  }

  /* get name to give to pose */
  RNA_string_get(op->ptr, "name", name);

  /* add pose to poselib - replaces any existing pose there
   * - for the 'replace' option, this should end up finding the appropriate marker,
   *   so no new one will be added
   */
  for (marker = act->markers.first; marker; marker = marker->next) {
    if (marker->frame == frame) {
      BLI_strncpy(marker->name, name, sizeof(marker->name));
      break;
    }
  }
  if (marker == NULL) {
    marker = MEM_callocN(sizeof(TimeMarker), "ActionMarker");

    BLI_strncpy(marker->name, name, sizeof(marker->name));
    marker->frame = frame;

    BLI_addtail(&act->markers, marker);
  }

  /* validate name */
  BLI_uniquename(
      &act->markers, marker, DATA_("Pose"), '.', offsetof(TimeMarker, name), sizeof(marker->name));

  /* use Keying Set to determine what to store for the pose */

  /* This includes custom props :). */
  ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_WHOLE_CHARACTER_SELECTED_ID);

  ANIM_apply_keyingset(C, NULL, act, ks, MODIFYKEY_MODE_INSERT, (float)frame);

  /* store new 'active' pose number */
  act->active_marker = BLI_listbase_count(&act->markers);
  DEG_id_tag_update(&act->id, ID_RECALC_COPY_ON_WRITE);

  /* done */
  return OPERATOR_FINISHED;
}

void POSELIB_OT_pose_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Legacy PoseLib Add Pose";
  ot->idname = "POSELIB_OT_pose_add";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Add the current Pose to the active Legacy Pose Library";

  /* api callbacks */
  ot->invoke = poselib_add_menu_invoke;
  ot->exec = poselib_add_exec;
  ot->poll = poselib_add_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_int(ot->srna, "frame", 1, 0, INT_MAX, "Frame", "Frame to store pose on", 0, INT_MAX);
  RNA_def_string(ot->srna, "name", "Pose", 64, "Pose Name", "Name of newly added Pose");
}

/* ----- */

/* can be called with C == NULL */
static const EnumPropertyItem *poselib_stored_pose_itemf(bContext *C,
                                                         PointerRNA *UNUSED(ptr),
                                                         PropertyRNA *UNUSED(prop),
                                                         bool *r_free)
{
  Object *ob = get_poselib_object(C);
  bAction *act = (ob) ? ob->poselib : NULL;
  TimeMarker *marker;
  EnumPropertyItem *item = NULL, item_tmp = {0};
  int totitem = 0;
  int i = 0;

  if (C == NULL) {
    return DummyRNA_NULL_items;
  }

  /* check that the action exists */
  if (act) {
    /* add each marker to the list */
    for (marker = act->markers.first, i = 0; marker; marker = marker->next, i++) {
      item_tmp.identifier = item_tmp.name = marker->name;
      item_tmp.icon = ICON_ARMATURE_DATA;
      item_tmp.value = i;
      RNA_enum_item_add(&item, &totitem, &item_tmp);
    }
  }

  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

static int poselib_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = get_poselib_object(C);
  bAction *act = (ob) ? ob->poselib : NULL;
  TimeMarker *marker;
  int marker_index;
  FCurve *fcu;
  PropertyRNA *prop;

  /* check if valid poselib */
  if (act == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Object does not have pose lib data");
    return OPERATOR_CANCELLED;
  }

  prop = RNA_struct_find_property(op->ptr, "pose");
  if (RNA_property_is_set(op->ptr, prop)) {
    marker_index = RNA_property_enum_get(op->ptr, prop);
  }
  else {
    marker_index = act->active_marker - 1;
  }

  /* get index (and pointer) of pose to remove */
  marker = BLI_findlink(&act->markers, marker_index);
  if (marker == NULL) {
    BKE_reportf(op->reports, RPT_ERROR, "Invalid pose specified %d", marker_index);
    return OPERATOR_CANCELLED;
  }

  /* remove relevant keyframes */
  for (fcu = act->curves.first; fcu; fcu = fcu->next) {
    BezTriple *bezt;
    uint i;

    if (fcu->bezt) {
      for (i = 0, bezt = fcu->bezt; i < fcu->totvert; i++, bezt++) {
        /* check if remove */
        if (IS_EQF(bezt->vec[1][0], (float)marker->frame)) {
          BKE_fcurve_delete_key(fcu, i);
          BKE_fcurve_handles_recalc(fcu);
          break;
        }
      }
    }
  }

  /* remove poselib from list */
  BLI_freelinkN(&act->markers, marker);

  /* fix active pose number */
  act->active_marker = 0;

  /* send notifiers for this - using keyframe editing notifiers, since action
   * may be being shown in anim editors as active action
   */
  WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME | NA_EDITED, NULL);
  DEG_id_tag_update(&act->id, ID_RECALC_COPY_ON_WRITE);

  /* done */
  return OPERATOR_FINISHED;
}

void POSELIB_OT_pose_remove(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Legacy PoseLib Remove Pose";
  ot->idname = "POSELIB_OT_pose_remove";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Remove nth pose from the active Legacy Pose Library";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = poselib_remove_exec;
  ot->poll = has_poselib_pose_data_for_editing_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(ot->srna, "pose", DummyRNA_NULL_items, 0, "Pose", "The pose to remove");
  RNA_def_enum_funcs(prop, poselib_stored_pose_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  ot->prop = prop;
}

static int poselib_rename_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = get_poselib_object(C);
  bAction *act = (ob) ? ob->poselib : NULL;
  TimeMarker *marker;

  /* check if valid poselib */
  if (act == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Object does not have pose lib data");
    return OPERATOR_CANCELLED;
  }

  /* get index (and pointer) of pose to remove */
  marker = BLI_findlink(&act->markers, act->active_marker - 1);
  if (marker == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Invalid index for pose");
    return OPERATOR_CANCELLED;
  }

  /* Use the existing name of the marker as the name,
   * and use the active marker as the one to rename. */
  RNA_enum_set(op->ptr, "pose", act->active_marker - 1);
  RNA_string_set(op->ptr, "name", marker->name);

  /* part to sync with other similar operators... */
  return WM_operator_props_popup_confirm(C, op, event);
}

static int poselib_rename_exec(bContext *C, wmOperator *op)
{
  Object *ob = BKE_object_pose_armature_get(CTX_data_active_object(C));
  bAction *act = (ob) ? ob->poselib : NULL;
  TimeMarker *marker;
  char newname[64];

  /* check if valid poselib */
  if (act == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Object does not have pose lib data");
    return OPERATOR_CANCELLED;
  }

  /* get index (and pointer) of pose to remove */
  marker = BLI_findlink(&act->markers, RNA_enum_get(op->ptr, "pose"));
  if (marker == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Invalid index for pose");
    return OPERATOR_CANCELLED;
  }

  /* get new name */
  RNA_string_get(op->ptr, "name", newname);

  /* copy name and validate it */
  BLI_strncpy(marker->name, newname, sizeof(marker->name));
  BLI_uniquename(
      &act->markers, marker, DATA_("Pose"), '.', offsetof(TimeMarker, name), sizeof(marker->name));

  /* send notifiers for this - using keyframe editing notifiers, since action
   * may be being shown in anim editors as active action
   */
  WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME | NA_EDITED, NULL);

  /* done */
  return OPERATOR_FINISHED;
}

void POSELIB_OT_pose_rename(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Legacy PoseLib Rename Pose";
  ot->idname = "POSELIB_OT_pose_rename";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Rename specified pose from the active Legacy Pose Library";

  /* api callbacks */
  ot->invoke = poselib_rename_invoke;
  ot->exec = poselib_rename_exec;
  ot->poll = has_poselib_pose_data_for_editing_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  /* NOTE: name not pose is the operator's "main" property,
   * so that it will get activated in the popup for easy renaming */
  ot->prop = RNA_def_string(
      ot->srna, "name", "RenamedPose", 64, "New Pose Name", "New name for pose");
  prop = RNA_def_enum(ot->srna, "pose", DummyRNA_NULL_items, 0, "Pose", "The pose to rename");
  RNA_def_enum_funcs(prop, poselib_stored_pose_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
}

static int poselib_move_exec(bContext *C, wmOperator *op)
{
  Object *ob = get_poselib_object(C);
  bAction *act = (ob) ? ob->poselib : NULL;
  TimeMarker *marker;
  int marker_index;
  int dir;
  PropertyRNA *prop;

  /* check if valid poselib */
  if (act == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Object does not have pose lib data");
    return OPERATOR_CANCELLED;
  }

  prop = RNA_struct_find_property(op->ptr, "pose");
  if (RNA_property_is_set(op->ptr, prop)) {
    marker_index = RNA_property_enum_get(op->ptr, prop);
  }
  else {
    marker_index = act->active_marker - 1;
  }

  /* get index (and pointer) of pose to remove */
  marker = BLI_findlink(&act->markers, marker_index);
  if (marker == NULL) {
    BKE_reportf(op->reports, RPT_ERROR, "Invalid pose specified %d", marker_index);
    return OPERATOR_CANCELLED;
  }

  dir = RNA_enum_get(op->ptr, "direction");

  /* move pose */
  if (BLI_listbase_link_move(&act->markers, marker, dir)) {
    act->active_marker = marker_index + dir + 1;

    /* send notifiers for this - using keyframe editing notifiers, since action
     * may be being shown in anim editors as active action
     */
    WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME | NA_EDITED, NULL);
  }
  else {
    return OPERATOR_CANCELLED;
  }

  /* done */
  return OPERATOR_FINISHED;
}

void POSELIB_OT_pose_move(wmOperatorType *ot)
{
  PropertyRNA *prop;
  static const EnumPropertyItem pose_lib_pose_move[] = {
      {-1, "UP", 0, "Up", ""},
      {1, "DOWN", 0, "Down", ""},
      {0, NULL, 0, NULL, NULL},
  };

  /* identifiers */
  ot->name = "Legacy PoseLib Move Pose";
  ot->idname = "POSELIB_OT_pose_move";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Move the pose up or down in the active Legacy Pose Library";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = poselib_move_exec;
  ot->poll = has_poselib_pose_data_for_editing_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_enum(ot->srna, "pose", DummyRNA_NULL_items, 0, "Pose", "The pose to move");
  RNA_def_enum_funcs(prop, poselib_stored_pose_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  ot->prop = prop;

  RNA_def_enum(ot->srna,
               "direction",
               pose_lib_pose_move,
               0,
               "Direction",
               "Direction to move the chosen pose towards");
}

/* ************************************************************* */
/* Pose-Lib Browsing/Previewing Operator */

/* Simple struct for storing settings/data for use during PoseLib preview */
typedef struct tPoseLib_PreviewData {
  /** tPoseLib_Backup structs for restoring poses. */
  ListBase backups;
  /** LinkData structs storing list of poses which match the current search-string. */
  ListBase searchp;

  /** active scene. */
  Scene *scene;
  /** active area. */
  ScrArea *area;

  /** RNA-Pointer to Object 'ob'. */
  PointerRNA rna_ptr;
  /** object to work on. */
  Object *ob;
  /** object's armature data. */
  bArmature *arm;
  /** object's pose. */
  bPose *pose;
  /** poselib to use. */
  bAction *act;
  /** 'active' pose. */
  TimeMarker *marker;

  /** total number of elements to work on. */
  int totcount;

  /** state of main loop. */
  short state;
  /** redraw/update settings during main loop. */
  short redraw;
  /** flags for various settings. */
  short flag;

  /** position of cursor in searchstr (cursor occurs before the item at the nominated index) */
  short search_cursor;
  /** (Part of) Name to search for to filter poses that get shown. */
  char searchstr[64];
  /** Previously set searchstr (from last loop run),
   * so that we can detected when to rebuild searchp. */
  char searchold[64];

  /** Info-text to print in header. */
  char headerstr[UI_MAX_DRAW_STR];
} tPoseLib_PreviewData;

/* defines for tPoseLib_PreviewData->state values */
enum {
  PL_PREVIEW_ERROR = -1,
  PL_PREVIEW_RUNNING,
  PL_PREVIEW_CONFIRM,
  PL_PREVIEW_CANCEL,
  PL_PREVIEW_RUNONCE,
};

/* defines for tPoseLib_PreviewData->redraw values */
enum {
  PL_PREVIEW_NOREDRAW = 0,
  PL_PREVIEW_REDRAWALL,
  PL_PREVIEW_REDRAWHEADER,
};

/* defines for tPoseLib_PreviewData->flag values */
enum {
  PL_PREVIEW_FIRSTTIME = (1 << 0),
  PL_PREVIEW_SHOWORIGINAL = (1 << 1),
  PL_PREVIEW_ANY_BONE_SELECTED = (1 << 2),
};

/* ---------------------------- */

/* simple struct for storing backup info */
typedef struct tPoseLib_Backup {
  struct tPoseLib_Backup *next, *prev;

  bPoseChannel *pchan; /* pose channel backups are for */

  bPoseChannel olddata; /* copy of pose channel's old data (at start) */
  IDProperty *oldprops; /* copy (needs freeing) of pose channel's properties (at start) */
} tPoseLib_Backup;

/* Makes a copy of the current pose for restoration purposes - doesn't do constraints currently */
static void poselib_backup_posecopy(tPoseLib_PreviewData *pld)
{
  bActionGroup *agrp;
  bPoseChannel *pchan;
  bool selected = false;

  /* determine whether any bone is selected. */
  LISTBASE_FOREACH (bPoseChannel *, bchan, &pld->pose->chanbase) {
    selected = bchan->bone != NULL && bchan->bone->flag & BONE_SELECTED;
    if (selected) {
      pld->flag |= PL_PREVIEW_ANY_BONE_SELECTED;
      break;
    }
  }
  if (!selected) {
    pld->flag &= ~PL_PREVIEW_ANY_BONE_SELECTED;
  }

  /* for each posechannel that has an actionchannel in */
  for (agrp = pld->act->groups.first; agrp; agrp = agrp->next) {
    /* try to find posechannel */
    pchan = BKE_pose_channel_find_name(pld->pose, agrp->name);

    /* backup data if available */
    if (pchan) {
      tPoseLib_Backup *plb;

      /* store backup */
      plb = MEM_callocN(sizeof(tPoseLib_Backup), "tPoseLib_Backup");

      plb->pchan = pchan;
      memcpy(&plb->olddata, plb->pchan, sizeof(bPoseChannel));

      if (pchan->prop) {
        plb->oldprops = IDP_CopyProperty(pchan->prop);
      }

      BLI_addtail(&pld->backups, plb);

      /* mark as being affected */
      pld->totcount++;
    }
  }
}

/* Restores original pose */
static void poselib_backup_restore(tPoseLib_PreviewData *pld)
{
  tPoseLib_Backup *plb;

  for (plb = pld->backups.first; plb; plb = plb->next) {
    /* copy most of data straight back */
    memcpy(plb->pchan, &plb->olddata, sizeof(bPoseChannel));

    /* just overwrite values of properties from the stored copies (there should be some) */
    if (plb->oldprops) {
      IDP_SyncGroupValues(plb->pchan->prop, plb->oldprops);
    }

    /* TODO: constraints settings aren't restored yet,
     * even though these could change (though not that likely) */
  }
}

/* Free list of backups, including any side data it may use */
static void poselib_backup_free_data(tPoseLib_PreviewData *pld)
{
  tPoseLib_Backup *plb, *plbn;

  for (plb = pld->backups.first; plb; plb = plbn) {
    plbn = plb->next;

    /* free custom data */
    if (plb->oldprops) {
      IDP_FreeProperty(plb->oldprops);
    }

    /* free backup element now */
    BLI_freelinkN(&pld->backups, plb);
  }
}

/* ---------------------------- */

/* Applies the appropriate stored pose from the pose-library to the current pose
 * - assumes that a valid object, with a poselib has been supplied
 * - gets the string to print in the header
 * - this code is based on the code for extract_pose_from_action in blenkernel/action.c
 */
static void poselib_apply_pose(tPoseLib_PreviewData *pld,
                               const AnimationEvalContext *anim_eval_context)
{
  PointerRNA *ptr = &pld->rna_ptr;
  bArmature *arm = pld->arm;
  bPose *pose = pld->pose;
  bPoseChannel *pchan;
  bAction *act = pld->act;
  bActionGroup *agrp;

  KeyframeEditData ked = {{NULL}};
  KeyframeEditFunc group_ok_cb;
  int frame = 1;
  const bool any_bone_selected = pld->flag & PL_PREVIEW_ANY_BONE_SELECTED;

  /* get the frame */
  if (pld->marker) {
    frame = pld->marker->frame;
  }
  else {
    return;
  }

  /* init settings for testing groups for keyframes */
  group_ok_cb = ANIM_editkeyframes_ok(BEZT_OK_FRAMERANGE);
  ked.f1 = ((float)frame) - 0.5f;
  ked.f2 = ((float)frame) + 0.5f;
  AnimationEvalContext anim_context_at_frame = BKE_animsys_eval_context_construct_at(
      anim_eval_context, frame);

  /* start applying - only those channels which have a key at this point in time! */
  for (agrp = act->groups.first; agrp; agrp = agrp->next) {
    /* check if group has any keyframes */
    if (ANIM_animchanneldata_keyframes_loop(
            &ked, NULL, agrp, ALE_GROUP, NULL, group_ok_cb, NULL)) {
      /* has keyframe on this frame, so try to get a PoseChannel with this name */
      pchan = BKE_pose_channel_find_name(pose, agrp->name);

      if (pchan) {
        bool ok = 0;

        /* check if this bone should get any animation applied */
        if (!any_bone_selected) {
          /* if no bones are selected, then any bone is ok */
          ok = 1;
        }
        else if (pchan->bone) {
          /* only ok if bone is visible and selected */
          if ((pchan->bone->flag & BONE_SELECTED) && (pchan->bone->flag & BONE_HIDDEN_P) == 0 &&
              BKE_pose_is_layer_visible(arm, pchan)) {
            ok = 1;
          }
        }

        if (ok) {
          animsys_evaluate_action_group(ptr, act, agrp, &anim_context_at_frame);
        }
      }
    }
  }
}

/* Auto-keys/tags bones affected by the pose used from the poselib */
static void poselib_keytag_pose(bContext *C, Scene *scene, tPoseLib_PreviewData *pld)
{
  bPose *pose = pld->pose;
  bPoseChannel *pchan;
  bAction *act = pld->act;
  bActionGroup *agrp;

  KeyingSet *ks = ANIM_get_keyingset_for_autokeying(scene, ANIM_KS_WHOLE_CHARACTER_ID);
  ListBase dsources = {NULL, NULL};
  bool autokey = autokeyframe_cfra_can_key(scene, &pld->ob->id);
  const bool any_bone_selected = pld->flag & PL_PREVIEW_ANY_BONE_SELECTED;

  /* start tagging/keying */
  for (agrp = act->groups.first; agrp; agrp = agrp->next) {
    /* Only for selected bones unless there aren't any selected, in which case all are included. */
    pchan = BKE_pose_channel_find_name(pose, agrp->name);

    if (pchan) {
      if (!any_bone_selected || ((pchan->bone) && (pchan->bone->flag & BONE_SELECTED))) {
        if (autokey) {
          /* Add data-source override for the PoseChannel, to be used later. */
          ANIM_relative_keyingset_add_source(&dsources, &pld->ob->id, &RNA_PoseBone, pchan);
        }
      }
    }
  }

  /* perform actual auto-keying now */
  if (autokey) {
    /* insert keyframes for all relevant bones in one go */
    ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, (float)scene->r.cfra);
    BLI_freelistN(&dsources);
  }

  /* send notifiers for this */
  WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME | NA_EDITED, NULL);
}

/* Apply the relevant changes to the pose */
static void poselib_preview_apply(bContext *C, wmOperator *op)
{
  tPoseLib_PreviewData *pld = (tPoseLib_PreviewData *)op->customdata;

  /* only recalc pose (and its dependencies) if pose has changed */
  if (pld->redraw == PL_PREVIEW_REDRAWALL) {
    /* Don't clear pose if first time. */
    if ((pld->flag & PL_PREVIEW_FIRSTTIME) == 0) {
      poselib_backup_restore(pld);
    }
    else {
      pld->flag &= ~PL_PREVIEW_FIRSTTIME;
    }

    /* pose should be the right one to draw (unless we're temporarily not showing it) */
    if ((pld->flag & PL_PREVIEW_SHOWORIGINAL) == 0) {
      RNA_int_set(op->ptr, "pose_index", BLI_findindex(&pld->act->markers, pld->marker));
      struct Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

      const AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(
          depsgraph, 0.0f /* poselib_apply_pose() determines its own evaluation time. */);
      poselib_apply_pose(pld, &anim_eval_context);
    }
    else {
      RNA_int_set(op->ptr, "pose_index", -2); /* -2 means don't apply any pose */
    }

    DEG_id_tag_update(&pld->ob->id, ID_RECALC_GEOMETRY);
  }

  /* do header print - if interactively previewing */
  if (pld->state == PL_PREVIEW_RUNNING) {
    if (pld->flag & PL_PREVIEW_SHOWORIGINAL) {
      ED_area_status_text(pld->area, TIP_("PoseLib Previewing Pose: [Showing Original Pose]"));
      ED_workspace_status_text(C, TIP_("Use Tab to start previewing poses again"));
    }
    else if (pld->searchstr[0]) {
      char tempstr[65];
      char markern[64];
      short index;

      /* get search-string */
      index = pld->search_cursor;

      if (index >= 0 && index < sizeof(tempstr) - 1) {
        memcpy(&tempstr[0], &pld->searchstr[0], index);
        tempstr[index] = '|';
        memcpy(&tempstr[index + 1], &pld->searchstr[index], (sizeof(tempstr) - 1) - index);
      }
      else {
        BLI_strncpy(tempstr, pld->searchstr, sizeof(tempstr));
      }

      /* get marker name */
      BLI_strncpy(markern, pld->marker ? pld->marker->name : "No Matches", sizeof(markern));

      BLI_snprintf(pld->headerstr,
                   sizeof(pld->headerstr),
                   TIP_("PoseLib Previewing Pose: Filter - [%s] | "
                        "Current Pose - \"%s\""),
                   tempstr,
                   markern);
      ED_area_status_text(pld->area, pld->headerstr);
      ED_workspace_status_text(C, TIP_("Use ScrollWheel or PageUp/Down to change pose"));
    }
    else {
      BLI_snprintf(pld->headerstr,
                   sizeof(pld->headerstr),
                   TIP_("PoseLib Previewing Pose: \"%s\""),
                   pld->marker->name);
      ED_area_status_text(pld->area, pld->headerstr);
      ED_workspace_status_text(C, NULL);
    }
  }

  /* request drawing of view + clear redraw flag */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, pld->ob);
  pld->redraw = PL_PREVIEW_NOREDRAW;
}

/* ---------------------------- */

/* This helper function is called during poselib_preview_poses to find the
 * pose to preview next (after a change event)
 */
static void poselib_preview_get_next(tPoseLib_PreviewData *pld, int step)
{
  /* stop if not going anywhere, as we assume that there is a direction to move in */
  if (step == 0) {
    return;
  }

  /* search-string dictates a special approach */
  if (pld->searchstr[0]) {
    TimeMarker *marker;
    LinkData *ld, *ldn, *ldc;

    /* free and rebuild if needed (i.e. if search-str changed) */
    if (!STREQ(pld->searchstr, pld->searchold)) {
      /* free list of temporary search matches */
      BLI_freelistN(&pld->searchp);

      /* generate a new list of search matches */
      for (marker = pld->act->markers.first; marker; marker = marker->next) {
        /* does the name partially match?
         * - don't worry about case, to make it easier for users to quickly input a name (or
         *   part of one), which is the whole point of this feature
         */
        if (BLI_strcasestr(marker->name, pld->searchstr)) {
          /* make link-data to store reference to it */
          ld = MEM_callocN(sizeof(LinkData), "PoseMatch");
          ld->data = marker;
          BLI_addtail(&pld->searchp, ld);
        }
      }

      /* set current marker to NULL (so that we start from first) */
      pld->marker = NULL;
    }

    /* check if any matches */
    if (BLI_listbase_is_empty(&pld->searchp)) {
      pld->marker = NULL;
      return;
    }

    /* find first match */
    for (ldc = pld->searchp.first; ldc; ldc = ldc->next) {
      if (ldc->data == pld->marker) {
        break;
      }
    }
    if (ldc == NULL) {
      ldc = pld->searchp.first;
    }

    /* Loop through the matches in a cyclic fashion, incrementing/decrementing step as appropriate
     * until step == 0. At this point, marker should be the correct marker.
     */
    if (step > 0) {
      for (ld = ldc; ld && step; ld = ldn, step--) {
        ldn = (ld->next) ? ld->next : pld->searchp.first;
      }
    }
    else {
      for (ld = ldc; ld && step; ld = ldn, step++) {
        ldn = (ld->prev) ? ld->prev : pld->searchp.last;
      }
    }

    /* set marker */
    if (ld) {
      pld->marker = ld->data;
    }
  }
  else {
    TimeMarker *marker, *next;

    /* if no marker, because we just ended searching, then set that to the start of the list */
    if (pld->marker == NULL) {
      pld->marker = pld->act->markers.first;
    }

    /* Loop through the markers in a cyclic fashion, incrementing/decrementing step as appropriate
     * until step == 0. At this point, marker should be the correct marker.
     */
    if (step > 0) {
      for (marker = pld->marker; marker && step; marker = next, step--) {
        next = (marker->next) ? marker->next : pld->act->markers.first;
      }
    }
    else {
      for (marker = pld->marker; marker && step; marker = next, step++) {
        next = (marker->prev) ? marker->prev : pld->act->markers.last;
      }
    }

    /* it should be fairly impossible for marker to be NULL */
    if (marker) {
      pld->marker = marker;
    }
  }
}

/* specially handle events for searching */
static void poselib_preview_handle_search(tPoseLib_PreviewData *pld, ushort event_type, char ascii)
{
  /* try doing some form of string manipulation first */
  switch (event_type) {
    case EVT_BACKSPACEKEY:
      if (pld->searchstr[0] && pld->search_cursor) {
        short len = strlen(pld->searchstr);
        short index = pld->search_cursor;
        short i;

        for (i = index; i <= len; i++) {
          pld->searchstr[i - 1] = pld->searchstr[i];
        }

        pld->search_cursor--;

        poselib_preview_get_next(pld, 1);
        pld->redraw = PL_PREVIEW_REDRAWALL;
        return;
      }
      break;

    case EVT_DELKEY:
      if (pld->searchstr[0] && pld->searchstr[1]) {
        short len = strlen(pld->searchstr);
        short index = pld->search_cursor;
        int i;

        if (index < len) {
          for (i = index; i < len; i++) {
            pld->searchstr[i] = pld->searchstr[i + 1];
          }

          poselib_preview_get_next(pld, 1);
          pld->redraw = PL_PREVIEW_REDRAWALL;
          return;
        }
      }
      break;
  }

  if (ascii) {
    /* character to add to the string */
    short index = pld->search_cursor;
    short len = (pld->searchstr[0]) ? strlen(pld->searchstr) : 0;
    short i;

    if (len) {
      for (i = len; i > index; i--) {
        pld->searchstr[i] = pld->searchstr[i - 1];
      }
    }
    else {
      pld->searchstr[1] = 0;
    }

    pld->searchstr[index] = ascii;
    pld->search_cursor++;

    poselib_preview_get_next(pld, 1);
    pld->redraw = PL_PREVIEW_REDRAWALL;
  }
}

/* handle events for poselib_preview_poses */
static int poselib_preview_handle_event(bContext *UNUSED(C), wmOperator *op, const wmEvent *event)
{
  tPoseLib_PreviewData *pld = op->customdata;
  int ret = OPERATOR_RUNNING_MODAL;

  /* only accept 'press' event, and ignore 'release', so that we don't get double actions */
  if (ELEM(event->val, KM_PRESS, KM_NOTHING) == 0) {
#if 0
    printf("PoseLib: skipping event with type '%s' and val %d\n",
           WM_key_event_string(event->type, false),
           event->val);
#endif
    return ret;
  }

  /* backup stuff that needs to occur before every operation
   * - make a copy of searchstr, so that we know if cache needs to be rebuilt
   */
  BLI_strncpy(pld->searchold, pld->searchstr, sizeof(pld->searchold));

  /* if we're currently showing the original pose, only certain events are handled */
  if (pld->flag & PL_PREVIEW_SHOWORIGINAL) {
    switch (event->type) {
      /* exit - cancel */
      case EVT_ESCKEY:
      case RIGHTMOUSE:
        pld->state = PL_PREVIEW_CANCEL;
        break;

      /* exit - confirm */
      case LEFTMOUSE:
      case EVT_RETKEY:
      case EVT_PADENTER:
      case EVT_SPACEKEY:
        pld->state = PL_PREVIEW_CONFIRM;
        break;

      /* view manipulation */
      /* we add pass through here, so that the operators responsible for these can still run,
       * even though we still maintain control (as RUNNING_MODAL flag is still set too)
       */
      case EVT_PAD0:
      case EVT_PAD1:
      case EVT_PAD2:
      case EVT_PAD3:
      case EVT_PAD4:
      case EVT_PAD5:
      case EVT_PAD6:
      case EVT_PAD7:
      case EVT_PAD8:
      case EVT_PAD9:
      case EVT_PADPLUSKEY:
      case EVT_PADMINUS:
      case MIDDLEMOUSE:
      case MOUSEMOVE:
        // pld->redraw = PL_PREVIEW_REDRAWHEADER;
        ret = OPERATOR_PASS_THROUGH;
        break;

      /* Quickly compare to original. */
      case EVT_TABKEY:
        pld->flag &= ~PL_PREVIEW_SHOWORIGINAL;
        pld->redraw = PL_PREVIEW_REDRAWALL;
        break;
    }

    /* EXITS HERE... */
    return ret;
  }

  /* NORMAL EVENT HANDLING... */
  /* searching takes priority over normal activity */
  switch (event->type) {
    /* exit - cancel */
    case EVT_ESCKEY:
    case RIGHTMOUSE:
      pld->state = PL_PREVIEW_CANCEL;
      break;

    /* exit - confirm */
    case LEFTMOUSE:
    case EVT_RETKEY:
    case EVT_PADENTER:
    case EVT_SPACEKEY:
      pld->state = PL_PREVIEW_CONFIRM;
      break;

    /* Toggle between original pose and poselib pose. */
    case EVT_TABKEY:
      pld->flag |= PL_PREVIEW_SHOWORIGINAL;
      pld->redraw = PL_PREVIEW_REDRAWALL;
      break;

    /* change to previous pose (cyclic) */
    case EVT_PAGEUPKEY:
    case WHEELUPMOUSE:
      poselib_preview_get_next(pld, -1);
      pld->redraw = PL_PREVIEW_REDRAWALL;
      break;

    /* change to next pose (cyclic) */
    case EVT_PAGEDOWNKEY:
    case WHEELDOWNMOUSE:
      poselib_preview_get_next(pld, 1);
      pld->redraw = PL_PREVIEW_REDRAWALL;
      break;

    /* jump 5 poses (cyclic, back) */
    case EVT_DOWNARROWKEY:
      poselib_preview_get_next(pld, -5);
      pld->redraw = PL_PREVIEW_REDRAWALL;
      break;

    /* jump 5 poses (cyclic, forward) */
    case EVT_UPARROWKEY:
      poselib_preview_get_next(pld, 5);
      pld->redraw = PL_PREVIEW_REDRAWALL;
      break;

    /* change to next pose or searching cursor control */
    case EVT_RIGHTARROWKEY:
      if (pld->searchstr[0]) {
        /* move text-cursor to the right */
        if (pld->search_cursor < strlen(pld->searchstr)) {
          pld->search_cursor++;
        }
        pld->redraw = PL_PREVIEW_REDRAWHEADER;
      }
      else {
        /* change to next pose (cyclic) */
        poselib_preview_get_next(pld, 1);
        pld->redraw = PL_PREVIEW_REDRAWALL;
      }
      break;

    /* change to next pose or searching cursor control */
    case EVT_LEFTARROWKEY:
      if (pld->searchstr[0]) {
        /* move text-cursor to the left */
        if (pld->search_cursor) {
          pld->search_cursor--;
        }
        pld->redraw = PL_PREVIEW_REDRAWHEADER;
      }
      else {
        /* change to previous pose (cyclic) */
        poselib_preview_get_next(pld, -1);
        pld->redraw = PL_PREVIEW_REDRAWALL;
      }
      break;

    /* change to first pose or start of searching string */
    case EVT_HOMEKEY:
      if (pld->searchstr[0]) {
        pld->search_cursor = 0;
        pld->redraw = PL_PREVIEW_REDRAWHEADER;
      }
      else {
        /* change to first pose */
        pld->marker = pld->act->markers.first;
        pld->act->active_marker = 1;

        pld->redraw = PL_PREVIEW_REDRAWALL;
      }
      break;

    /* change to last pose or start of searching string */
    case EVT_ENDKEY:
      if (pld->searchstr[0]) {
        pld->search_cursor = strlen(pld->searchstr);
        pld->redraw = PL_PREVIEW_REDRAWHEADER;
      }
      else {
        /* change to last pose */
        pld->marker = pld->act->markers.last;
        pld->act->active_marker = BLI_listbase_count(&pld->act->markers);

        pld->redraw = PL_PREVIEW_REDRAWALL;
      }
      break;

    /* view manipulation */
    /* we add pass through here, so that the operators responsible for these can still run,
     * even though we still maintain control (as RUNNING_MODAL flag is still set too)
     */
    case MIDDLEMOUSE:
    case MOUSEMOVE:
      // pld->redraw = PL_PREVIEW_REDRAWHEADER;
      ret = OPERATOR_PASS_THROUGH;
      break;

    /* view manipulation, or searching */
    case EVT_PAD0:
    case EVT_PAD1:
    case EVT_PAD2:
    case EVT_PAD3:
    case EVT_PAD4:
    case EVT_PAD5:
    case EVT_PAD6:
    case EVT_PAD7:
    case EVT_PAD8:
    case EVT_PAD9:
    case EVT_PADPLUSKEY:
    case EVT_PADMINUS:
      if (pld->searchstr[0]) {
        /* searching... */
        poselib_preview_handle_search(pld, event->type, WM_event_utf8_to_ascii(event));
      }
      else {
        /* view manipulation (see above) */
        // pld->redraw = PL_PREVIEW_REDRAWHEADER;
        ret = OPERATOR_PASS_THROUGH;
      }
      break;

    /* otherwise, assume that searching might be able to handle it */
    default:
      poselib_preview_handle_search(pld, event->type, WM_event_utf8_to_ascii(event));
      break;
  }

  return ret;
}

/* ---------------------------- */

/* Init PoseLib Previewing data */
static void poselib_preview_init_data(bContext *C, wmOperator *op)
{
  tPoseLib_PreviewData *pld;
  Object *ob = get_poselib_object(C);
  int pose_index = RNA_int_get(op->ptr, "pose_index");

  /* set up preview state info */
  op->customdata = pld = MEM_callocN(sizeof(tPoseLib_PreviewData), "PoseLib Preview Data");

  /* get basic data */
  pld->ob = ob;
  pld->arm = (ob) ? (ob->data) : NULL;
  pld->pose = (ob) ? (ob->pose) : NULL;
  pld->act = (ob) ? (ob->poselib) : NULL;

  pld->scene = CTX_data_scene(C);
  pld->area = CTX_wm_area(C);

  /* get starting pose based on RNA-props for this operator */
  if (pose_index == -1) {
    pld->marker = poselib_get_active_pose(pld->act);
  }
  else if (pose_index == -2) {
    pld->flag |= PL_PREVIEW_SHOWORIGINAL;
  }
  else {
    pld->marker = (pld->act) ? BLI_findlink(&pld->act->markers, pose_index) : NULL;
  }

  /* check if valid poselib */
  if (ELEM(NULL, pld->ob, pld->pose, pld->arm)) {
    BKE_report(op->reports, RPT_ERROR, "Pose lib is only for armatures in pose mode");
    pld->state = PL_PREVIEW_ERROR;
    return;
  }
  if (pld->act == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Object does not have a valid pose lib");
    pld->state = PL_PREVIEW_ERROR;
    return;
  }
  if (pld->marker == NULL) {
    if (pld->act->markers.first) {
      /* just use first one then... */
      pld->marker = pld->act->markers.first;
      if (pose_index > -2) {
        BKE_report(op->reports, RPT_WARNING, "Pose lib had no active pose");
      }
    }
    else {
      BKE_report(op->reports, RPT_ERROR, "Pose lib has no poses to preview/apply");
      pld->state = PL_PREVIEW_ERROR;
      return;
    }
  }

  /* get ID pointer for applying poses */
  RNA_id_pointer_create(&ob->id, &pld->rna_ptr);

  /* make backups for restoring pose */
  poselib_backup_posecopy(pld);

  /* set flags for running */
  pld->state = PL_PREVIEW_RUNNING;
  pld->redraw = PL_PREVIEW_REDRAWALL;
  pld->flag |= PL_PREVIEW_FIRSTTIME;

  /* set depsgraph flags */
  /* make sure the lock is set OK, unlock can be accidentally saved? */
  pld->pose->flag |= POSE_LOCKED;
  pld->pose->flag &= ~POSE_DO_UNLOCK;

  /* clear strings + search */
  pld->headerstr[0] = pld->searchstr[0] = pld->searchold[0] = '\0';
  pld->search_cursor = 0;
}

/* After previewing poses */
static void poselib_preview_cleanup(bContext *C, wmOperator *op)
{
  tPoseLib_PreviewData *pld = (tPoseLib_PreviewData *)op->customdata;
  Scene *scene = pld->scene;
  Object *ob = pld->ob;
  bPose *pose = pld->pose;
  bAction *act = pld->act;
  TimeMarker *marker = pld->marker;

  /* redraw the header so that it doesn't show any of our stuff anymore */
  ED_area_status_text(pld->area, NULL);
  ED_workspace_status_text(C, NULL);

  /* this signal does one recalc on pose, then unlocks, so ESC or edit will work */
  pose->flag |= POSE_DO_UNLOCK;

  /* clear pose if canceled */
  if (pld->state == PL_PREVIEW_CANCEL) {
    poselib_backup_restore(pld);

    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }
  else if (pld->state == PL_PREVIEW_CONFIRM) {
    /* tag poses as appropriate */
    poselib_keytag_pose(C, scene, pld);

    /* change active pose setting */
    act->active_marker = BLI_findindex(&act->markers, marker) + 1;
    action_set_activemarker(act, marker, NULL);

    /* Update event for pose and deformation children */
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);

    /* updates */
    if (IS_AUTOKEY_MODE(scene, NORMAL)) {
      // remake_action_ipos(ob->action);
    }
  }

  /* Request final redraw of the view. */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, pld->ob);

  /* free memory used for backups and searching */
  poselib_backup_free_data(pld);
  BLI_freelistN(&pld->searchp);

  /* free temp data for operator */
  MEM_freeN(pld);
  op->customdata = NULL;
}

/* End previewing operation */
static int poselib_preview_exit(bContext *C, wmOperator *op)
{
  tPoseLib_PreviewData *pld = op->customdata;
  int exit_state = pld->state;

  /* finish up */
  poselib_preview_cleanup(C, op);

  if (ELEM(exit_state, PL_PREVIEW_CANCEL, PL_PREVIEW_ERROR)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

/* Cancel previewing operation (called when exiting Blender) */
static void poselib_preview_cancel(bContext *C, wmOperator *op)
{
  poselib_preview_exit(C, op);
}

/* main modal status check */
static int poselib_preview_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  tPoseLib_PreviewData *pld = op->customdata;
  int ret;

  /* 1) check state to see if we're still running */
  if (pld->state != PL_PREVIEW_RUNNING) {
    return poselib_preview_exit(C, op);
  }

  /* 2) handle events */
  ret = poselib_preview_handle_event(C, op, event);

  /* 3) apply changes and redraw, otherwise, confirming goes wrong */
  if (pld->redraw) {
    poselib_preview_apply(C, op);
  }

  return ret;
}

/* Modal Operator init */
static int poselib_preview_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
  tPoseLib_PreviewData *pld;

  /* check if everything is ok, and init settings for modal operator */
  poselib_preview_init_data(C, op);
  pld = (tPoseLib_PreviewData *)op->customdata;

  if (pld->state == PL_PREVIEW_ERROR) {
    /* an error occurred, so free temp mem used */
    poselib_preview_cleanup(C, op);
    return OPERATOR_CANCELLED;
  }

  /* do initial apply to have something to look at */
  poselib_preview_apply(C, op);

  /* add temp handler if we're running as a modal operator */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

/* Repeat operator */
static int poselib_preview_exec(bContext *C, wmOperator *op)
{
  tPoseLib_PreviewData *pld;

  /* check if everything is ok, and init settings for modal operator */
  poselib_preview_init_data(C, op);
  pld = (tPoseLib_PreviewData *)op->customdata;

  if (pld->state == PL_PREVIEW_ERROR) {
    /* an error occurred, so free temp mem used */
    poselib_preview_cleanup(C, op);
    return OPERATOR_CANCELLED;
  }

  /* the exec() callback is effectively a 'run-once' scenario, so set the state to that
   * so that everything draws correctly
   */
  pld->state = PL_PREVIEW_RUNONCE;

  /* apply the active pose */
  poselib_preview_apply(C, op);

  /* now, set the status to exit */
  pld->state = PL_PREVIEW_CONFIRM;

  /* cleanup */
  return poselib_preview_exit(C, op);
}

void POSELIB_OT_browse_interactive(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Legacy PoseLib Browse Poses";
  ot->idname = "POSELIB_OT_browse_interactive";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Interactively browse Legacy Pose Library poses in 3D-View";

  /* callbacks */
  ot->invoke = poselib_preview_invoke;
  ot->modal = poselib_preview_modal;
  ot->cancel = poselib_preview_cancel;
  ot->exec = poselib_preview_exec;
  ot->poll = has_poselib_pose_data_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* properties */
  /* TODO: make the pose_index into a proper enum instead of a cryptic int. */
  ot->prop = RNA_def_int(
      ot->srna,
      "pose_index",
      -1,
      -2,
      INT_MAX,
      "Pose",
      "Index of the pose to apply (-2 for no change to pose, -1 for poselib active pose)",
      0,
      INT_MAX);

  /* XXX: percentage vs factor? */
  /* not used yet */
#if 0
  RNA_def_float_factor(ot->srna,
                       "blend_factor",
                       1.0f,
                       0.0f,
                       1.0f,
                       "Blend Factor",
                       "Amount that the pose is applied on top of the existing poses",
                       0.0f,
                       1.0f);
#endif
}

void POSELIB_OT_apply_pose(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Apply Legacy Pose Library Pose";
  ot->idname = "POSELIB_OT_apply_pose";
  ot->description =
      "Deprecated, will be removed in Blender 3.3. "
      "Apply specified Legacy Pose Library pose to the rig";

  /* callbacks */
  ot->exec = poselib_preview_exec;
  ot->poll = has_poselib_pose_data_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  /* TODO: make the pose_index into a proper enum instead of a cryptic int... */
  ot->prop = RNA_def_int(
      ot->srna,
      "pose_index",
      -1,
      -2,
      INT_MAX,
      "Pose",
      "Index of the pose to apply (-2 for no change to pose, -1 for poselib active pose)",
      0,
      INT_MAX);
}

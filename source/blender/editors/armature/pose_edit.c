/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup edarmature
 * Pose Mode API's and Operators for Pose Mode armatures.
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_anim_visualization.h"
#include "BKE_armature.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_global.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"
#include "RNA_prototypes.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_anim_api.h"
#include "ED_armature.h"
#include "ED_keyframing.h"
#include "ED_object.h"
#include "ED_screen.h"
#include "ED_view3d.h"

#include "UI_interface.h"

#include "armature_intern.h"

#undef DEBUG_TIME

#include "PIL_time.h"
#ifdef DEBUG_TIME
#  include "PIL_time_utildefines.h"
#endif

Object *ED_pose_object_from_context(bContext *C)
{
  /* NOTE: matches logic with #ED_operator_posemode_context(). */

  ScrArea *area = CTX_wm_area(C);
  Object *ob;

  /* Since this call may also be used from the buttons window,
   * we need to check for where to get the object. */
  if (area && area->spacetype == SPACE_PROPERTIES) {
    ob = ED_object_context(C);
  }
  else {
    ob = BKE_object_pose_armature_get(CTX_data_active_object(C));
  }

  return ob;
}

bool ED_object_posemode_enter_ex(struct Main *bmain, Object *ob)
{
  BLI_assert(BKE_id_is_editable(bmain, &ob->id));
  bool ok = false;

  switch (ob->type) {
    case OB_ARMATURE:
      ob->restore_mode = ob->mode;
      ob->mode |= OB_MODE_POSE;
      /* Inform all CoW versions that we changed the mode. */
      DEG_id_tag_update_ex(bmain, &ob->id, ID_RECALC_COPY_ON_WRITE);
      ok = true;

      break;
    default:
      break;
  }

  return ok;
}
bool ED_object_posemode_enter(bContext *C, Object *ob)
{
  ReportList *reports = CTX_wm_reports(C);
  struct Main *bmain = CTX_data_main(C);
  if (!BKE_id_is_editable(bmain, &ob->id)) {
    BKE_report(reports, RPT_WARNING, "Cannot pose libdata");
    return false;
  }
  bool ok = ED_object_posemode_enter_ex(bmain, ob);
  if (ok) {
    WM_event_add_notifier(C, NC_SCENE | ND_MODE | NS_MODE_POSE, NULL);
  }
  return ok;
}

bool ED_object_posemode_exit_ex(struct Main *bmain, Object *ob)
{
  bool ok = false;
  if (ob) {
    ob->restore_mode = ob->mode;
    ob->mode &= ~OB_MODE_POSE;

    /* Inform all CoW versions that we changed the mode. */
    DEG_id_tag_update_ex(bmain, &ob->id, ID_RECALC_COPY_ON_WRITE);
    ok = true;
  }
  return ok;
}
bool ED_object_posemode_exit(bContext *C, Object *ob)
{
  struct Main *bmain = CTX_data_main(C);
  bool ok = ED_object_posemode_exit_ex(bmain, ob);
  if (ok) {
    WM_event_add_notifier(C, NC_SCENE | ND_MODE | NS_MODE_OBJECT, NULL);
  }
  return ok;
}

/* ********************************************** */
/* Motion Paths */

static eAnimvizCalcRange pose_path_convert_range(ePosePathCalcRange range)
{
  switch (range) {
    case POSE_PATH_CALC_RANGE_CURRENT_FRAME:
      return ANIMVIZ_CALC_RANGE_CURRENT_FRAME;
    case POSE_PATH_CALC_RANGE_CHANGED:
      return ANIMVIZ_CALC_RANGE_CHANGED;
    case POSE_PATH_CALC_RANGE_FULL:
      return ANIMVIZ_CALC_RANGE_FULL;
  }
  return ANIMVIZ_CALC_RANGE_FULL;
}

void ED_pose_recalculate_paths(bContext *C, Scene *scene, Object *ob, ePosePathCalcRange range)
{
  /* Transform doesn't always have context available to do update. */
  if (C == NULL) {
    return;
  }

  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  Depsgraph *depsgraph;
  bool free_depsgraph = false;

  ListBase targets = {NULL, NULL};
  /* set flag to force recalc, then grab the relevant bones to target */
  ob->pose->avs.recalc |= ANIMVIZ_RECALC_PATHS;
  animviz_get_object_motionpaths(ob, &targets);

  /* recalculate paths, then free */
#ifdef DEBUG_TIME
  TIMEIT_START(pose_path_calc);
#endif

  /* For a single frame update it's faster to re-use existing dependency graph and avoid overhead
   * of building all the relations and so on for a temporary one. */
  if (range == POSE_PATH_CALC_RANGE_CURRENT_FRAME) {
    /* NOTE: Dependency graph will be evaluated at all the frames, but we first need to access some
     * nested pointers, like animation data. */
    depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    free_depsgraph = false;
  }
  else {
    depsgraph = animviz_depsgraph_build(bmain, scene, view_layer, &targets);
    free_depsgraph = true;
  }

  animviz_calc_motionpaths(
      depsgraph, bmain, scene, &targets, pose_path_convert_range(range), !free_depsgraph);

#ifdef DEBUG_TIME
  TIMEIT_END(pose_path_calc);
#endif

  BLI_freelistN(&targets);

  if (range != POSE_PATH_CALC_RANGE_CURRENT_FRAME) {
    /* Tag armature object for copy on write - so paths will draw/redraw.
     * For currently frame only we update evaluated object directly. */
    DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);
  }

  /* Free temporary depsgraph. */
  if (free_depsgraph) {
    DEG_graph_free(depsgraph);
  }
}

/* show popup to determine settings */
static int pose_calculate_paths_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
  Object *ob = BKE_object_pose_armature_get(CTX_data_active_object(C));

  if (ELEM(NULL, ob, ob->pose)) {
    return OPERATOR_CANCELLED;
  }

  /* set default settings from existing/stored settings */
  {
    bAnimVizSettings *avs = &ob->pose->avs;
    PointerRNA avs_ptr;

    RNA_pointer_create(NULL, &RNA_AnimVizMotionPaths, avs, &avs_ptr);
    RNA_enum_set(op->ptr, "display_type", RNA_enum_get(&avs_ptr, "type"));
    RNA_enum_set(op->ptr, "range", RNA_enum_get(&avs_ptr, "range"));
    RNA_enum_set(op->ptr, "bake_location", RNA_enum_get(&avs_ptr, "bake_location"));
  }

  /* show popup dialog to allow editing of range... */
  /* FIXME: hard-coded dimensions here are just arbitrary. */
  return WM_operator_props_dialog_popup(C, op, 270);
}

/* For the object with pose/action: create path curves for selected bones
 * This recalculates the WHOLE path within the pchan->pathsf and pchan->pathef range
 */
static int pose_calculate_paths_exec(bContext *C, wmOperator *op)
{
  Object *ob = BKE_object_pose_armature_get(CTX_data_active_object(C));
  Scene *scene = CTX_data_scene(C);

  if (ELEM(NULL, ob, ob->pose)) {
    return OPERATOR_CANCELLED;
  }

  /* grab baking settings from operator settings */
  {
    bAnimVizSettings *avs = &ob->pose->avs;
    PointerRNA avs_ptr;

    avs->path_type = RNA_enum_get(op->ptr, "display_type");
    avs->path_range = RNA_enum_get(op->ptr, "range");
    animviz_motionpath_compute_range(ob, scene);

    RNA_pointer_create(NULL, &RNA_AnimVizMotionPaths, avs, &avs_ptr);
    RNA_enum_set(&avs_ptr, "bake_location", RNA_enum_get(op->ptr, "bake_location"));
  }

  /* set up path data for bones being calculated */
  CTX_DATA_BEGIN (C, bPoseChannel *, pchan, selected_pose_bones_from_active_object) {
    /* verify makes sure that the selected bone has a bone with the appropriate settings */
    animviz_verify_motionpaths(op->reports, scene, ob, pchan);
  }
  CTX_DATA_END;

#ifdef DEBUG_TIME
  TIMEIT_START(recalc_pose_paths);
#endif

  /* Calculate the bones that now have motionpaths. */
  /* TODO: only make for the selected bones? */
  ED_pose_recalculate_paths(C, scene, ob, POSE_PATH_CALC_RANGE_FULL);

#ifdef DEBUG_TIME
  TIMEIT_END(recalc_pose_paths);
#endif

  /* notifiers for updates */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);

  return OPERATOR_FINISHED;
}

void POSE_OT_paths_calculate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Calculate Bone Paths";
  ot->idname = "POSE_OT_paths_calculate";
  ot->description = "Calculate paths for the selected bones";

  /* api callbacks */
  ot->invoke = pose_calculate_paths_invoke;
  ot->exec = pose_calculate_paths_exec;
  ot->poll = ED_operator_posemode_exclusive;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_enum(ot->srna,
               "display_type",
               rna_enum_motionpath_display_type_items,
               MOTIONPATH_TYPE_RANGE,
               "Display type",
               "");
  RNA_def_enum(ot->srna,
               "range",
               rna_enum_motionpath_range_items,
               MOTIONPATH_RANGE_SCENE,
               "Computation Range",
               "");

  RNA_def_enum(ot->srna,
               "bake_location",
               rna_enum_motionpath_bake_location_items,
               MOTIONPATH_BAKE_HEADS,
               "Bake Location",
               "Which point on the bones is used when calculating paths");
}

/* --------- */

static bool pose_update_paths_poll(bContext *C)
{
  if (ED_operator_posemode_exclusive(C)) {
    Object *ob = CTX_data_active_object(C);
    return (ob->pose->avs.path_bakeflag & MOTIONPATH_BAKE_HAS_PATHS) != 0;
  }

  return false;
}

static int pose_update_paths_exec(bContext *C, wmOperator *op)
{
  Object *ob = BKE_object_pose_armature_get(CTX_data_active_object(C));
  Scene *scene = CTX_data_scene(C);

  if (ELEM(NULL, ob, scene)) {
    return OPERATOR_CANCELLED;
  }
  animviz_motionpath_compute_range(ob, scene);

  /* set up path data for bones being calculated */
  CTX_DATA_BEGIN (C, bPoseChannel *, pchan, selected_pose_bones_from_active_object) {
    animviz_verify_motionpaths(op->reports, scene, ob, pchan);
  }
  CTX_DATA_END;

  /* Calculate the bones that now have motion-paths. */
  /* TODO: only make for the selected bones? */
  ED_pose_recalculate_paths(C, scene, ob, POSE_PATH_CALC_RANGE_FULL);

  /* notifiers for updates */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);

  return OPERATOR_FINISHED;
}

void POSE_OT_paths_update(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Update Bone Paths";
  ot->idname = "POSE_OT_paths_update";
  ot->description = "Recalculate paths for bones that already have them";

  /* api callbacks */
  ot->exec = pose_update_paths_exec;
  ot->poll = pose_update_paths_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* --------- */

/* for the object with pose/action: clear path curves for selected bones only */
static void ED_pose_clear_paths(Object *ob, bool only_selected)
{
  bPoseChannel *pchan;
  bool skipped = false;

  if (ELEM(NULL, ob, ob->pose)) {
    return;
  }

  /* free the motionpath blocks for all bones - This is easier for users to quickly clear all */
  for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
    if (pchan->mpath) {
      if ((only_selected == false) || ((pchan->bone) && (pchan->bone->flag & BONE_SELECTED))) {
        animviz_free_motionpath(pchan->mpath);
        pchan->mpath = NULL;
      }
      else {
        skipped = true;
      }
    }
  }

  /* if nothing was skipped, there should be no paths left! */
  if (skipped == false) {
    ob->pose->avs.path_bakeflag &= ~MOTIONPATH_BAKE_HAS_PATHS;
  }

  /* tag armature object for copy on write - so removed paths don't still show */
  DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);
}

/* Operator callback - wrapper for the back-end function. */
static int pose_clear_paths_exec(bContext *C, wmOperator *op)
{
  Object *ob = BKE_object_pose_armature_get(CTX_data_active_object(C));
  bool only_selected = RNA_boolean_get(op->ptr, "only_selected");

  /* only continue if there's an object */
  if (ELEM(NULL, ob, ob->pose)) {
    return OPERATOR_CANCELLED;
  }

  /* use the backend function for this */
  ED_pose_clear_paths(ob, only_selected);

  /* notifiers for updates */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);

  return OPERATOR_FINISHED;
}

/* operator callback/wrapper */
static int pose_clear_paths_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if ((event->modifier & KM_SHIFT) && !RNA_struct_property_is_set(op->ptr, "only_selected")) {
    RNA_boolean_set(op->ptr, "only_selected", true);
  }
  return pose_clear_paths_exec(C, op);
}

void POSE_OT_paths_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Bone Paths";
  ot->idname = "POSE_OT_paths_clear";
  ot->description = "Clear path caches for all bones, hold Shift key for selected bones only";

  /* api callbacks */
  ot->invoke = pose_clear_paths_invoke;
  ot->exec = pose_clear_paths_exec;
  ot->poll = ED_operator_posemode_exclusive;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_boolean(
      ot->srna, "only_selected", false, "Only Selected", "Only clear paths from selected bones");
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE);
}

/* --------- */

static int pose_update_paths_range_exec(bContext *C, wmOperator *UNUSED(op))
{
  Scene *scene = CTX_data_scene(C);
  Object *ob = BKE_object_pose_armature_get(CTX_data_active_object(C));

  if (ELEM(NULL, scene, ob, ob->pose)) {
    return OPERATOR_CANCELLED;
  }

  /* use Preview Range or Full Frame Range - whichever is in use */
  ob->pose->avs.path_sf = PSFRA;
  ob->pose->avs.path_ef = PEFRA;

  /* tag for updates */
  DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);

  return OPERATOR_FINISHED;
}

void POSE_OT_paths_range_update(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Update Range from Scene";
  ot->idname = "POSE_OT_paths_range_update";
  ot->description = "Update frame range for motion paths from the Scene's current frame range";

  /* callbacks */
  ot->exec = pose_update_paths_range_exec;
  ot->poll = ED_operator_posemode_exclusive;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ********************************************** */

static int pose_flip_names_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  const bool do_strip_numbers = RNA_boolean_get(op->ptr, "do_strip_numbers");

  FOREACH_OBJECT_IN_MODE_BEGIN (view_layer, v3d, OB_ARMATURE, OB_MODE_POSE, ob) {
    bArmature *arm = ob->data;
    ListBase bones_names = {NULL};

    FOREACH_PCHAN_SELECTED_IN_OBJECT_BEGIN (ob, pchan) {
      BLI_addtail(&bones_names, BLI_genericNodeN(pchan->name));
    }
    FOREACH_PCHAN_SELECTED_IN_OBJECT_END;

    ED_armature_bones_flip_names(bmain, arm, &bones_names, do_strip_numbers);

    BLI_freelistN(&bones_names);

    /* since we renamed stuff... */
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);

    /* NOTE: notifier might evolve. */
    WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);
  }
  FOREACH_OBJECT_IN_MODE_END;

  return OPERATOR_FINISHED;
}

void POSE_OT_flip_names(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Flip Names";
  ot->idname = "POSE_OT_flip_names";
  ot->description = "Flips (and corrects) the axis suffixes of the names of selected bones";

  /* api callbacks */
  ot->exec = pose_flip_names_exec;
  ot->poll = ED_operator_posemode_local;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "do_strip_numbers",
                  false,
                  "Strip Numbers",
                  "Try to remove right-most dot-number from flipped names.\n"
                  "Warning: May result in incoherent naming in some cases");
}

/* ------------------ */

static int pose_autoside_names_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  char newname[MAXBONENAME];
  short axis = RNA_enum_get(op->ptr, "axis");
  Object *ob_prev = NULL;

  /* loop through selected bones, auto-naming them */
  CTX_DATA_BEGIN_WITH_ID (C, bPoseChannel *, pchan, selected_pose_bones, Object *, ob) {
    bArmature *arm = ob->data;
    BLI_strncpy(newname, pchan->name, sizeof(newname));
    if (bone_autoside_name(newname, 1, axis, pchan->bone->head[axis], pchan->bone->tail[axis])) {
      ED_armature_bone_rename(bmain, arm, pchan->name, newname);
    }

    if (ob_prev != ob) {
      /* since we renamed stuff... */
      DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);

      /* NOTE: notifier might evolve. */
      WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);
      ob_prev = ob;
    }
  }
  CTX_DATA_END;

  return OPERATOR_FINISHED;
}

void POSE_OT_autoside_names(wmOperatorType *ot)
{
  static const EnumPropertyItem axis_items[] = {
      {0, "XAXIS", 0, "X-Axis", "Left/Right"},
      {1, "YAXIS", 0, "Y-Axis", "Front/Back"},
      {2, "ZAXIS", 0, "Z-Axis", "Top/Bottom"},
      {0, NULL, 0, NULL, NULL},
  };

  /* identifiers */
  ot->name = "Auto-Name by Axis";
  ot->idname = "POSE_OT_autoside_names";
  ot->description =
      "Automatically renames the selected bones according to which side of the target axis they "
      "fall on";

  /* api callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = pose_autoside_names_exec;
  ot->poll = ED_operator_posemode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* settings */
  ot->prop = RNA_def_enum(ot->srna, "axis", axis_items, 0, "Axis", "Axis tag names with");
}

/* ********************************************** */

static int pose_bone_rotmode_exec(bContext *C, wmOperator *op)
{
  const int mode = RNA_enum_get(op->ptr, "type");
  Object *prev_ob = NULL;

  /* Set rotation mode of selected bones. */
  CTX_DATA_BEGIN_WITH_ID (C, bPoseChannel *, pchan, selected_pose_bones, Object *, ob) {
    /* use API Method for conversions... */
    BKE_rotMode_change_values(
        pchan->quat, pchan->eul, pchan->rotAxis, &pchan->rotAngle, pchan->rotmode, (short)mode);

    /* finally, set the new rotation type */
    pchan->rotmode = mode;

    if (prev_ob != ob) {
      /* Notifiers and updates. */
      DEG_id_tag_update((ID *)ob, ID_RECALC_GEOMETRY);
      WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, ob);
      WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, ob);
      prev_ob = ob;
    }
  }
  CTX_DATA_END;

  return OPERATOR_FINISHED;
}

void POSE_OT_rotation_mode_set(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Rotation Mode";
  ot->idname = "POSE_OT_rotation_mode_set";
  ot->description = "Set the rotation representation used by selected bones";

  /* callbacks */
  ot->invoke = WM_menu_invoke;
  ot->exec = pose_bone_rotmode_exec;
  ot->poll = ED_operator_posemode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_enum(
      ot->srna, "type", rna_enum_object_rotation_mode_items, 0, "Rotation Mode", "");
}

/* ********************************************** */

static bool armature_layers_poll(bContext *C)
{
  /* Armature layers operators can be used in posemode OR editmode for armatures */
  return ED_operator_posemode(C) || ED_operator_editarmature(C);
}

static bArmature *armature_layers_get_data(Object **ob)
{
  bArmature *arm = NULL;

  /* Sanity checking and handling of posemode. */
  if (*ob) {
    Object *tob = BKE_object_pose_armature_get(*ob);
    if (tob) {
      *ob = tob;
      arm = (*ob)->data;
    }
    else if ((*ob)->type == OB_ARMATURE) {
      arm = (*ob)->data;
    }
  }

  return arm;
}

/* Show all armature layers */

static int pose_armature_layers_showall_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  bArmature *arm = armature_layers_get_data(&ob);
  PointerRNA ptr;
  int maxLayers = (RNA_boolean_get(op->ptr, "all")) ? 32 : 16;
  /* hardcoded for now - we can only have 32 armature layers, so this should be fine... */
  bool layers[32] = {false};

  /* sanity checking */
  if (arm == NULL) {
    return OPERATOR_CANCELLED;
  }

  /* use RNA to set the layers
   * although it would be faster to just set directly using bitflags, we still
   * need to setup a RNA pointer so that we get the "update" callbacks for free...
   */
  RNA_id_pointer_create(&arm->id, &ptr);

  for (int i = 0; i < maxLayers; i++) {
    layers[i] = 1;
  }

  RNA_boolean_set_array(&ptr, "layers", layers);

  /* NOTE: notifier might evolve. */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);
  DEG_id_tag_update(&arm->id, ID_RECALC_COPY_ON_WRITE);

  /* done */
  return OPERATOR_FINISHED;
}

void ARMATURE_OT_layers_show_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Show All Layers";
  ot->idname = "ARMATURE_OT_layers_show_all";
  ot->description = "Make all armature layers visible";

  /* callbacks */
  ot->exec = pose_armature_layers_showall_exec;
  ot->poll = armature_layers_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_boolean(
      ot->srna, "all", 1, "All Layers", "Enable all layers or just the first 16 (top row)");
}

/* ------------------- */

/* Present a popup to get the layers that should be used */
static int armature_layers_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  bArmature *arm = armature_layers_get_data(&ob);
  PointerRNA ptr;
  /* hardcoded for now - we can only have 32 armature layers, so this should be fine... */
  bool layers[32];

  /* sanity checking */
  if (arm == NULL) {
    return OPERATOR_CANCELLED;
  }

  /* Get RNA pointer to armature data to use that to retrieve the layers as ints
   * to init the operator. */
  RNA_id_pointer_create((ID *)arm, &ptr);
  RNA_boolean_get_array(&ptr, "layers", layers);
  RNA_boolean_set_array(op->ptr, "layers", layers);

  /* part to sync with other similar operators... */
  return WM_operator_props_popup(C, op, event);
}

/* Set the visible layers for the active armature (edit and pose modes) */
static int armature_layers_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  bArmature *arm = armature_layers_get_data(&ob);
  PointerRNA ptr;
  /* hardcoded for now - we can only have 32 armature layers, so this should be fine... */
  bool layers[32];

  if (arm == NULL) {
    return OPERATOR_CANCELLED;
  }

  /* get the values set in the operator properties */
  RNA_boolean_get_array(op->ptr, "layers", layers);

  /* get pointer for armature, and write data there... */
  RNA_id_pointer_create((ID *)arm, &ptr);
  RNA_boolean_set_array(&ptr, "layers", layers);

  /* NOTE: notifier might evolve. */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);
  DEG_id_tag_update(&arm->id, ID_RECALC_COPY_ON_WRITE);

  return OPERATOR_FINISHED;
}

void ARMATURE_OT_armature_layers(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Change Armature Layers";
  ot->idname = "ARMATURE_OT_armature_layers";
  ot->description = "Change the visible armature layers";

  /* callbacks */
  ot->invoke = armature_layers_invoke;
  ot->exec = armature_layers_exec;
  ot->poll = armature_layers_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean_layer_member(
      ot->srna, "layers", 32, NULL, "Layer", "Armature layers to make visible");
}

/* ------------------- */

/* Present a popup to get the layers that should be used */
static int pose_bone_layers_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* hardcoded for now - we can only have 32 armature layers, so this should be fine... */
  bool layers[32] = {0};

  /* get layers that are active already */
  CTX_DATA_BEGIN (C, bPoseChannel *, pchan, selected_pose_bones) {
    short bit;

    /* loop over the bits for this pchan's layers, adding layers where they're needed */
    for (bit = 0; bit < 32; bit++) {
      layers[bit] = (pchan->bone->layer & (1u << bit)) != 0;
    }
  }
  CTX_DATA_END;

  /* copy layers to operator */
  RNA_boolean_set_array(op->ptr, "layers", layers);

  /* part to sync with other similar operators... */
  return WM_operator_props_popup(C, op, event);
}

/* Set the visible layers for the active armature (edit and pose modes) */
static int pose_bone_layers_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr;
  /* hardcoded for now - we can only have 32 armature layers, so this should be fine... */
  bool layers[32];

  /* get the values set in the operator properties */
  RNA_boolean_get_array(op->ptr, "layers", layers);

  Object *prev_ob = NULL;

  /* Make sure that the pose bone data is up to date.
   * (May not always be the case after undo/redo e.g.).
   */
  struct Main *bmain = CTX_data_main(C);
  wmWindow *win = CTX_wm_window(C);
  View3D *v3d = CTX_wm_view3d(C); /* This may be NULL in a lot of cases. */
  ViewLayer *view_layer = WM_window_get_active_view_layer(win);

  FOREACH_OBJECT_IN_MODE_BEGIN (view_layer, v3d, OB_ARMATURE, OB_MODE_POSE, ob_iter) {
    bArmature *arm = ob_iter->data;
    BKE_pose_ensure(bmain, ob_iter, arm, true);
  }
  FOREACH_OBJECT_IN_MODE_END;

  /* set layers of pchans based on the values set in the operator props */
  CTX_DATA_BEGIN_WITH_ID (C, bPoseChannel *, pchan, selected_pose_bones, Object *, ob) {
    /* get pointer for pchan, and write flags this way */
    RNA_pointer_create((ID *)ob->data, &RNA_Bone, pchan->bone, &ptr);
    RNA_boolean_set_array(&ptr, "layers", layers);

    if (prev_ob != ob) {
      /* NOTE: notifier might evolve. */
      WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);
      DEG_id_tag_update((ID *)ob->data, ID_RECALC_COPY_ON_WRITE);
      prev_ob = ob;
    }
  }
  CTX_DATA_END;
  return OPERATOR_FINISHED;
}

void POSE_OT_bone_layers(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Change Bone Layers";
  ot->idname = "POSE_OT_bone_layers";
  ot->description = "Change the layers that the selected bones belong to";

  /* callbacks */
  ot->invoke = pose_bone_layers_invoke;
  ot->exec = pose_bone_layers_exec;
  ot->poll = ED_operator_posemode_exclusive;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean_layer_member(
      ot->srna, "layers", 32, NULL, "Layer", "Armature layers that bone belongs to");
}

/* ------------------- */

/* Present a popup to get the layers that should be used */
static int armature_bone_layers_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* hardcoded for now - we can only have 32 armature layers, so this should be fine... */
  bool layers[32] = {0};

  /* get layers that are active already */
  CTX_DATA_BEGIN (C, EditBone *, ebone, selected_editable_bones) {
    short bit;

    /* loop over the bits for this pchan's layers, adding layers where they're needed */
    for (bit = 0; bit < 32; bit++) {
      if (ebone->layer & (1u << bit)) {
        layers[bit] = 1;
      }
    }
  }
  CTX_DATA_END;

  /* copy layers to operator */
  RNA_boolean_set_array(op->ptr, "layers", layers);

  /* part to sync with other similar operators... */
  return WM_operator_props_popup(C, op, event);
}

/* Set the visible layers for the active armature (edit and pose modes) */
static int armature_bone_layers_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_edit_object(C);
  PointerRNA ptr;
  /* hardcoded for now - we can only have 32 armature layers, so this should be fine... */
  bool layers[32];

  /* get the values set in the operator properties */
  RNA_boolean_get_array(op->ptr, "layers", layers);

  /* set layers of pchans based on the values set in the operator props */
  CTX_DATA_BEGIN_WITH_ID (C, EditBone *, ebone, selected_editable_bones, bArmature *, arm) {
    /* get pointer for pchan, and write flags this way */
    RNA_pointer_create((ID *)arm, &RNA_EditBone, ebone, &ptr);
    RNA_boolean_set_array(&ptr, "layers", layers);
  }
  CTX_DATA_END;

  ED_armature_edit_refresh_layer_used(ob->data);

  /* NOTE: notifier might evolve. */
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, ob);

  return OPERATOR_FINISHED;
}

void ARMATURE_OT_bone_layers(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Change Bone Layers";
  ot->idname = "ARMATURE_OT_bone_layers";
  ot->description = "Change the layers that the selected bones belong to";

  /* callbacks */
  ot->invoke = armature_bone_layers_invoke;
  ot->exec = armature_bone_layers_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean_layer_member(
      ot->srna, "layers", 32, NULL, "Layer", "Armature layers that bone belongs to");
}

/* ********************************************** */
/* Show/Hide Bones */

static int hide_pose_bone_fn(Object *ob, Bone *bone, void *ptr)
{
  bArmature *arm = ob->data;
  const bool hide_select = (bool)POINTER_AS_INT(ptr);
  int count = 0;
  if (arm->layer & bone->layer) {
    if (((bone->flag & BONE_SELECTED) != 0) == hide_select) {
      bone->flag |= BONE_HIDDEN_P;
      /* only needed when 'hide_select' is true, but harmless. */
      bone->flag &= ~BONE_SELECTED;
      if (arm->act_bone == bone) {
        arm->act_bone = NULL;
      }
      count += 1;
    }
  }
  return count;
}

/* active object is armature in posemode, poll checked */
static int pose_hide_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len;
  Object **objects = BKE_object_pose_array_get_unique(view_layer, CTX_wm_view3d(C), &objects_len);
  bool changed_multi = false;

  const int hide_select = !RNA_boolean_get(op->ptr, "unselected");
  void *hide_select_p = POINTER_FROM_INT(hide_select);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *ob_iter = objects[ob_index];
    bArmature *arm = ob_iter->data;

    bool changed = bone_looper(ob_iter, arm->bonebase.first, hide_select_p, hide_pose_bone_fn) !=
                   0;
    if (changed) {
      changed_multi = true;
      WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, ob_iter);
      DEG_id_tag_update(&arm->id, ID_RECALC_COPY_ON_WRITE);
    }
  }
  MEM_freeN(objects);

  return changed_multi ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void POSE_OT_hide(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Hide Selected";
  ot->idname = "POSE_OT_hide";
  ot->description = "Tag selected bones to not be visible in Pose Mode";

  /* api callbacks */
  ot->exec = pose_hide_exec;
  ot->poll = ED_operator_posemode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna, "unselected", 0, "Unselected", "");
}

static int show_pose_bone_cb(Object *ob, Bone *bone, void *data)
{
  const bool select = POINTER_AS_INT(data);

  bArmature *arm = ob->data;
  int count = 0;
  if (arm->layer & bone->layer) {
    if (bone->flag & BONE_HIDDEN_P) {
      if (!(bone->flag & BONE_UNSELECTABLE)) {
        SET_FLAG_FROM_TEST(bone->flag, select, BONE_SELECTED);
      }
      bone->flag &= ~BONE_HIDDEN_P;
      count += 1;
    }
  }

  return count;
}

/* active object is armature in posemode, poll checked */
static int pose_reveal_exec(bContext *C, wmOperator *op)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  uint objects_len;
  Object **objects = BKE_object_pose_array_get_unique(view_layer, CTX_wm_view3d(C), &objects_len);
  bool changed_multi = false;
  const bool select = RNA_boolean_get(op->ptr, "select");
  void *select_p = POINTER_FROM_INT(select);

  for (uint ob_index = 0; ob_index < objects_len; ob_index++) {
    Object *ob_iter = objects[ob_index];
    bArmature *arm = ob_iter->data;

    bool changed = bone_looper(ob_iter, arm->bonebase.first, select_p, show_pose_bone_cb);
    if (changed) {
      changed_multi = true;
      WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, ob_iter);
      DEG_id_tag_update(&arm->id, ID_RECALC_COPY_ON_WRITE);
    }
  }
  MEM_freeN(objects);

  return changed_multi ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void POSE_OT_reveal(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reveal Selected";
  ot->idname = "POSE_OT_reveal";
  ot->description = "Reveal all bones hidden in Pose Mode";

  /* api callbacks */
  ot->exec = pose_reveal_exec;
  ot->poll = ED_operator_posemode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "select", true, "Select", "");
}

/* ********************************************** */
/* Flip Quats */

static int pose_flip_quats_exec(bContext *C, wmOperator *UNUSED(op))
{
  Scene *scene = CTX_data_scene(C);
  KeyingSet *ks = ANIM_builtin_keyingset_get_named(NULL, ANIM_KS_LOC_ROT_SCALE_ID);

  bool changed_multi = false;

  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  FOREACH_OBJECT_IN_MODE_BEGIN (view_layer, v3d, OB_ARMATURE, OB_MODE_POSE, ob_iter) {
    bool changed = false;
    /* loop through all selected pchans, flipping and keying (as needed) */
    FOREACH_PCHAN_SELECTED_IN_OBJECT_BEGIN (ob_iter, pchan) {
      /* only if bone is using quaternion rotation */
      if (pchan->rotmode == ROT_MODE_QUAT) {
        changed = true;
        /* quaternions have 720 degree range */
        negate_v4(pchan->quat);

        ED_autokeyframe_pchan(C, scene, ob_iter, pchan, ks);
      }
    }
    FOREACH_PCHAN_SELECTED_IN_OBJECT_END;

    if (changed) {
      changed_multi = true;
      /* notifiers and updates */
      DEG_id_tag_update(&ob_iter->id, ID_RECALC_GEOMETRY);
      WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, ob_iter);
    }
  }
  FOREACH_OBJECT_IN_MODE_END;

  return changed_multi ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void POSE_OT_quaternions_flip(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Flip Quats";
  ot->idname = "POSE_OT_quaternions_flip";
  ot->description =
      "Flip quaternion values to achieve desired rotations, while maintaining the same "
      "orientations";

  /* callbacks */
  ot->exec = pose_flip_quats_exec;
  ot->poll = ED_operator_posemode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

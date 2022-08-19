/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <stdlib.h>
#include <string.h>

#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "rna_internal.h"

#include "DNA_action_types.h"
#include "DNA_armature_types.h"
#include "DNA_constraint_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_math.h"

#include "BLT_translation.h"

#include "UI_resources.h"

#include "WM_types.h"

/* Bone and Group Color Sets */
const EnumPropertyItem rna_enum_color_sets_items[] = {
    {0, "DEFAULT", 0, "Default Colors", ""},
    {1, "THEME01", ICON_COLORSET_01_VEC, "01 - Theme Color Set", ""},
    {2, "THEME02", ICON_COLORSET_02_VEC, "02 - Theme Color Set", ""},
    {3, "THEME03", ICON_COLORSET_03_VEC, "03 - Theme Color Set", ""},
    {4, "THEME04", ICON_COLORSET_04_VEC, "04 - Theme Color Set", ""},
    {5, "THEME05", ICON_COLORSET_05_VEC, "05 - Theme Color Set", ""},
    {6, "THEME06", ICON_COLORSET_06_VEC, "06 - Theme Color Set", ""},
    {7, "THEME07", ICON_COLORSET_07_VEC, "07 - Theme Color Set", ""},
    {8, "THEME08", ICON_COLORSET_08_VEC, "08 - Theme Color Set", ""},
    {9, "THEME09", ICON_COLORSET_09_VEC, "09 - Theme Color Set", ""},
    {10, "THEME10", ICON_COLORSET_10_VEC, "10 - Theme Color Set", ""},
    {11, "THEME11", ICON_COLORSET_11_VEC, "11 - Theme Color Set", ""},
    {12, "THEME12", ICON_COLORSET_12_VEC, "12 - Theme Color Set", ""},
    {13, "THEME13", ICON_COLORSET_13_VEC, "13 - Theme Color Set", ""},
    {14, "THEME14", ICON_COLORSET_14_VEC, "14 - Theme Color Set", ""},
    {15, "THEME15", ICON_COLORSET_15_VEC, "15 - Theme Color Set", ""},
    {16, "THEME16", ICON_COLORSET_16_VEC, "16 - Theme Color Set", ""},
    {17, "THEME17", ICON_COLORSET_17_VEC, "17 - Theme Color Set", ""},
    {18, "THEME18", ICON_COLORSET_18_VEC, "18 - Theme Color Set", ""},
    {19, "THEME19", ICON_COLORSET_19_VEC, "19 - Theme Color Set", ""},
    {20, "THEME20", ICON_COLORSET_20_VEC, "20 - Theme Color Set", ""},
    {-1, "CUSTOM", 0, "Custom Color Set", ""},
    {0, NULL, 0, NULL, NULL},
};

#ifdef RNA_RUNTIME

#  include "BLI_ghash.h"
#  include "BLI_string_utils.h"

#  include "BIK_api.h"
#  include "BKE_action.h"
#  include "BKE_armature.h"

#  include "DNA_userdef_types.h"

#  include "MEM_guardedalloc.h"

#  include "BKE_constraint.h"
#  include "BKE_context.h"
#  include "BKE_global.h"
#  include "BKE_idprop.h"

#  include "DEG_depsgraph.h"
#  include "DEG_depsgraph_build.h"

#  include "ED_armature.h"
#  include "ED_object.h"

#  include "WM_api.h"

#  include "RNA_access.h"

static void rna_Pose_update(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  /* XXX when to use this? ob->pose->flag |= (POSE_LOCKED|POSE_DO_UNLOCK); */

  DEG_id_tag_update(ptr->owner_id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_OBJECT | ND_POSE, ptr->owner_id);
}

static void rna_Pose_dependency_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  DEG_relations_tag_update(bmain);

  DEG_id_tag_update(ptr->owner_id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_OBJECT | ND_POSE, ptr->owner_id);
}

static void rna_Pose_IK_update(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  /* XXX when to use this? ob->pose->flag |= (POSE_LOCKED|POSE_DO_UNLOCK); */
  Object *ob = (Object *)ptr->owner_id;

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_OBJECT | ND_POSE, ptr->owner_id);

  BIK_clear_data(ob->pose);
}

static char *rna_Pose_path(const PointerRNA *UNUSED(ptr))
{
  return BLI_strdup("pose");
}

static char *rna_PoseBone_path(const PointerRNA *ptr)
{
  const bPoseChannel *pchan = ptr->data;
  char name_esc[sizeof(pchan->name) * 2];

  BLI_str_escape(name_esc, pchan->name, sizeof(name_esc));
  return BLI_sprintfN("pose.bones[\"%s\"]", name_esc);
}

/* Bone groups only. */

static bool rna_bone_group_poll(Object *ob, ReportList *reports)
{
  if (ID_IS_OVERRIDE_LIBRARY(ob)) {
    BKE_report(reports, RPT_ERROR, "Cannot edit bone groups for proxies or library overrides");
    return false;
  }

  return true;
}

static bActionGroup *rna_bone_group_new(ID *id, bPose *pose, ReportList *reports, const char *name)
{
  if (!rna_bone_group_poll((Object *)id, reports)) {
    return NULL;
  }

  bActionGroup *grp = BKE_pose_add_group(pose, name);
  WM_main_add_notifier(NC_OBJECT | ND_POSE | NA_ADDED, id);
  return grp;
}

static void rna_bone_group_remove(ID *id, bPose *pose, ReportList *reports, PointerRNA *grp_ptr)
{
  if (!rna_bone_group_poll((Object *)id, reports)) {
    return;
  }

  bActionGroup *grp = grp_ptr->data;
  const int grp_idx = BLI_findindex(&pose->agroups, grp);

  if (grp_idx == -1) {
    BKE_reportf(reports, RPT_ERROR, "Bone group '%s' not found in this object", grp->name);
    return;
  }

  BKE_pose_remove_group(pose, grp, grp_idx + 1);
  WM_main_add_notifier(NC_OBJECT | ND_POSE | NA_REMOVED, id);
}

/* shared for actions groups and bone groups */

void rna_ActionGroup_colorset_set(PointerRNA *ptr, int value)
{
  Object *ob = (Object *)ptr->owner_id;
  if (!rna_bone_group_poll(ob, NULL)) {
    return;
  }

  bActionGroup *grp = ptr->data;

  /* ensure only valid values get set */
  if ((value >= -1) && (value < 21)) {
    grp->customCol = value;

    /* sync colors stored with theme colors based on the index specified */
    action_group_colors_sync(grp, NULL);
  }
}

bool rna_ActionGroup_is_custom_colorset_get(PointerRNA *ptr)
{
  bActionGroup *grp = ptr->data;

  return (grp->customCol < 0);
}

static void rna_BoneGroup_name_set(PointerRNA *ptr, const char *value)
{
  Object *ob = (Object *)ptr->owner_id;
  if (!rna_bone_group_poll(ob, NULL)) {
    return;
  }

  bActionGroup *agrp = ptr->data;

  /* copy the new name into the name slot */
  BLI_strncpy_utf8(agrp->name, value, sizeof(agrp->name));

  BLI_uniquename(&ob->pose->agroups,
                 agrp,
                 CTX_DATA_(BLT_I18NCONTEXT_ID_ARMATURE, "Group"),
                 '.',
                 offsetof(bActionGroup, name),
                 sizeof(agrp->name));
}

static IDProperty **rna_PoseBone_idprops(PointerRNA *ptr)
{
  bPoseChannel *pchan = ptr->data;
  return &pchan->prop;
}

static void rna_Pose_ik_solver_set(struct PointerRNA *ptr, int value)
{
  bPose *pose = (bPose *)ptr->data;

  if (pose->iksolver != value) {
    /* the solver has changed, must clean any temporary structures */
    BIK_clear_data(pose);
    if (pose->ikparam) {
      MEM_freeN(pose->ikparam);
      pose->ikparam = NULL;
    }
    pose->iksolver = value;
    BKE_pose_ikparam_init(pose);
  }
}

static void rna_Pose_ik_solver_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  bPose *pose = ptr->data;

  BKE_pose_tag_recalc(bmain, pose); /* checks & sorts pose channels */
  DEG_relations_tag_update(bmain);

  BKE_pose_update_constraint_flags(pose);

  object_test_constraints(bmain, ob);

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY | ID_RECALC_TRANSFORM);
}

/* rotation - axis-angle */
static void rna_PoseChannel_rotation_axis_angle_get(PointerRNA *ptr, float *value)
{
  bPoseChannel *pchan = ptr->data;

  /* for now, assume that rotation mode is axis-angle */
  value[0] = pchan->rotAngle;
  copy_v3_v3(&value[1], pchan->rotAxis);
}

/* rotation - axis-angle */
static void rna_PoseChannel_rotation_axis_angle_set(PointerRNA *ptr, const float *value)
{
  bPoseChannel *pchan = ptr->data;

  /* for now, assume that rotation mode is axis-angle */
  pchan->rotAngle = value[0];
  copy_v3_v3(pchan->rotAxis, &value[1]);

  /* TODO: validate axis? */
}

static void rna_PoseChannel_rotation_mode_set(PointerRNA *ptr, int value)
{
  bPoseChannel *pchan = ptr->data;

  /* use API Method for conversions... */
  BKE_rotMode_change_values(
      pchan->quat, pchan->eul, pchan->rotAxis, &pchan->rotAngle, pchan->rotmode, (short)value);

  /* finally, set the new rotation type */
  pchan->rotmode = value;
}

static float rna_PoseChannel_length_get(PointerRNA *ptr)
{
  bPoseChannel *pchan = ptr->data;
  return len_v3v3(pchan->pose_head, pchan->pose_tail);
}

static void rna_PoseChannel_name_set(PointerRNA *ptr, const char *value)
{
  Object *ob = (Object *)ptr->owner_id;
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  char oldname[sizeof(pchan->name)], newname[sizeof(pchan->name)];

  /* need to be on the stack */
  BLI_strncpy_utf8(newname, value, sizeof(pchan->name));
  BLI_strncpy(oldname, pchan->name, sizeof(pchan->name));

  BLI_assert(BKE_id_is_in_global_main(&ob->id));
  BLI_assert(BKE_id_is_in_global_main(ob->data));
  ED_armature_bone_rename(G_MAIN, ob->data, oldname, newname);
}

/* See rna_Bone_update_renamed() */
static void rna_PoseChannel_name_update(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  ID *id = ptr->owner_id;

  /* redraw view */
  WM_main_add_notifier(NC_GEOM | ND_DATA, id);

  /* update animation channels */
  WM_main_add_notifier(NC_ANIMATION | ND_ANIMCHAN, id);
}

static PointerRNA rna_PoseChannel_bone_get(PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  PointerRNA tmp_ptr = *ptr;

  /* Replace the id_data pointer with the Armature ID. */
  tmp_ptr.owner_id = ob->data;

  return rna_pointer_inherit_refine(&tmp_ptr, &RNA_Bone, pchan->bone);
}

static bool rna_PoseChannel_has_ik_get(PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;

  return BKE_pose_channel_in_IK_chain(ob, pchan);
}

static StructRNA *rna_IKParam_refine(PointerRNA *ptr)
{
  bIKParam *param = (bIKParam *)ptr->data;

  switch (param->iksolver) {
    case IKSOLVER_ITASC:
      return &RNA_Itasc;
    default:
      return &RNA_IKParam;
  }
}

static PointerRNA rna_Pose_ikparam_get(struct PointerRNA *ptr)
{
  bPose *pose = (bPose *)ptr->data;
  return rna_pointer_inherit_refine(ptr, &RNA_IKParam, pose->ikparam);
}

static StructRNA *rna_Pose_ikparam_typef(PointerRNA *ptr)
{
  bPose *pose = (bPose *)ptr->data;

  switch (pose->iksolver) {
    case IKSOLVER_ITASC:
      return &RNA_Itasc;
    default:
      return &RNA_IKParam;
  }
}

static void rna_Itasc_update(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  bItasc *itasc = ptr->data;

  /* verify values */
  if (itasc->precision < 0.0001f) {
    itasc->precision = 0.0001f;
  }
  if (itasc->minstep < 0.001f) {
    itasc->minstep = 0.001f;
  }
  if (itasc->maxstep < itasc->minstep) {
    itasc->maxstep = itasc->minstep;
  }
  if (itasc->feedback < 0.01f) {
    itasc->feedback = 0.01f;
  }
  if (itasc->feedback > 100.0f) {
    itasc->feedback = 100.0f;
  }
  if (itasc->maxvel < 0.01f) {
    itasc->maxvel = 0.01f;
  }
  if (itasc->maxvel > 100.0f) {
    itasc->maxvel = 100.0f;
  }
  BIK_update_param(ob->pose);

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
}

static void rna_Itasc_update_rebuild(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  bPose *pose = ob->pose;

  BKE_pose_tag_recalc(bmain, pose); /* checks & sorts pose channels */
  rna_Itasc_update(bmain, scene, ptr);
}

static PointerRNA rna_PoseChannel_bone_group_get(PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  bPose *pose = (ob) ? ob->pose : NULL;
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  bActionGroup *grp;

  if (pose) {
    grp = BLI_findlink(&pose->agroups, pchan->agrp_index - 1);
  }
  else {
    grp = NULL;
  }

  return rna_pointer_inherit_refine(ptr, &RNA_BoneGroup, grp);
}

static void rna_PoseChannel_bone_group_set(PointerRNA *ptr,
                                           PointerRNA value,
                                           struct ReportList *UNUSED(reports))
{
  Object *ob = (Object *)ptr->owner_id;
  bPose *pose = (ob) ? ob->pose : NULL;
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;

  if (pose) {
    pchan->agrp_index = BLI_findindex(&pose->agroups, value.data) + 1;
  }
  else {
    pchan->agrp_index = 0;
  }
}

static int rna_PoseChannel_bone_group_index_get(PointerRNA *ptr)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  return MAX2(pchan->agrp_index - 1, 0);
}

static void rna_PoseChannel_bone_group_index_set(PointerRNA *ptr, int value)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  pchan->agrp_index = value + 1;
}

static void rna_PoseChannel_bone_group_index_range(
    PointerRNA *ptr, int *min, int *max, int *UNUSED(softmin), int *UNUSED(softmax))
{
  Object *ob = (Object *)ptr->owner_id;
  bPose *pose = (ob) ? ob->pose : NULL;

  *min = 0;
  *max = pose ? max_ii(0, BLI_listbase_count(&pose->agroups) - 1) : 0;
}

static PointerRNA rna_Pose_active_bone_group_get(PointerRNA *ptr)
{
  bPose *pose = (bPose *)ptr->data;
  return rna_pointer_inherit_refine(
      ptr, &RNA_BoneGroup, BLI_findlink(&pose->agroups, pose->active_group - 1));
}

static void rna_Pose_active_bone_group_set(PointerRNA *ptr,
                                           PointerRNA value,
                                           struct ReportList *UNUSED(reports))
{
  bPose *pose = (bPose *)ptr->data;
  pose->active_group = BLI_findindex(&pose->agroups, value.data) + 1;
}

static int rna_Pose_active_bone_group_index_get(PointerRNA *ptr)
{
  bPose *pose = (bPose *)ptr->data;
  return MAX2(pose->active_group - 1, 0);
}

static void rna_Pose_active_bone_group_index_set(PointerRNA *ptr, int value)
{
  bPose *pose = (bPose *)ptr->data;
  pose->active_group = value + 1;
}

static void rna_Pose_active_bone_group_index_range(
    PointerRNA *ptr, int *min, int *max, int *UNUSED(softmin), int *UNUSED(softmax))
{
  bPose *pose = (bPose *)ptr->data;

  *min = 0;
  *max = max_ii(0, BLI_listbase_count(&pose->agroups) - 1);
}

#  if 0
static void rna_pose_bgroup_name_index_get(PointerRNA *ptr, char *value, int index)
{
  bPose *pose = (bPose *)ptr->data;
  bActionGroup *grp;

  grp = BLI_findlink(&pose->agroups, index - 1);

  if (grp) {
    BLI_strncpy(value, grp->name, sizeof(grp->name));
  }
  else {
    value[0] = '\0';
  }
}

static int rna_pose_bgroup_name_index_length(PointerRNA *ptr, int index)
{
  bPose *pose = (bPose *)ptr->data;
  bActionGroup *grp;

  grp = BLI_findlink(&pose->agroups, index - 1);
  return (grp) ? strlen(grp->name) : 0;
}

static void rna_pose_bgroup_name_index_set(PointerRNA *ptr, const char *value, short *index)
{
  bPose *pose = (bPose *)ptr->data;
  bActionGroup *grp;
  int a;

  for (a = 1, grp = pose->agroups.first; grp; grp = grp->next, a++) {
    if (STREQ(grp->name, value)) {
      *index = a;
      return;
    }
  }

  *index = 0;
}

static void rna_pose_pgroup_name_set(PointerRNA *ptr, const char *value, char *result, int maxlen)
{
  bPose *pose = (bPose *)ptr->data;
  bActionGroup *grp;

  for (grp = pose->agroups.first; grp; grp = grp->next) {
    if (STREQ(grp->name, value)) {
      BLI_strncpy(result, value, maxlen);
      return;
    }
  }

  result[0] = '\0';
}
#  endif

static PointerRNA rna_PoseChannel_active_constraint_get(PointerRNA *ptr)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  bConstraint *con = BKE_constraints_active_get(&pchan->constraints);
  return rna_pointer_inherit_refine(ptr, &RNA_Constraint, con);
}

static void rna_PoseChannel_active_constraint_set(PointerRNA *ptr,
                                                  PointerRNA value,
                                                  struct ReportList *UNUSED(reports))
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  BKE_constraints_active_set(&pchan->constraints, (bConstraint *)value.data);
}

static bConstraint *rna_PoseChannel_constraints_new(ID *id,
                                                    bPoseChannel *pchan,
                                                    Main *main,
                                                    int type)
{
  Object *ob = (Object *)id;
  bConstraint *new_con = BKE_constraint_add_for_pose(ob, pchan, NULL, type);

  ED_object_constraint_dependency_tag_update(main, ob, new_con);
  WM_main_add_notifier(NC_OBJECT | ND_CONSTRAINT | NA_ADDED, id);

  return new_con;
}

static void rna_PoseChannel_constraints_remove(
    ID *id, bPoseChannel *pchan, Main *bmain, ReportList *reports, PointerRNA *con_ptr)
{
  bConstraint *con = con_ptr->data;
  const bool is_ik = ELEM(con->type, CONSTRAINT_TYPE_KINEMATIC, CONSTRAINT_TYPE_SPLINEIK);
  Object *ob = (Object *)id;

  if (BLI_findindex(&pchan->constraints, con) == -1) {
    BKE_reportf(
        reports, RPT_ERROR, "Constraint '%s' not found in pose bone '%s'", con->name, pchan->name);
    return;
  }

  BKE_constraint_remove(&pchan->constraints, con);
  RNA_POINTER_INVALIDATE(con_ptr);

  ED_object_constraint_update(bmain, ob);

  /* XXX(@campbellbarton): is this really needed? */
  BKE_constraints_active_set(&pchan->constraints, NULL);

  WM_main_add_notifier(NC_OBJECT | ND_CONSTRAINT | NA_REMOVED, id);

  if (is_ik) {
    BIK_clear_data(ob->pose);
  }
}

static void rna_PoseChannel_constraints_move(
    ID *id, bPoseChannel *pchan, Main *bmain, ReportList *reports, int from, int to)
{
  Object *ob = (Object *)id;

  if (from == to) {
    return;
  }

  if (!BLI_listbase_move_index(&pchan->constraints, from, to)) {
    BKE_reportf(reports, RPT_ERROR, "Could not move constraint from index '%d' to '%d'", from, to);
    return;
  }

  ED_object_constraint_tag_update(bmain, ob, NULL);
  WM_main_add_notifier(NC_OBJECT | ND_CONSTRAINT, ob);
}

static bConstraint *rna_PoseChannel_constraints_copy(ID *id,
                                                     bPoseChannel *pchan,
                                                     Main *bmain,
                                                     PointerRNA *con_ptr)
{
  Object *ob = (Object *)id;
  bConstraint *con = con_ptr->data;
  bConstraint *new_con = BKE_constraint_copy_for_pose(ob, pchan, con);
  new_con->flag |= CONSTRAINT_OVERRIDE_LIBRARY_LOCAL;

  ED_object_constraint_dependency_tag_update(bmain, ob, new_con);
  WM_main_add_notifier(NC_OBJECT | ND_CONSTRAINT | NA_ADDED, id);

  return new_con;
}

bool rna_PoseChannel_constraints_override_apply(Main *bmain,
                                                PointerRNA *ptr_dst,
                                                PointerRNA *ptr_src,
                                                PointerRNA *UNUSED(ptr_storage),
                                                PropertyRNA *prop_dst,
                                                PropertyRNA *UNUSED(prop_src),
                                                PropertyRNA *UNUSED(prop_storage),
                                                const int UNUSED(len_dst),
                                                const int UNUSED(len_src),
                                                const int UNUSED(len_storage),
                                                PointerRNA *UNUSED(ptr_item_dst),
                                                PointerRNA *UNUSED(ptr_item_src),
                                                PointerRNA *UNUSED(ptr_item_storage),
                                                IDOverrideLibraryPropertyOperation *opop)
{
  BLI_assert(opop->operation == IDOVERRIDE_LIBRARY_OP_INSERT_AFTER &&
             "Unsupported RNA override operation on constraints collection");

  bPoseChannel *pchan_dst = (bPoseChannel *)ptr_dst->data;
  bPoseChannel *pchan_src = (bPoseChannel *)ptr_src->data;

  /* Remember that insertion operations are defined and stored in correct order, which means that
   * even if we insert several items in a row, we always insert first one, then second one, etc.
   * So we should always find 'anchor' constraint in both _src *and* _dst */
  const size_t name_offset = offsetof(bConstraint, name);
  bConstraint *con_anchor = BLI_listbase_string_or_index_find(&pchan_dst->constraints,
                                                              opop->subitem_reference_name,
                                                              name_offset,
                                                              opop->subitem_reference_index);
  /* If `con_anchor` is NULL, `con_src` will be inserted in first position. */

  bConstraint *con_src = BLI_listbase_string_or_index_find(
      &pchan_src->constraints, opop->subitem_local_name, name_offset, opop->subitem_local_index);

  if (con_src == NULL) {
    BLI_assert(con_src != NULL);
    return false;
  }

  bConstraint *con_dst = BKE_constraint_duplicate_ex(con_src, 0, true);

  /* This handles NULL anchor as expected by adding at head of list. */
  BLI_insertlinkafter(&pchan_dst->constraints, con_anchor, con_dst);

  /* This should actually *not* be needed in typical cases.
   * However, if overridden source was edited,
   * we *may* have some new conflicting names. */
  BKE_constraint_unique_name(con_dst, &pchan_dst->constraints);

  //  printf("%s: We inserted a constraint...\n", __func__);
  RNA_property_update_main(bmain, NULL, ptr_dst, prop_dst);
  return true;
}

static int rna_PoseChannel_proxy_editable(PointerRNA *UNUSED(ptr), const char **UNUSED(r_info))
{
#  if 0
  Object *ob = (Object *)ptr->owner_id;
  bArmature *arm = ob->data;
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;

  if (pchan->bone && (pchan->bone->layer & arm->layer_protected)) {
    *r_info = "Can't edit property of a proxy on a protected layer";
    return 0;
  }
#  endif

  return PROP_EDITABLE;
}

static int rna_PoseChannel_location_editable(PointerRNA *ptr, int index)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;

  /* only if the axis in question is locked, not editable... */
  if ((index == 0) && (pchan->protectflag & OB_LOCK_LOCX)) {
    return 0;
  }
  else if ((index == 1) && (pchan->protectflag & OB_LOCK_LOCY)) {
    return 0;
  }
  else if ((index == 2) && (pchan->protectflag & OB_LOCK_LOCZ)) {
    return 0;
  }
  else {
    return PROP_EDITABLE;
  }
}

static int rna_PoseChannel_scale_editable(PointerRNA *ptr, int index)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;

  /* only if the axis in question is locked, not editable... */
  if ((index == 0) && (pchan->protectflag & OB_LOCK_SCALEX)) {
    return 0;
  }
  else if ((index == 1) && (pchan->protectflag & OB_LOCK_SCALEY)) {
    return 0;
  }
  else if ((index == 2) && (pchan->protectflag & OB_LOCK_SCALEZ)) {
    return 0;
  }
  else {
    return PROP_EDITABLE;
  }
}

static int rna_PoseChannel_rotation_euler_editable(PointerRNA *ptr, int index)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;

  /* only if the axis in question is locked, not editable... */
  if ((index == 0) && (pchan->protectflag & OB_LOCK_ROTX)) {
    return 0;
  }
  else if ((index == 1) && (pchan->protectflag & OB_LOCK_ROTY)) {
    return 0;
  }
  else if ((index == 2) && (pchan->protectflag & OB_LOCK_ROTZ)) {
    return 0;
  }
  else {
    return PROP_EDITABLE;
  }
}

static int rna_PoseChannel_rotation_4d_editable(PointerRNA *ptr, int index)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;

  /* only consider locks if locking components individually... */
  if (pchan->protectflag & OB_LOCK_ROT4D) {
    /* only if the axis in question is locked, not editable... */
    if ((index == 0) && (pchan->protectflag & OB_LOCK_ROTW)) {
      return 0;
    }
    else if ((index == 1) && (pchan->protectflag & OB_LOCK_ROTX)) {
      return 0;
    }
    else if ((index == 2) && (pchan->protectflag & OB_LOCK_ROTY)) {
      return 0;
    }
    else if ((index == 3) && (pchan->protectflag & OB_LOCK_ROTZ)) {
      return 0;
    }
  }

  return PROP_EDITABLE;
}

/* not essential, but much faster than the default lookup function */
static int rna_PoseBones_lookup_string(PointerRNA *ptr, const char *key, PointerRNA *r_ptr)
{
  bPose *pose = (bPose *)ptr->data;
  bPoseChannel *pchan = BKE_pose_channel_find_name(pose, key);
  if (pchan) {
    RNA_pointer_create(ptr->owner_id, &RNA_PoseBone, pchan, r_ptr);
    return true;
  }
  else {
    return false;
  }
}

static void rna_PoseChannel_matrix_basis_get(PointerRNA *ptr, float *values)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  BKE_pchan_to_mat4(pchan, (float(*)[4])values);
}

static void rna_PoseChannel_matrix_basis_set(PointerRNA *ptr, const float *values)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  BKE_pchan_apply_mat4(pchan, (float(*)[4])values, false); /* no compat for predictable result */
}

static void rna_PoseChannel_matrix_set(PointerRNA *ptr, const float *values)
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  Object *ob = (Object *)ptr->owner_id;
  float tmat[4][4];

  BKE_armature_mat_pose_to_bone_ex(NULL, ob, pchan, (float(*)[4])values, tmat);

  BKE_pchan_apply_mat4(pchan, tmat, false); /* no compat for predictable result */
}

static bPoseChannel *rna_PoseChannel_ensure_own_pchan(Object *ob,
                                                      Object *ref_ob,
                                                      bPoseChannel *ref_pchan)
{
  if (ref_ob != ob) {
    /* We are trying to set a pchan from another object! Forbidden,
     * try to find by name, or abort. */
    if (ref_pchan != NULL) {
      ref_pchan = BKE_pose_channel_find_name(ob->pose, ref_pchan->name);
    }
  }
  return ref_pchan;
}

static void rna_PoseChannel_custom_shape_transform_set(PointerRNA *ptr,
                                                       PointerRNA value,
                                                       struct ReportList *UNUSED(reports))
{
  bPoseChannel *pchan = (bPoseChannel *)ptr->data;
  Object *ob = (Object *)ptr->owner_id;

  pchan->custom_tx = rna_PoseChannel_ensure_own_pchan(ob, (Object *)value.owner_id, value.data);
}

#else

void rna_def_actionbone_group_common(StructRNA *srna, int update_flag, const char *update_cb)
{
  PropertyRNA *prop;

  /* color set + colors */
  prop = RNA_def_property(srna, "color_set", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "customCol");
  RNA_def_property_enum_items(prop, rna_enum_color_sets_items);
  RNA_def_property_enum_funcs(prop, NULL, "rna_ActionGroup_colorset_set", NULL);
  RNA_def_property_ui_text(prop, "Color Set", "Custom color set to use");
  RNA_def_property_update(prop, update_flag, update_cb);

  prop = RNA_def_property(srna, "is_custom_color_set", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_ActionGroup_is_custom_colorset_get", NULL);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Custom Color Set", "Color set is user-defined instead of a fixed theme color set");

  /* TODO: editing the colors for this should result in changes to the color type... */
  prop = RNA_def_property(srna, "colors", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "ThemeBoneColorSet");
  /* NOTE: the DNA data is not really a pointer, but this code works :) */
  RNA_def_property_pointer_sdna(prop, NULL, "cs");
  RNA_def_property_ui_text(
      prop, "Colors", "Copy of the colors associated with the group's color set");
  RNA_def_property_update(prop, update_flag, update_cb);
}

static void rna_def_bone_group(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* struct */
  srna = RNA_def_struct(brna, "BoneGroup", NULL);
  RNA_def_struct_sdna(srna, "bActionGroup");
  RNA_def_struct_ui_text(srna, "Bone Group", "Groups of Pose Channels (Bones)");
  RNA_def_struct_ui_icon(srna, ICON_GROUP_BONE);

  /* name */
  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_ui_text(prop, "Name", "");
  RNA_def_property_string_funcs(prop, NULL, NULL, "rna_BoneGroup_name_set");
  RNA_def_struct_name_property(srna, prop);

  /* TODO: add some runtime-collections stuff to access grouped bones. */

  /* color set */
  rna_def_actionbone_group_common(srna, NC_OBJECT | ND_POSE, "rna_Pose_update");
}

static const EnumPropertyItem prop_iksolver_items[] = {
    {IKSOLVER_STANDARD, "LEGACY", 0, "Standard", "Original IK solver"},
    {IKSOLVER_ITASC, "ITASC", 0, "iTaSC", "Multi constraint, stateful IK solver"},
    {0, NULL, 0, NULL, NULL},
};

static const EnumPropertyItem prop_solver_items[] = {
    {ITASC_SOLVER_SDLS, "SDLS", 0, "SDLS", "Selective Damped Least Square"},
    {ITASC_SOLVER_DLS, "DLS", 0, "DLS", "Damped Least Square with Numerical Filtering"},
    {0, NULL, 0, NULL, NULL},
};

static void rna_def_pose_channel_constraints(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  PropertyRNA *prop;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "PoseBoneConstraints");
  srna = RNA_def_struct(brna, "PoseBoneConstraints", NULL);
  RNA_def_struct_sdna(srna, "bPoseChannel");
  RNA_def_struct_ui_text(srna, "PoseBone Constraints", "Collection of pose bone constraints");

  /* Collection active property */
  prop = RNA_def_property(srna, "active", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "Constraint");
  RNA_def_property_pointer_funcs(prop,
                                 "rna_PoseChannel_active_constraint_get",
                                 "rna_PoseChannel_active_constraint_set",
                                 NULL,
                                 NULL);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Active Constraint", "Active PoseChannel constraint");

  /* Constraint collection */
  func = RNA_def_function(srna, "new", "rna_PoseChannel_constraints_new");
  RNA_def_function_ui_description(func, "Add a constraint to this object");
  RNA_def_function_flag(func,
                        FUNC_USE_MAIN | FUNC_USE_SELF_ID); /* ID and Main needed for refresh */
  /* return type */
  parm = RNA_def_pointer(func, "constraint", "Constraint", "", "New constraint");
  RNA_def_function_return(func, parm);
  /* constraint to add */
  parm = RNA_def_enum(
      func, "type", rna_enum_constraint_type_items, 1, "", "Constraint type to add");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);

  func = RNA_def_function(srna, "remove", "rna_PoseChannel_constraints_remove");
  RNA_def_function_ui_description(func, "Remove a constraint from this object");
  RNA_def_function_flag(
      func, FUNC_USE_SELF_ID | FUNC_USE_MAIN | FUNC_USE_REPORTS); /* ID needed for refresh */
  /* constraint to remove */
  parm = RNA_def_pointer(func, "constraint", "Constraint", "", "Removed constraint");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, 0);

  func = RNA_def_function(srna, "move", "rna_PoseChannel_constraints_move");
  RNA_def_function_ui_description(func, "Move a constraint to a different position");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_USE_MAIN | FUNC_USE_REPORTS);
  parm = RNA_def_int(
      func, "from_index", -1, INT_MIN, INT_MAX, "From Index", "Index to move", 0, 10000);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_int(func, "to_index", -1, INT_MIN, INT_MAX, "To Index", "Target index", 0, 10000);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);

  func = RNA_def_function(srna, "copy", "rna_PoseChannel_constraints_copy");
  RNA_def_function_ui_description(func, "Add a new constraint that is a copy of the given one");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_SELF_ID);
  /* constraint to copy */
  parm = RNA_def_pointer(func,
                         "constraint",
                         "Constraint",
                         "",
                         "Constraint to copy - may belong to a different object");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, 0);
  /* return type */
  parm = RNA_def_pointer(func, "new_constraint", "Constraint", "", "New constraint");
  RNA_def_function_return(func, parm);
}

static void rna_def_pose_channel(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "PoseBone", NULL);
  RNA_def_struct_sdna(srna, "bPoseChannel");
  RNA_def_struct_ui_text(srna, "Pose Bone", "Channel defining pose data for a bone in a Pose");
  RNA_def_struct_path_func(srna, "rna_PoseBone_path");
  RNA_def_struct_idprops_func(srna, "rna_PoseBone_idprops");
  RNA_def_struct_ui_icon(srna, ICON_BONE_DATA);

  /* Bone Constraints */
  prop = RNA_def_property(srna, "constraints", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "Constraint");
  RNA_def_property_override_flag(
      prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY | PROPOVERRIDE_LIBRARY_INSERTION);
  RNA_def_property_ui_text(prop, "Constraints", "Constraints that act on this pose channel");
  RNA_def_property_override_funcs(prop, NULL, NULL, "rna_PoseChannel_constraints_override_apply");

  rna_def_pose_channel_constraints(brna, prop);

  /* Name + Selection Status */
  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop, NULL, NULL, "rna_PoseChannel_name_set");
  RNA_def_property_ui_text(prop, "Name", "");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_update(prop, 0, "rna_PoseChannel_name_update");

  /* Baked Bone Path cache data */
  rna_def_motionpath_common(srna);

  /* Relationships to other bones */
  prop = RNA_def_property(srna, "bone", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "Bone");
  RNA_def_property_pointer_funcs(prop, "rna_PoseChannel_bone_get", NULL, NULL, NULL);
  RNA_def_property_flag(prop, PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Bone", "Bone associated with this PoseBone");

  prop = RNA_def_property(srna, "parent", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "PoseBone");
  RNA_def_property_flag(prop, PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Parent", "Parent of this pose bone");

  prop = RNA_def_property(srna, "child", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "PoseBone");
  RNA_def_property_flag(prop, PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Child", "Child of this pose bone");

  /* Transformation settings */
  prop = RNA_def_property(srna, "location", PROP_FLOAT, PROP_TRANSLATION);
  RNA_def_property_float_sdna(prop, NULL, "loc");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_editable_array_func(prop, "rna_PoseChannel_location_editable");
  RNA_def_property_ui_text(prop, "Location", "");
  RNA_def_property_ui_range(prop, -FLT_MAX, FLT_MAX, 1, RNA_TRANSLATION_PREC_DEFAULT);
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "scale", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, NULL, "size");
  RNA_def_property_flag(prop, PROP_PROPORTIONAL);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_editable_array_func(prop, "rna_PoseChannel_scale_editable");
  RNA_def_property_float_array_default(prop, rna_default_scale_3d);
  RNA_def_property_ui_text(prop, "Scale", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "rotation_quaternion", PROP_FLOAT, PROP_QUATERNION);
  RNA_def_property_float_sdna(prop, NULL, "quat");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_editable_array_func(prop, "rna_PoseChannel_rotation_4d_editable");
  RNA_def_property_float_array_default(prop, rna_default_quaternion);
  RNA_def_property_ui_text(prop, "Quaternion Rotation", "Rotation in Quaternions");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* XXX: for axis-angle, it would have been nice to have 2 separate fields for UI purposes, but
   * having a single one is better for Keyframing and other property-management situations...
   */
  prop = RNA_def_property(srna, "rotation_axis_angle", PROP_FLOAT, PROP_AXISANGLE);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_array(prop, 4);
  RNA_def_property_float_funcs(prop,
                               "rna_PoseChannel_rotation_axis_angle_get",
                               "rna_PoseChannel_rotation_axis_angle_set",
                               NULL);
  RNA_def_property_editable_array_func(prop, "rna_PoseChannel_rotation_4d_editable");
  RNA_def_property_float_array_default(prop, rna_default_axis_angle);
  RNA_def_property_ui_text(
      prop, "Axis-Angle Rotation", "Angle of Rotation for Axis-Angle rotation representation");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "rotation_euler", PROP_FLOAT, PROP_EULER);
  RNA_def_property_float_sdna(prop, NULL, "eul");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_editable_array_func(prop, "rna_PoseChannel_rotation_euler_editable");
  RNA_def_property_ui_text(prop, "Euler Rotation", "Rotation in Eulers");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "rotation_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "rotmode");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_enum_items(prop, rna_enum_object_rotation_mode_items);
  RNA_def_property_enum_funcs(prop, NULL, "rna_PoseChannel_rotation_mode_set", NULL);
  /* XXX... disabled, since proxy-locked layers are currently
   * used for ensuring proxy-syncing too */
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_ui_text(prop, "Rotation Mode", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* Curved bones settings - Applied on top of restpose values */
  rna_def_bone_curved_common(srna, true, false);

  /* Custom BBone next/prev sources */
  prop = RNA_def_property(srna, "bbone_custom_handle_start", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "bbone_prev");
  RNA_def_property_struct_type(prop, "PoseBone");
  RNA_def_property_flag(prop, PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_NO_COMPARISON);
  RNA_def_property_ui_text(
      prop, "B-Bone Start Handle", "Bone that serves as the start handle for the B-Bone curve");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_dependency_update");

  prop = RNA_def_property(srna, "bbone_custom_handle_end", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "bbone_next");
  RNA_def_property_struct_type(prop, "PoseBone");
  RNA_def_property_flag(prop, PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_NO_COMPARISON);
  RNA_def_property_ui_text(
      prop, "B-Bone End Handle", "Bone that serves as the end handle for the B-Bone curve");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_dependency_update");

  /* transform matrices - should be read-only since these are set directly by AnimSys evaluation */
  prop = RNA_def_property(srna, "matrix_channel", PROP_FLOAT, PROP_MATRIX);
  RNA_def_property_float_sdna(prop, NULL, "chan_mat");
  RNA_def_property_multi_array(prop, 2, rna_matrix_dimsize_4x4);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel Matrix", "4x4 matrix, before constraints");

  /* writable because it touches loc/scale/rot directly */
  prop = RNA_def_property(srna, "matrix_basis", PROP_FLOAT, PROP_MATRIX);
  RNA_def_property_multi_array(prop, 2, rna_matrix_dimsize_4x4);
  RNA_def_property_ui_text(
      prop,
      "Basis Matrix",
      "Alternative access to location/scale/rotation relative to the parent and own rest bone");
  RNA_def_property_float_funcs(
      prop, "rna_PoseChannel_matrix_basis_get", "rna_PoseChannel_matrix_basis_set", NULL);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* final matrix */
  prop = RNA_def_property(srna, "matrix", PROP_FLOAT, PROP_MATRIX);
  RNA_def_property_float_sdna(prop, NULL, "pose_mat");
  RNA_def_property_multi_array(prop, 2, rna_matrix_dimsize_4x4);
  RNA_def_property_float_funcs(prop, NULL, "rna_PoseChannel_matrix_set", NULL);
  RNA_def_property_ui_text(
      prop,
      "Pose Matrix",
      "Final 4x4 matrix after constraints and drivers are applied (object space)");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* Head/Tail Coordinates (in Pose Space) - Automatically calculated... */
  prop = RNA_def_property(srna, "head", PROP_FLOAT, PROP_TRANSLATION);
  RNA_def_property_float_sdna(prop, NULL, "pose_head");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Pose Head Position", "Location of head of the channel's bone");
  RNA_def_property_ui_range(prop, -FLT_MAX, FLT_MAX, 1, RNA_TRANSLATION_PREC_DEFAULT);

  prop = RNA_def_property(srna, "tail", PROP_FLOAT, PROP_TRANSLATION);
  RNA_def_property_float_sdna(prop, NULL, "pose_tail");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Pose Tail Position", "Location of tail of the channel's bone");
  RNA_def_property_ui_range(prop, -FLT_MAX, FLT_MAX, 1, RNA_TRANSLATION_PREC_DEFAULT);

  prop = RNA_def_property(srna, "length", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_funcs(prop, "rna_PoseChannel_length_get", NULL, NULL);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Length", "Length of the bone");

  /* IK Settings */
  prop = RNA_def_property(srna, "is_in_ik_chain", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_PoseChannel_has_ik_get", NULL);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Has IK", "Is part of an IK chain");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "lock_ik_x", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "ikflag", BONE_IK_NO_XDOF);
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, true);
  RNA_def_property_ui_text(prop, "IK X Lock", "Disallow movement around the X axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "lock_ik_y", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "ikflag", BONE_IK_NO_YDOF);
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, true);
  RNA_def_property_ui_text(prop, "IK Y Lock", "Disallow movement around the Y axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "lock_ik_z", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "ikflag", BONE_IK_NO_ZDOF);
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, true);
  RNA_def_property_ui_text(prop, "IK Z Lock", "Disallow movement around the Z axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "use_ik_limit_x", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "ikflag", BONE_IK_XLIMIT);
  RNA_def_property_ui_text(prop, "IK X Limit", "Limit movement around the X axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "use_ik_limit_y", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "ikflag", BONE_IK_YLIMIT);
  RNA_def_property_ui_text(prop, "IK Y Limit", "Limit movement around the Y axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "use_ik_limit_z", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "ikflag", BONE_IK_ZLIMIT);
  RNA_def_property_ui_text(prop, "IK Z Limit", "Limit movement around the Z axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "use_ik_rotation_control", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "ikflag", BONE_IK_ROTCTL);
  RNA_def_property_ui_text(prop, "IK Rotation Control", "Apply channel rotation as IK constraint");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "use_ik_linear_control", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "ikflag", BONE_IK_LINCTL);
  RNA_def_property_ui_text(
      prop, "IK Linear Control", "Apply channel size as IK constraint if stretching is enabled");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_min_x", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "limitmin[0]");
  RNA_def_property_range(prop, -M_PI, 0.0f);
  RNA_def_property_ui_text(prop, "IK X Minimum", "Minimum angles for IK Limit");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_max_x", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "limitmax[0]");
  RNA_def_property_range(prop, 0.0f, M_PI);
  RNA_def_property_ui_text(prop, "IK X Maximum", "Maximum angles for IK Limit");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_min_y", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "limitmin[1]");
  RNA_def_property_range(prop, -M_PI, 0.0f);
  RNA_def_property_ui_text(prop, "IK Y Minimum", "Minimum angles for IK Limit");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_max_y", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "limitmax[1]");
  RNA_def_property_range(prop, 0.0f, M_PI);
  RNA_def_property_ui_text(prop, "IK Y Maximum", "Maximum angles for IK Limit");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_min_z", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "limitmin[2]");
  RNA_def_property_range(prop, -M_PI, 0.0f);
  RNA_def_property_ui_text(prop, "IK Z Minimum", "Minimum angles for IK Limit");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_max_z", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "limitmax[2]");
  RNA_def_property_range(prop, 0.0f, M_PI);
  RNA_def_property_ui_text(prop, "IK Z Maximum", "Maximum angles for IK Limit");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_stiffness_x", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "stiffness[0]");
  RNA_def_property_range(prop, 0.0f, 0.99f);
  RNA_def_property_ui_text(prop, "IK X Stiffness", "IK stiffness around the X axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_stiffness_y", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "stiffness[1]");
  RNA_def_property_range(prop, 0.0f, 0.99f);
  RNA_def_property_ui_text(prop, "IK Y Stiffness", "IK stiffness around the Y axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_stiffness_z", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "stiffness[2]");
  RNA_def_property_range(prop, 0.0f, 0.99f);
  RNA_def_property_ui_text(prop, "IK Z Stiffness", "IK stiffness around the Z axis");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_stretch", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "ikstretch");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "IK Stretch", "Allow scaling of the bone for IK");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_IK_update");

  prop = RNA_def_property(srna, "ik_rotation_weight", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "ikrotweight");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "IK Rotation Weight", "Weight of rotation constraint for IK");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "ik_linear_weight", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "iklinweight");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "IK Lin Weight", "Weight of scale constraint for IK");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* custom bone shapes */
  prop = RNA_def_property(srna, "custom_shape", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "custom");
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_REFCOUNT);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(
      prop, "Custom Object", "Object that defines custom display shape for this bone");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_dependency_update");

  prop = RNA_def_property(srna, "custom_shape_scale_xyz", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, NULL, "custom_scale_xyz");
  RNA_def_property_flag(prop, PROP_PROPORTIONAL);
  RNA_def_property_float_array_default(prop, rna_default_scale_3d);
  RNA_def_property_ui_text(prop, "Custom Shape Scale", "Adjust the size of the custom shape");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "custom_shape_translation", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, NULL, "custom_translation");
  RNA_def_property_flag(prop, PROP_PROPORTIONAL);
  RNA_def_property_ui_text(
      prop, "Custom Shape Translation", "Adjust the location of the custom shape");
  RNA_def_property_ui_range(prop, -FLT_MAX, FLT_MAX, 1, RNA_TRANSLATION_PREC_DEFAULT);
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "custom_shape_rotation_euler", PROP_FLOAT, PROP_EULER);
  RNA_def_property_float_sdna(prop, NULL, "custom_rotation_euler");
  RNA_def_property_ui_text(
      prop, "Custom Shape Rotation", "Adjust the rotation of the custom shape");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "use_custom_shape_bone_size", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, NULL, "drawflag", PCHAN_DRAW_NO_CUSTOM_BONE_SIZE);
  RNA_def_property_ui_text(
      prop, "Scale to Bone Length", "Scale the custom object by the bone length");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "custom_shape_transform", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "custom_tx");
  RNA_def_property_struct_type(prop, "PoseBone");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_PTR_NO_OWNERSHIP);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_NO_COMPARISON);
  RNA_def_property_ui_text(prop,
                           "Custom Shape Transform",
                           "Bone that defines the display transform of this custom shape");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_pointer_funcs(
      prop, NULL, "rna_PoseChannel_custom_shape_transform_set", NULL, NULL);
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* bone groups */
  prop = RNA_def_property(srna, "bone_group_index", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "agrp_index");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_int_funcs(prop,
                             "rna_PoseChannel_bone_group_index_get",
                             "rna_PoseChannel_bone_group_index_set",
                             "rna_PoseChannel_bone_group_index_range");
  RNA_def_property_ui_text(
      prop, "Bone Group Index", "Bone group this pose channel belongs to (0 means no group)");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "bone_group", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "BoneGroup");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_pointer_funcs(
      prop, "rna_PoseChannel_bone_group_get", "rna_PoseChannel_bone_group_set", NULL, NULL);
  RNA_def_property_ui_text(prop, "Bone Group", "Bone group this pose channel belongs to");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* transform locks */
  prop = RNA_def_property(srna, "lock_location", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "protectflag", OB_LOCK_LOCX);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Lock Location", "Lock editing of location when transforming");
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, 1);
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "lock_rotation", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "protectflag", OB_LOCK_ROTX);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Lock Rotation", "Lock editing of rotation when transforming");
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, 1);
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* XXX this is sub-optimal - it really should be included above, but due to technical reasons
   *     we can't do this! */
  prop = RNA_def_property(srna, "lock_rotation_w", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "protectflag", OB_LOCK_ROTW);
  RNA_def_property_ui_text(
      prop,
      "Lock Rotation (4D Angle)",
      "Lock editing of 'angle' component of four-component rotations when transforming");
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, 1);
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  /* XXX this needs a better name */
  prop = RNA_def_property(srna, "lock_rotations_4d", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "protectflag", OB_LOCK_ROT4D);
  RNA_def_property_ui_text(
      prop,
      "Lock Rotations (4D)",
      "Lock editing of four component rotations by components (instead of as Eulers)");
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "lock_scale", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "protectflag", OB_LOCK_SCALEX);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Lock Scale", "Lock editing of scale when transforming");
  RNA_def_property_ui_icon(prop, ICON_UNLOCKED, 1);
  RNA_def_property_editable_func(prop, "rna_PoseChannel_proxy_editable");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  RNA_api_pose_channel(srna);
}

static void rna_def_pose_itasc(BlenderRNA *brna)
{
  static const EnumPropertyItem prop_itasc_mode_items[] = {
      {0,
       "ANIMATION",
       0,
       "Animation",
       "Stateless solver computing pose starting from current action and non-IK constraints"},
      {ITASC_SIMULATION,
       "SIMULATION",
       0,
       "Simulation",
       "State-full solver running in real-time context and ignoring actions "
       "and non-IK constraints"},
      {0, NULL, 0, NULL, NULL},
  };
  static const EnumPropertyItem prop_itasc_reiteration_items[] = {
      {0,
       "NEVER",
       0,
       "Never",
       "The solver does not reiterate, not even on first frame (starts from rest pose)"},
      {ITASC_INITIAL_REITERATION,
       "INITIAL",
       0,
       "Initial",
       "The solver reiterates (converges) on the first frame but not on "
       "subsequent frame"},
      {ITASC_INITIAL_REITERATION | ITASC_REITERATION,
       "ALWAYS",
       0,
       "Always",
       "The solver reiterates (converges) on all frames"},
      {0, NULL, 0, NULL, NULL},
  };

  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "Itasc", "IKParam");
  RNA_def_struct_sdna(srna, "bItasc");
  RNA_def_struct_ui_text(srna, "bItasc", "Parameters for the iTaSC IK solver");

  prop = RNA_def_property(srna, "precision", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "precision");
  RNA_def_property_range(prop, 0.0f, 0.1f);
  RNA_def_property_ui_text(prop, "Precision", "Precision of convergence in case of reiteration");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "iterations", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "numiter");
  RNA_def_property_range(prop, 0, 1000);
  RNA_def_property_ui_text(
      prop, "Iterations", "Maximum number of iterations for convergence in case of reiteration");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "step_count", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "numstep");
  RNA_def_property_range(prop, 1.0f, 50.0f);
  RNA_def_property_ui_text(prop, "Num Steps", "Divide the frame interval into this many steps");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, NULL, "flag");
  RNA_def_property_enum_items(prop, prop_itasc_mode_items);
  RNA_def_property_ui_text(prop, "Mode", NULL);
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update_rebuild");

  prop = RNA_def_property(srna, "reiteration_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, NULL, "flag");
  RNA_def_property_enum_items(prop, prop_itasc_reiteration_items);
  RNA_def_property_ui_text(prop,
                           "Reiteration",
                           "Defines if the solver is allowed to reiterate (converge until "
                           "precision is met) on none, first or all frames");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "use_auto_step", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", ITASC_AUTO_STEP);
  RNA_def_property_ui_text(prop,
                           "Auto Step",
                           "Automatically determine the optimal number of steps for best "
                           "performance/accuracy trade off");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "step_min", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "minstep");
  RNA_def_property_range(prop, 0.0f, 0.1f);
  RNA_def_property_ui_text(
      prop, "Min Step", "Lower bound for timestep in second in case of automatic substeps");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "step_max", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "maxstep");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(
      prop, "Max Step", "Higher bound for timestep in second in case of automatic substeps");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "feedback", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "feedback");
  RNA_def_property_range(prop, 0.0f, 100.0f);
  RNA_def_property_ui_text(
      prop,
      "Feedback",
      "Feedback coefficient for error correction, average response time is 1/feedback");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "velocity_max", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "maxvel");
  RNA_def_property_range(prop, 0.0f, 100.0f);
  RNA_def_property_ui_text(prop, "Max Velocity", "Maximum joint velocity in radians/second");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "solver", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "solver");
  RNA_def_property_enum_items(prop, prop_solver_items);
  RNA_def_property_ui_text(
      prop, "Solver", "Solving method selection: automatic damping or manual damping");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update_rebuild");

  prop = RNA_def_property(srna, "damping_max", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "dampmax");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop,
                           "Damp",
                           "Maximum damping coefficient when singular value is nearly 0 "
                           "(higher values produce results with more stability, less reactivity)");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");

  prop = RNA_def_property(srna, "damping_epsilon", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "dampeps");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop,
                           "Epsilon",
                           "Singular value under which damping is progressively applied "
                           "(higher values produce results with more stability, less reactivity)");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Itasc_update");
}

static void rna_def_pose_ikparam(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "IKParam", NULL);
  RNA_def_struct_sdna(srna, "bIKParam");
  RNA_def_struct_ui_text(srna, "IKParam", "Base type for IK solver parameters");
  RNA_def_struct_refine_func(srna, "rna_IKParam_refine");

  prop = RNA_def_property(srna, "ik_solver", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "iksolver");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_enum_items(prop, prop_iksolver_items);
  RNA_def_property_ui_text(prop, "IK Solver", "IK solver for which these parameters are defined");
}

/* pose.bone_groups */
static void rna_def_bone_groups(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  PropertyRNA *prop;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "BoneGroups");
  srna = RNA_def_struct(brna, "BoneGroups", NULL);
  RNA_def_struct_sdna(srna, "bPose");
  RNA_def_struct_ui_text(srna, "Bone Groups", "Collection of bone groups");

  func = RNA_def_function(srna, "new", "rna_bone_group_new");
  RNA_def_function_ui_description(func, "Add a new bone group to the object");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_USE_REPORTS); /* ID needed for refresh */
  RNA_def_string(func, "name", "Group", MAX_NAME, "", "Name of the new group");
  /* return type */
  parm = RNA_def_pointer(func, "group", "BoneGroup", "", "New bone group");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_bone_group_remove");
  RNA_def_function_ui_description(func, "Remove a bone group from this object");
  RNA_def_function_flag(func, FUNC_USE_REPORTS | FUNC_USE_SELF_ID); /* ID needed for refresh */
  /* bone group to remove */
  parm = RNA_def_pointer(func, "group", "BoneGroup", "", "Removed bone group");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, 0);

  prop = RNA_def_property(srna, "active", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "BoneGroup");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_pointer_funcs(
      prop, "rna_Pose_active_bone_group_get", "rna_Pose_active_bone_group_set", NULL, NULL);
  RNA_def_property_ui_text(prop, "Active Bone Group", "Active bone group for this pose");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");

  prop = RNA_def_property(srna, "active_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, NULL, "active_group");
  RNA_def_property_int_funcs(prop,
                             "rna_Pose_active_bone_group_index_get",
                             "rna_Pose_active_bone_group_index_set",
                             "rna_Pose_active_bone_group_index_range");
  RNA_def_property_ui_text(prop, "Active Bone Group Index", "Active index in bone groups array");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_update");
}

static void rna_def_pose(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* struct definition */
  srna = RNA_def_struct(brna, "Pose", NULL);
  RNA_def_struct_sdna(srna, "bPose");
  RNA_def_struct_ui_text(
      srna, "Pose", "A collection of pose channels, including settings for animating bones");

  /* pose channels */
  prop = RNA_def_property(srna, "bones", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "chanbase", NULL);
  RNA_def_property_struct_type(prop, "PoseBone");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Pose Bones", "Individual pose bones for the armature");
  /* can be removed, only for fast lookup */
  RNA_def_property_collection_funcs(
      prop, NULL, NULL, NULL, NULL, NULL, NULL, "rna_PoseBones_lookup_string", NULL);

  /* bone groups */
  prop = RNA_def_property(srna, "bone_groups", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "agroups", NULL);
  RNA_def_property_struct_type(prop, "BoneGroup");
  RNA_def_property_ui_text(prop, "Bone Groups", "Groups of the bones");
  rna_def_bone_groups(brna, prop);

  /* ik solvers */
  prop = RNA_def_property(srna, "ik_solver", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "iksolver");
  RNA_def_property_enum_funcs(prop, NULL, "rna_Pose_ik_solver_set", NULL);
  RNA_def_property_enum_items(prop, prop_iksolver_items);
  RNA_def_property_ui_text(prop, "IK Solver", "Selection of IK solver for IK chain");
  RNA_def_property_update(prop, NC_OBJECT | ND_POSE, "rna_Pose_ik_solver_update");

  prop = RNA_def_property(srna, "ik_param", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "IKParam");
  RNA_def_property_pointer_funcs(
      prop, "rna_Pose_ikparam_get", NULL, "rna_Pose_ikparam_typef", NULL);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "IK Param", "Parameters for IK solver");

  RNA_define_lib_overridable(true);

  /* pose edit options */
  prop = RNA_def_property(srna, "use_mirror_x", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", POSE_MIRROR_EDIT);
  RNA_def_property_ui_text(
      prop, "X-Axis Mirror", "Apply changes to matching bone on opposite side of X-Axis");
  RNA_def_struct_path_func(srna, "rna_Pose_path");
  RNA_def_property_update(prop, 0, "rna_Pose_update");
  RNA_def_property_flag(prop, PROP_LIB_EXCEPTION);

  prop = RNA_def_property(srna, "use_mirror_relative", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", POSE_MIRROR_RELATIVE);
  RNA_def_property_ui_text(
      prop,
      "Relative Mirror",
      "Apply relative transformations in X-mirror mode (not supported with Auto IK)");
  RNA_def_struct_path_func(srna, "rna_Pose_path");
  RNA_def_property_update(prop, 0, "rna_Pose_update");
  RNA_def_property_flag(prop, PROP_LIB_EXCEPTION);

  prop = RNA_def_property(srna, "use_auto_ik", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", POSE_AUTO_IK);
  RNA_def_property_ui_text(
      prop, "Auto IK", "Add temporary IK constraints while grabbing bones in Pose Mode");
  RNA_def_struct_path_func(srna, "rna_Pose_path");
  RNA_def_property_update(prop, 0, "rna_Pose_update");
  RNA_def_property_flag(prop, PROP_LIB_EXCEPTION);

  RNA_define_lib_overridable(false);

  /* animviz */
  rna_def_animviz_common(srna);

  RNA_api_pose(srna);
}

void RNA_def_pose(BlenderRNA *brna)
{
  rna_def_pose(brna);
  rna_def_pose_channel(brna);
  rna_def_pose_ikparam(brna);
  rna_def_pose_itasc(brna);
  rna_def_bone_group(brna);
}

#endif

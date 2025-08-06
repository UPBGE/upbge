/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edarmature
 * Operators and API's for creating bones.
 */

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_constraint_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_fcurve.hh"
#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"

#include "ANIM_action.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_armature.hh"
#include "ED_outliner.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "ANIM_armature.hh"
#include "ANIM_bone_collections.hh"

#include "DEG_depsgraph.hh"

#include "armature_intern.hh"

using blender::Vector;

/* *************** Adding stuff in editmode *************** */

EditBone *ED_armature_ebone_add(bArmature *arm, const char *name)
{
  EditBone *bone = MEM_callocN<EditBone>("eBone");

  STRNCPY_UTF8(bone->name, name);
  ED_armature_ebone_unique_name(arm->edbo, bone->name, nullptr);

  BLI_addtail(arm->edbo, bone);

  bone->flag |= BONE_TIPSEL;
  bone->drawtype = ARM_DRAW_TYPE_ARMATURE_DEFINED;
  bone->weight = 1.0f;
  bone->dist = 0.25f;
  bone->xwidth = 0.1f;
  bone->zwidth = 0.1f;
  bone->rad_head = 0.10f;
  bone->rad_tail = 0.05f;
  bone->segments = 1;

  /* Bendy-Bone parameters */
  bone->roll1 = 0.0f;
  bone->roll2 = 0.0f;
  bone->curve_in_x = 0.0f;
  bone->curve_in_z = 0.0f;
  bone->curve_out_x = 0.0f;
  bone->curve_out_z = 0.0f;
  bone->ease1 = 1.0f;
  bone->ease2 = 1.0f;

  /* Prevent custom bone colors from having alpha zero.
   * Part of the fix for issue #115434. */
  bone->color.custom.solid[3] = 255;
  bone->color.custom.select[3] = 255;
  bone->color.custom.active[3] = 255;

  copy_v3_fl(bone->scale_in, 1.0f);
  copy_v3_fl(bone->scale_out, 1.0f);

  return bone;
}

EditBone *ED_armature_ebone_add_primitive(Object *obedit_arm,
                                          const float length,
                                          const bool view_aligned)
{
  bArmature *arm = static_cast<bArmature *>(obedit_arm->data);
  EditBone *bone;

  ED_armature_edit_deselect_all(obedit_arm);

  /* Create a bone */
  bone = ED_armature_ebone_add(arm, DATA_("Bone"));

  arm->act_edbone = bone;

  zero_v3(bone->head);
  zero_v3(bone->tail);

  bone->tail[view_aligned ? 1 : 2] = length;

  if (arm->runtime.active_collection) {
    ANIM_armature_bonecoll_assign_editbone(arm->runtime.active_collection, bone);
  }

  return bone;
}

/**
 * Note this is already ported to multi-objects as it is.
 * Since only the active bone is extruded even for single objects,
 * it makes sense to stick to the active object here.
 *
 * If we want the support to be expanded we should something like the
 * offset we do for mesh click extrude.
 */
static wmOperatorStatus armature_click_extrude_exec(bContext *C, wmOperator * /*op*/)
{
  bArmature *arm;
  EditBone *ebone, *newbone, *flipbone;
  float mat[3][3], imat[3][3];
  int a, to_root = 0;
  Object *obedit;
  Scene *scene;

  scene = CTX_data_scene(C);
  obedit = CTX_data_edit_object(C);
  arm = static_cast<bArmature *>(obedit->data);

  /* find the active or selected bone */
  for (ebone = static_cast<EditBone *>(arm->edbo->first); ebone; ebone = ebone->next) {
    if (!blender::animrig::bone_is_visible_editbone(arm, ebone)) {
      continue;
    }
    if (ebone->flag & BONE_TIPSEL || arm->act_edbone == ebone) {
      break;
    }
  }

  if (ebone == nullptr) {
    for (ebone = static_cast<EditBone *>(arm->edbo->first); ebone; ebone = ebone->next) {
      if (!blender::animrig::bone_is_visible_editbone(arm, ebone)) {
        continue;
      }
      if (ebone->flag & BONE_ROOTSEL || arm->act_edbone == ebone) {
        break;
      }
    }
    if (ebone == nullptr) {
      return OPERATOR_CANCELLED;
    }

    to_root = 1;
  }

  ED_armature_edit_deselect_all(obedit);

  /* we re-use code for mirror editing... */
  flipbone = nullptr;
  if (arm->flag & ARM_MIRROR_EDIT) {
    flipbone = ED_armature_ebone_get_mirrored(arm->edbo, ebone);
  }

  for (a = 0; a < 2; a++) {
    if (a == 1) {
      if (flipbone == nullptr) {
        break;
      }
      std::swap(flipbone, ebone);
    }

    newbone = ED_armature_ebone_add(arm, ebone->name);
    arm->act_edbone = newbone;

    if (to_root) {
      copy_v3_v3(newbone->head, ebone->head);
      newbone->rad_head = ebone->rad_tail;
      newbone->parent = ebone->parent;
    }
    else {
      copy_v3_v3(newbone->head, ebone->tail);
      newbone->rad_head = ebone->rad_tail;
      newbone->parent = ebone;
      newbone->flag |= BONE_CONNECTED;
    }

    const View3DCursor *curs = &scene->cursor;
    copy_v3_v3(newbone->tail, curs->location);
    sub_v3_v3v3(newbone->tail, newbone->tail, obedit->object_to_world().location());

    if (a == 1) {
      newbone->tail[0] = -newbone->tail[0];
    }

    copy_m3_m4(mat, obedit->object_to_world().ptr());
    invert_m3_m3(imat, mat);
    mul_m3_v3(imat, newbone->tail);

    newbone->length = len_v3v3(newbone->head, newbone->tail);
    newbone->rad_tail = newbone->length * 0.05f;
    newbone->dist = newbone->length * 0.25f;
  }

  ED_armature_edit_sync_selection(arm->edbo);

  WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, obedit);
  DEG_id_tag_update(&obedit->id, ID_RECALC_SELECT);
  ED_outliner_select_sync_from_edit_bone_tag(C);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus armature_click_extrude_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  /* TODO: most of this code is copied from set3dcursor_invoke,
   * it would be better to reuse code in set3dcursor_invoke */

  /* temporarily change 3d cursor position */
  Scene *scene;
  ARegion *region;
  View3D *v3d;
  float tvec[3], oldcurs[3], mval_f[2];

  scene = CTX_data_scene(C);
  region = CTX_wm_region(C);
  v3d = CTX_wm_view3d(C);

  View3DCursor *cursor = &scene->cursor;

  copy_v3_v3(oldcurs, cursor->location);

  copy_v2fl_v2i(mval_f, event->mval);
  ED_view3d_win_to_3d(v3d, region, cursor->location, mval_f, tvec);
  copy_v3_v3(cursor->location, tvec);

  /* extrude to the where new cursor is and store the operation result */
  wmOperatorStatus retval = armature_click_extrude_exec(C, op);

  /* restore previous 3d cursor position */
  copy_v3_v3(cursor->location, oldcurs);

  /* Support dragging to move after extrude, see: #114282. */
  if (retval & OPERATOR_FINISHED) {
    retval |= OPERATOR_PASS_THROUGH;
  }
  return WM_operator_flag_only_pass_through_on_press(retval, event);
}

void ARMATURE_OT_click_extrude(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Extrude to Cursor";
  ot->idname = "ARMATURE_OT_click_extrude";
  ot->description = "Create a new bone going from the last selected joint to the mouse position";

  /* API callbacks. */
  ot->invoke = armature_click_extrude_invoke;
  ot->exec = armature_click_extrude_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  /* props */
}

EditBone *add_points_bone(Object *obedit, float head[3], float tail[3])
{
  EditBone *ebo;

  ebo = ED_armature_ebone_add(static_cast<bArmature *>(obedit->data), DATA_("Bone"));

  copy_v3_v3(ebo->head, head);
  copy_v3_v3(ebo->tail, tail);

  return ebo;
}

static void pre_edit_bone_duplicate(ListBase *editbones)
{
  /* clear temp */
  ED_armature_ebone_listbase_temp_clear(editbones);
}

/**
 * Helper function for #pose_edit_bone_duplicate,
 * return the destination pchan from the original.
 */
static bPoseChannel *pchan_duplicate_map(
    const bPose *pose,
    const blender::Map<blender::StringRefNull, blender::StringRefNull> &name_map,
    bPoseChannel *pchan_src)
{
  bPoseChannel *pchan_dst = nullptr;
  const char *name_src = pchan_src->name;
  const blender::StringRefNull name_dst = name_map.lookup_default(name_src, "");
  if (!name_dst.is_empty()) {
    pchan_dst = BKE_pose_channel_find_name(pose, name_dst.c_str());
  }

  if (pchan_dst == nullptr) {
    pchan_dst = pchan_src;
  }

  return pchan_dst;
}

static void pose_edit_bone_duplicate(ListBase *editbones, Object *ob)
{
  if (ob->pose == nullptr) {
    return;
  }

  BKE_pose_channels_hash_free(ob->pose);
  BKE_pose_channels_hash_ensure(ob->pose);

  blender::Map<blender::StringRefNull, blender::StringRefNull> name_map;

  LISTBASE_FOREACH (EditBone *, ebone_src, editbones) {
    EditBone *ebone_dst = ebone_src->temp.ebone;
    if (!ebone_dst) {
      ebone_dst = ED_armature_ebone_get_mirrored(editbones, ebone_src);
    }

    if (ebone_dst) {
      name_map.add_as(ebone_src->name, ebone_dst->name);
    }
  }

  LISTBASE_FOREACH (EditBone *, ebone_src, editbones) {
    EditBone *ebone_dst = ebone_src->temp.ebone;
    if (!ebone_dst) {
      continue;
    }

    bPoseChannel *pchan_src = BKE_pose_channel_find_name(ob->pose, ebone_src->name);
    if (!pchan_src) {
      continue;
    }

    bPoseChannel *pchan_dst = BKE_pose_channel_find_name(ob->pose, ebone_dst->name);
    if (!pchan_dst) {
      continue;
    }

    if (pchan_src->custom_tx) {
      pchan_dst->custom_tx = pchan_duplicate_map(ob->pose, name_map, pchan_src->custom_tx);
    }
    if (pchan_src->bbone_prev) {
      pchan_dst->bbone_prev = pchan_duplicate_map(ob->pose, name_map, pchan_src->bbone_prev);
    }
    if (pchan_src->bbone_next) {
      pchan_dst->bbone_next = pchan_duplicate_map(ob->pose, name_map, pchan_src->bbone_next);
    }
  }
}

static void update_duplicate_subtarget(EditBone *dup_bone,
                                       Object *ob,
                                       const bool lookup_mirror_subtarget)
{
  /* If an edit bone has been duplicated, lets update its constraints if the
   * subtarget they point to has also been duplicated.
   */
  bPoseChannel *pchan = BKE_pose_channel_ensure(ob->pose, dup_bone->name);

  if (!pchan) {
    return;
  }

  EditBone *oldtarget, *newtarget;
  ListBase *conlist = &pchan->constraints;
  char name_flipped[MAX_ID_NAME - 2];
  LISTBASE_FOREACH (bConstraint *, curcon, conlist) {
    /* does this constraint have a subtarget in
     * this armature?
     */
    ListBase targets = {nullptr, nullptr};

    if (!BKE_constraint_targets_get(curcon, &targets)) {
      continue;
    }
    LISTBASE_FOREACH (bConstraintTarget *, ct, &targets) {
      if (!ct->tar || !ct->subtarget[0]) {
        continue;
      }
      Object *target_ob = ct->tar;
      if (target_ob->type != OB_ARMATURE || !target_ob->data) {
        /* Can only mirror armature. */
        continue;
      }
      bArmature *target_armature = static_cast<bArmature *>(target_ob->data);
      /* Was the subtarget bone duplicated too? If
       * so, update the constraint to point at the
       * duplicate of the old subtarget.
       */

      /* TODO: support updating sub-targets for multi-object edit mode.
       * This requires all objects bones to be duplicated before this runs. */
      oldtarget = (ob == target_ob) ?
                      ED_armature_ebone_find_name(target_armature->edbo, ct->subtarget) :
                      nullptr;
      if (oldtarget && oldtarget->temp.ebone) {
        newtarget = oldtarget->temp.ebone;
        STRNCPY_UTF8(ct->subtarget, newtarget->name);
      }
      else if (lookup_mirror_subtarget) {
        BLI_string_flip_side_name(name_flipped, ct->subtarget, false, sizeof(name_flipped));
        if (bPoseChannel *flipped_bone = BKE_pose_channel_find_name(ct->tar->pose, name_flipped)) {
          STRNCPY_UTF8(ct->subtarget, flipped_bone->name);
        }
      }
    }
    BKE_constraint_targets_flush(curcon, &targets, false);
  }
}

static void update_duplicate_action_constraint_settings(
    EditBone *dup_bone, EditBone *orig_bone, Object *ob, bPoseChannel *pchan, bConstraint *curcon)
{
  bActionConstraint *act_con = static_cast<bActionConstraint *>(curcon->data);

  float mat[4][4];

  bConstraintOb cob{};
  cob.depsgraph = nullptr;
  cob.scene = nullptr;
  cob.ob = ob;
  cob.pchan = pchan;
  BKE_constraint_custom_object_space_init(&cob, curcon);

  unit_m4(mat);
  bPoseChannel *target_pchan = BKE_pose_channel_find_name(ob->pose, act_con->subtarget);
  BKE_constraint_mat_convertspace(
      ob, target_pchan, &cob, mat, curcon->tarspace, CONSTRAINT_SPACE_LOCAL, false);

  float max_axis_val = 0;
  int max_axis = 0;
  /* Which axis represents X now. IE, which axis defines the mirror plane. */
  for (int i = 0; i < 3; i++) {
    float cur_val = fabsf(mat[0][i]);
    if (cur_val > max_axis_val) {
      max_axis = i;
      max_axis_val = cur_val;
    }
  }

  /* data->type is mapped as follows for backwards compatibility:
   * 00,01,02 - rotation (it used to be like this)
   * 10,11,12 - scaling
   * 20,21,22 - location
   */
  /* Mirror the target range */
  if (act_con->type < 10 && act_con->type != max_axis) {
    /* Y or Z rotation */
    act_con->min = -act_con->min;
    act_con->max = -act_con->max;
  }
  else if (act_con->type == max_axis + 10) {
    /* X scaling */
  }
  else if (act_con->type == max_axis + 20) {
    /* X location */
    float imat[4][4];

    invert_m4_m4(imat, mat);

    float min_vec[3], max_vec[3];

    zero_v3(min_vec);
    zero_v3(max_vec);

    min_vec[0] = act_con->min;
    max_vec[0] = act_con->max;

    /* convert values into local object space */
    mul_m4_v3(mat, min_vec);
    mul_m4_v3(mat, max_vec);

    min_vec[0] *= -1;
    max_vec[0] *= -1;

    /* convert back to the settings space */
    mul_m4_v3(imat, min_vec);
    mul_m4_v3(imat, max_vec);

    act_con->min = min_vec[0];
    act_con->max = max_vec[0];
  }

  /* See if there is any channels that uses this bone */
  bAction *act = (bAction *)act_con->act;
  if (act) {
    blender::animrig::Action &action = act->wrap();
    blender::animrig::Channelbag *cbag = blender::animrig::channelbag_for_action_slot(
        action, act_con->action_slot_handle);

    /* Create a copy and mirror the animation */
    auto bone_name_filter = [&](const FCurve &fcurve) -> bool {
      return blender::animrig::fcurve_matches_collection_path(
          fcurve, "pose.bones[", orig_bone->name);
    };
    Vector<FCurve *> fcurves = blender::animrig::fcurves_in_action_slot_filtered(
        act, act_con->action_slot_handle, bone_name_filter);
    for (const FCurve *old_curve : fcurves) {
      FCurve *new_curve = BKE_fcurve_copy(old_curve);
      char *old_path = new_curve->rna_path;

      new_curve->rna_path = BLI_string_replaceN(old_path, orig_bone->name, dup_bone->name);
      MEM_freeN(old_path);

      /* FIXME: deal with the case where this F-Curve already exists. */

      /* Flip the animation */
      int i;
      BezTriple *bezt;
      for (i = 0, bezt = new_curve->bezt; i < new_curve->totvert; i++, bezt++) {
        const size_t slength = strlen(new_curve->rna_path);
        bool flip = false;
        if (BLI_strn_endswith(new_curve->rna_path, "location", slength) &&
            new_curve->array_index == 0)
        {
          flip = true;
        }
        else if (BLI_strn_endswith(new_curve->rna_path, "rotation_quaternion", slength) &&
                 ELEM(new_curve->array_index, 2, 3))
        {
          flip = true;
        }
        else if (BLI_strn_endswith(new_curve->rna_path, "rotation_euler", slength) &&
                 ELEM(new_curve->array_index, 1, 2))
        {
          flip = true;
        }
        else if (BLI_strn_endswith(new_curve->rna_path, "rotation_axis_angle", slength) &&
                 ELEM(new_curve->array_index, 2, 3))
        {
          flip = true;
        }

        if (flip) {
          bezt->vec[0][1] *= -1;
          bezt->vec[1][1] *= -1;
          bezt->vec[2][1] *= -1;
        }
      }

      if (action.is_action_legacy()) {
        /* Make sure that a action group name for the new bone exists */
        bActionGroup *agrp = BKE_action_group_find_name(act, dup_bone->name);
        if (agrp == nullptr) {
          agrp = action_groups_add_new(act, dup_bone->name);
        }
        BLI_assert(agrp != nullptr);
        action_groups_add_channel(act, agrp, new_curve);
        continue;
      }

      BLI_assert_msg(cbag, "If there are F-Curves for this slot, there should be a channelbag");
      bActionGroup &agrp = cbag->channel_group_ensure(dup_bone->name);
      cbag->fcurve_append(*new_curve);
      cbag->fcurve_assign_to_channel_group(*new_curve, agrp);
    }
  }

  /* Make depsgraph aware of our changes. */
  DEG_id_tag_update(&act->id, ID_RECALC_ANIMATION_NO_FLUSH);
}

static void update_duplicate_kinematics_constraint_settings(bConstraint *curcon)
{
  /* IK constraint */
  bKinematicConstraint *ik = static_cast<bKinematicConstraint *>(curcon->data);
  ik->poleangle = -M_PI - ik->poleangle;
  /* Wrap the angle to the +/-180.0f range (default soft limit of the input boxes). */
  ik->poleangle = angle_wrap_rad(ik->poleangle);
}

static void update_duplicate_loc_rot_constraint_settings(Object *ob,
                                                         bPoseChannel *pchan,
                                                         bConstraint *curcon)
{
  /* This code assumes that bRotLimitConstraint and bLocLimitConstraint have the same fields in
   * the same memory locations. */
  bRotLimitConstraint *limit = static_cast<bRotLimitConstraint *>(curcon->data);
  float local_mat[4][4], imat[4][4];

  float min_vec[3], max_vec[3];

  min_vec[0] = limit->xmin;
  min_vec[1] = limit->ymin;
  min_vec[2] = limit->zmin;

  max_vec[0] = limit->xmax;
  max_vec[1] = limit->ymax;
  max_vec[2] = limit->zmax;

  unit_m4(local_mat);

  bConstraintOb cob{};
  cob.depsgraph = nullptr;
  cob.scene = nullptr;
  cob.ob = ob;
  cob.pchan = pchan;
  BKE_constraint_custom_object_space_init(&cob, curcon);

  BKE_constraint_mat_convertspace(
      ob, pchan, &cob, local_mat, curcon->ownspace, CONSTRAINT_SPACE_LOCAL, false);

  if (curcon->type == CONSTRAINT_TYPE_ROTLIMIT) {
    /* Zero out any location translation */
    local_mat[3][0] = local_mat[3][1] = local_mat[3][2] = 0;
  }

  invert_m4_m4(imat, local_mat);
  /* convert values into local object space */
  mul_m4_v3(local_mat, min_vec);
  mul_m4_v3(local_mat, max_vec);

  if (curcon->type == CONSTRAINT_TYPE_ROTLIMIT) {
    float min_copy[3];

    copy_v3_v3(min_copy, min_vec);

    min_vec[1] = max_vec[1] * -1;
    min_vec[2] = max_vec[2] * -1;

    max_vec[1] = min_copy[1] * -1;
    max_vec[2] = min_copy[2] * -1;
  }
  else {
    float min_x_copy = min_vec[0];

    min_vec[0] = max_vec[0] * -1;
    max_vec[0] = min_x_copy * -1;

    /* Also flip the enabled axis check-boxes accordingly. */
    const bool use_max_x = (limit->flag & LIMIT_XMAX);
    const bool use_min_x = (limit->flag & LIMIT_XMIN);
    limit->flag |= use_max_x ? LIMIT_XMIN : 0;
    limit->flag &= (use_max_x && !use_min_x) ? ~LIMIT_XMAX : limit->flag;
    limit->flag |= use_min_x ? LIMIT_XMAX : 0;
    limit->flag &= (use_min_x && !use_max_x) ? ~LIMIT_XMIN : limit->flag;
  }

  /* convert back to the settings space */
  mul_m4_v3(imat, min_vec);
  mul_m4_v3(imat, max_vec);

  limit->xmin = min_vec[0];
  limit->ymin = min_vec[1];
  limit->zmin = min_vec[2];

  limit->xmax = max_vec[0];
  limit->ymax = max_vec[1];
  limit->zmax = max_vec[2];
}

static void update_duplicate_transform_constraint_settings(Object *ob,
                                                           bPoseChannel *pchan,
                                                           bConstraint *curcon)
{
  bTransformConstraint *trans = static_cast<bTransformConstraint *>(curcon->data);

  float target_mat[4][4], own_mat[4][4], imat[4][4];

  bConstraintOb cob{};
  cob.depsgraph = nullptr;
  cob.scene = nullptr;
  cob.ob = ob;
  cob.pchan = pchan;
  BKE_constraint_custom_object_space_init(&cob, curcon);

  unit_m4(own_mat);
  BKE_constraint_mat_convertspace(
      ob, pchan, &cob, own_mat, curcon->ownspace, CONSTRAINT_SPACE_LOCAL, false);

  /* ###Source map mirroring### */
  float old_min, old_max;

  /* Source location */
  invert_m4_m4(imat, own_mat);

  /* convert values into local object space */
  mul_m4_v3(own_mat, trans->from_min);
  mul_m4_v3(own_mat, trans->from_max);

  old_min = trans->from_min[0];
  old_max = trans->from_max[0];

  trans->from_min[0] = -old_max;
  trans->from_max[0] = -old_min;

  /* convert back to the settings space */
  mul_m4_v3(imat, trans->from_min);
  mul_m4_v3(imat, trans->from_max);

  /* Source rotation */

  /* Zero out any location translation */
  own_mat[3][0] = own_mat[3][1] = own_mat[3][2] = 0;

  invert_m4_m4(imat, own_mat);

  /* convert values into local object space */
  mul_m4_v3(own_mat, trans->from_min_rot);
  mul_m4_v3(own_mat, trans->from_max_rot);

  old_min = trans->from_min_rot[1];
  old_max = trans->from_max_rot[1];

  trans->from_min_rot[1] = old_max * -1;
  trans->from_max_rot[1] = old_min * -1;

  old_min = trans->from_min_rot[2];
  old_max = trans->from_max_rot[2];

  trans->from_min_rot[2] = old_max * -1;
  trans->from_max_rot[2] = old_min * -1;

  /* convert back to the settings space */
  mul_m4_v3(imat, trans->from_min_rot);
  mul_m4_v3(imat, trans->from_max_rot);

  /* Source scale does not require any mirroring */

  /* ###Destination map mirroring### */
  float temp_vec[3];
  float imat_rot[4][4];

  bPoseChannel *target_pchan = BKE_pose_channel_find_name(ob->pose, trans->subtarget);
  unit_m4(target_mat);
  BKE_constraint_mat_convertspace(
      ob, target_pchan, &cob, target_mat, curcon->tarspace, CONSTRAINT_SPACE_LOCAL, false);

  invert_m4_m4(imat, target_mat);
  /* convert values into local object space */
  mul_m4_v3(target_mat, trans->to_min);
  mul_m4_v3(target_mat, trans->to_max);
  mul_m4_v3(target_mat, trans->to_min_scale);
  mul_m4_v3(target_mat, trans->to_max_scale);

  /* Zero out any location translation */
  target_mat[3][0] = target_mat[3][1] = target_mat[3][2] = 0;
  invert_m4_m4(imat_rot, target_mat);

  mul_m4_v3(target_mat, trans->to_min_rot);
  mul_m4_v3(target_mat, trans->to_max_rot);

  /* TODO(sebpa): This does not support euler order, but doing so will make this way more complex.
   * For now we have decided to not support all corner cases and advanced setups. */

  /* Helper variables to denote the axis in trans->map */
  const char X = 0;
  const char Y = 1;
  const char Z = 2;

  switch (trans->to) {
    case TRANS_SCALE:
      copy_v3_v3(temp_vec, trans->to_max_scale);

      for (int i = 0; i < 3; i++) {
        if ((trans->from == TRANS_LOCATION && trans->map[i] == X) ||
            (trans->from == TRANS_ROTATION && trans->map[i] != X))
        {
          /* X Loc to X/Y/Z Scale: Min/Max Flipped */
          /* Y Rot to X/Y/Z Scale: Min/Max Flipped */
          /* Z Rot to X/Y/Z Scale: Min/Max Flipped */
          trans->to_max_scale[i] = trans->to_min_scale[i];
          trans->to_min_scale[i] = temp_vec[i];
        }
      }
      break;
    case TRANS_LOCATION:
      /* Invert the X location */
      trans->to_min[0] *= -1;
      trans->to_max[0] *= -1;

      copy_v3_v3(temp_vec, trans->to_max);

      for (int i = 0; i < 3; i++) {
        if ((trans->from == TRANS_LOCATION && trans->map[i] == X) ||
            (trans->from == TRANS_ROTATION && trans->map[i] != X))
        {
          /* X Loc to X/Y/Z Loc: Min/Max Flipped (and Inverted)
           * Y Rot to X/Y/Z Loc: Min/Max Flipped
           * Z Rot to X/Y/Z Loc: Min/Max Flipped */
          trans->to_max[i] = trans->to_min[i];
          trans->to_min[i] = temp_vec[i];
        }
      }
      break;
    case TRANS_ROTATION:
      /* Invert the Z rotation */
      trans->to_min_rot[2] *= -1;
      trans->to_max_rot[2] *= -1;

      if ((trans->from == TRANS_LOCATION && trans->map[1] != X) ||
          (trans->from == TRANS_ROTATION && trans->map[1] != Y) || trans->from == TRANS_SCALE)
      {
        /* Invert the Y rotation */
        trans->to_min_rot[1] *= -1;
        trans->to_max_rot[1] *= -1;
      }

      copy_v3_v3(temp_vec, trans->to_max_rot);

      for (int i = 0; i < 3; i++) {
        if ((trans->from == TRANS_LOCATION && trans->map[i] == X && i != 1) ||
            (trans->from == TRANS_ROTATION && trans->map[i] == Y && i != 1) ||
            (trans->from == TRANS_ROTATION && trans->map[i] == Z))
        {
          /* X Loc to X/Z Rot: Flipped
           * Y Rot to X/Z Rot: Flipped
           * Z Rot to X/Y/Z rot: Flipped */
          trans->to_max_rot[i] = trans->to_min_rot[i];
          trans->to_min_rot[i] = temp_vec[i];
        }
      }

      if (trans->from == TRANS_ROTATION && trans->map[1] == Y) {
        /* Y Rot to Y Rot: Flip and invert */
        trans->to_max_rot[1] = -trans->to_min_rot[1];
        trans->to_min_rot[1] = -temp_vec[1];
      }

      break;
  }
  /* convert back to the settings space */
  mul_m4_v3(imat, trans->to_min);
  mul_m4_v3(imat, trans->to_max);
  mul_m4_v3(imat_rot, trans->to_min_rot);
  mul_m4_v3(imat_rot, trans->to_max_rot);
  mul_m4_v3(imat, trans->to_min_scale);
  mul_m4_v3(imat, trans->to_max_scale);
}

static void track_axis_x_swap(int &value)
{
  /* Swap track axis X <> -X. */
  if (value == TRACK_X) {
    value = TRACK_nX;
  }
  else if (value == TRACK_nX) {
    value = TRACK_X;
  }
}

static void track_axis_x_swap(char &value)
{
  /* Swap track axis X <> -X. */
  if (value == TRACK_X) {
    value = TRACK_nX;
  }
  else if (value == TRACK_nX) {
    value = TRACK_X;
  }
}

static void update_duplicate_constraint_track_to_settings(bConstraint *curcon)
{
  bTrackToConstraint *data = static_cast<bTrackToConstraint *>(curcon->data);
  track_axis_x_swap(data->reserved1);
}

static void update_duplicate_constraint_lock_track_settings(bConstraint *curcon)
{
  bLockTrackConstraint *data = static_cast<bLockTrackConstraint *>(curcon->data);
  track_axis_x_swap(data->trackflag);
}

static void update_duplicate_constraint_damp_track_settings(bConstraint *curcon)
{
  bDampTrackConstraint *data = static_cast<bDampTrackConstraint *>(curcon->data);
  track_axis_x_swap(data->trackflag);
}

static void update_duplicate_constraint_shrinkwrap_settings(bConstraint *curcon)
{
  bShrinkwrapConstraint *data = static_cast<bShrinkwrapConstraint *>(curcon->data);
  track_axis_x_swap(data->trackAxis);
}

static void update_duplicate_constraint_settings(EditBone *dup_bone,
                                                 EditBone *orig_bone,
                                                 Object *ob)
{
  /* If an edit bone has been duplicated, lets update its constraints if the
   * subtarget they point to has also been duplicated.
   */
  bPoseChannel *pchan;
  ListBase *conlist;

  if ((pchan = BKE_pose_channel_ensure(ob->pose, dup_bone->name)) == nullptr ||
      (conlist = &pchan->constraints) == nullptr)
  {
    return;
  }

  LISTBASE_FOREACH (bConstraint *, curcon, conlist) {
    switch (curcon->type) {
      case CONSTRAINT_TYPE_ACTION:
        update_duplicate_action_constraint_settings(dup_bone, orig_bone, ob, pchan, curcon);
        break;
      case CONSTRAINT_TYPE_KINEMATIC:
        update_duplicate_kinematics_constraint_settings(curcon);
        break;
      case CONSTRAINT_TYPE_LOCLIMIT:
      case CONSTRAINT_TYPE_ROTLIMIT:
        update_duplicate_loc_rot_constraint_settings(ob, pchan, curcon);
        break;
      case CONSTRAINT_TYPE_TRANSFORM:
        update_duplicate_transform_constraint_settings(ob, pchan, curcon);
        break;
      case CONSTRAINT_TYPE_TRACKTO:
        update_duplicate_constraint_track_to_settings(curcon);
        break;
      case CONSTRAINT_TYPE_LOCKTRACK:
        update_duplicate_constraint_lock_track_settings(curcon);
        break;
      case CONSTRAINT_TYPE_DAMPTRACK:
        update_duplicate_constraint_damp_track_settings(curcon);
        break;
      case CONSTRAINT_TYPE_SHRINKWRAP:
        update_duplicate_constraint_shrinkwrap_settings(curcon);
        break;
    }
  }
}

static void update_duplicate_custom_bone_shapes(bContext *C, EditBone *dup_bone, Object *ob)
{
  if (ob->pose == nullptr) {
    return;
  }
  bPoseChannel *pchan;
  pchan = BKE_pose_channel_ensure(ob->pose, dup_bone->name);

  if (pchan->custom != nullptr) {
    Main *bmain = CTX_data_main(C);
    char name_flip[MAX_ID_NAME - 2];

    /* Invert the X location */
    pchan->custom_translation[0] *= -1;
    /* Invert the Y rotation */
    pchan->custom_rotation_euler[1] *= -1;
    /* Invert the Z rotation */
    pchan->custom_rotation_euler[2] *= -1;

    /* Skip the first two chars in the object name as those are used to store object type */
    BLI_string_flip_side_name(name_flip, pchan->custom->id.name + 2, false, sizeof(name_flip));
    Object *shape_ob = reinterpret_cast<Object *>(BKE_libblock_find_name(bmain, ID_OB, name_flip));

    /* If name_flip doesn't exist, BKE_libblock_find_name() returns pchan->custom (best match) */
    shape_ob = shape_ob == pchan->custom ? nullptr : shape_ob;

    if (shape_ob != nullptr) {
      /* A flipped shape object exists, use it! */
      pchan->custom = shape_ob;
    }
    else {
      /* Flip shape */
      pchan->custom_scale_xyz[0] *= -1;
    }
  }
}

/* Properties should be added on a case by case basis whenever needed to avoid mirroring things
 * that shouldn't be mirrored. */
static void mirror_pose_bone(Object &ob, EditBone &ebone)
{
  bPoseChannel *pose_bone = BKE_pose_channel_find_name(ob.pose, ebone.name);
  BLI_assert(pose_bone);
  float limit_min = pose_bone->limitmin[2];
  pose_bone->limitmin[2] = -pose_bone->limitmax[2];
  pose_bone->limitmax[2] = -limit_min;
}

static void mirror_bone_collection_assignments(bArmature &armature,
                                               EditBone &source_bone,
                                               EditBone &target_bone)
{
  BLI_assert_msg(armature.edbo != nullptr, "Expecting the armature to be in edit mode");
  char name_flip[64];
  /* Avoiding modification of the ListBase in the iteration. */
  blender::Vector<BoneCollection *> unassign_collections;
  blender::Vector<BoneCollection *> assign_collections;

  /* Find all collections from source_bone that can be flipped. */
  LISTBASE_FOREACH (BoneCollectionReference *, collection_reference, &source_bone.bone_collections)
  {
    BoneCollection *collection = collection_reference->bcoll;
    BLI_string_flip_side_name(name_flip, collection->name, false, sizeof(name_flip));
    if (STREQ(name_flip, collection->name)) {
      /* Name flipping failed. */
      continue;
    }
    BoneCollection *flipped_collection = ANIM_armature_bonecoll_get_by_name(&armature, name_flip);
    if (!flipped_collection) {
      const int bcoll_index = blender::animrig::armature_bonecoll_find_index(&armature,
                                                                             collection);
      const int parent_index = blender::animrig::armature_bonecoll_find_parent_index(&armature,
                                                                                     bcoll_index);
      flipped_collection = ANIM_armature_bonecoll_new(&armature, name_flip, parent_index);
    }
    BLI_assert(flipped_collection != nullptr);
    unassign_collections.append(collection);
    assign_collections.append(flipped_collection);
  }

  /* The target_bone might not be in unassign_collections anymore, or might already be in
   * assign_collections. The assign functions will just do nothing in those cases. */
  for (BoneCollection *collection : unassign_collections) {
    ANIM_armature_bonecoll_unassign_editbone(collection, &target_bone);
  }

  for (BoneCollection *collection : assign_collections) {
    ANIM_armature_bonecoll_assign_editbone(collection, &target_bone);
  }
}

static void copy_pchan(EditBone *src_bone, EditBone *dst_bone, Object *src_ob, Object *dst_ob)
{
  /* copy the ID property */
  if (src_bone->prop) {
    dst_bone->prop = IDP_CopyProperty(src_bone->prop);
  }
  if (src_bone->system_properties) {
    dst_bone->system_properties = IDP_CopyProperty(src_bone->system_properties);
  }

  /* Lets duplicate the list of constraints that the
   * current bone has.
   */
  if (src_ob->pose) {
    bPoseChannel *chanold, *channew;

    chanold = BKE_pose_channel_ensure(src_ob->pose, src_bone->name);
    if (chanold) {
      /* WARNING: this creates a new pose-channel, but there will not be an attached bone
       * yet as the new bones created here are still 'EditBones' not 'Bones'.
       */
      channew = BKE_pose_channel_ensure(dst_ob->pose, dst_bone->name);

      if (channew) {
        BKE_pose_channel_copy_data(channew, chanold);
      }
    }
  }
}

void ED_armature_ebone_copy(EditBone *dest, const EditBone *source)
{
  memcpy(dest, source, sizeof(*dest));
  BLI_duplicatelist(&dest->bone_collections, &dest->bone_collections);
}

EditBone *duplicateEditBoneObjects(
    EditBone *cur_bone, const char *name, ListBase *editbones, Object *src_ob, Object *dst_ob)
{
  EditBone *e_bone = MEM_mallocN<EditBone>("addup_editbone");

  /* Copy data from old bone to new bone */
  ED_armature_ebone_copy(e_bone, cur_bone);

  cur_bone->temp.ebone = e_bone;
  e_bone->temp.ebone = cur_bone;

  if (name != nullptr) {
    STRNCPY_UTF8(e_bone->name, name);
  }

  ED_armature_ebone_unique_name(editbones, e_bone->name, nullptr);
  BLI_addtail(editbones, e_bone);

  copy_pchan(cur_bone, e_bone, src_ob, dst_ob);

  return e_bone;
}

EditBone *duplicateEditBone(EditBone *cur_bone, const char *name, ListBase *editbones, Object *ob)
{
  return duplicateEditBoneObjects(cur_bone, name, editbones, ob, ob);
}

static wmOperatorStatus armature_duplicate_selected_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bool do_flip_names = RNA_boolean_get(op->ptr, "do_flip_names");

  /* cancel if nothing selected */
  if (CTX_DATA_COUNT(C, selected_bones) == 0) {
    return OPERATOR_CANCELLED;
  }

  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *ob : objects) {
    EditBone *ebone_iter;
    /* The beginning of the duplicated bones in the edbo list */
    EditBone *ebone_first_dupe = nullptr;

    bArmature *arm = static_cast<bArmature *>(ob->data);

    ED_armature_edit_sync_selection(arm->edbo); /* XXX why is this needed? */

    pre_edit_bone_duplicate(arm->edbo);

    /* Select mirrored bones */
    if (arm->flag & ARM_MIRROR_EDIT) {
      LISTBASE_FOREACH (EditBone *, ebone_iter, arm->edbo) {
        if (blender::animrig::bone_is_selected(arm, ebone_iter)) {
          EditBone *ebone;

          ebone = ED_armature_ebone_get_mirrored(arm->edbo, ebone_iter);
          if (ebone) {
            ebone->flag |= BONE_SELECTED;
          }
        }
      }
    }

    /* Find the selected bones and duplicate them as needed */
    for (ebone_iter = static_cast<EditBone *>(arm->edbo->first);
         ebone_iter && ebone_iter != ebone_first_dupe;
         ebone_iter = ebone_iter->next)
    {
      if (blender::animrig::bone_is_selected(arm, ebone_iter)) {
        EditBone *ebone;
        char new_bone_name_buff[MAXBONENAME];
        const char *new_bone_name = ebone_iter->name;

        if (do_flip_names) {
          BLI_string_flip_side_name(
              new_bone_name_buff, ebone_iter->name, false, sizeof(new_bone_name_buff));

          /* Only use flipped name if not yet in use. Otherwise we'd get again inconsistent
           * namings (different numbers), better keep default behavior in this case. */
          if (ED_armature_ebone_find_name(arm->edbo, new_bone_name_buff) == nullptr) {
            new_bone_name = new_bone_name_buff;
          }
        }

        ebone = duplicateEditBone(ebone_iter, new_bone_name, arm->edbo, ob);

        if (!ebone_first_dupe) {
          ebone_first_dupe = ebone;
        }
      }
    }

    /* Run though the list and fix the pointers */
    for (ebone_iter = static_cast<EditBone *>(arm->edbo->first);
         ebone_iter && ebone_iter != ebone_first_dupe;
         ebone_iter = ebone_iter->next)
    {
      if (blender::animrig::bone_is_selected(arm, ebone_iter)) {
        EditBone *ebone = ebone_iter->temp.ebone;

        if (!ebone_iter->parent) {
          /* If this bone has no parent,
           * Set the duplicate->parent to nullptr
           */
          ebone->parent = nullptr;
        }
        else if (ebone_iter->parent->temp.ebone) {
          /* If this bone has a parent that was duplicated,
           * Set the duplicate->parent to the cur_bone->parent->temp
           */
          ebone->parent = ebone_iter->parent->temp.ebone;
        }
        else {
          /* If this bone has a parent that IS not selected,
           * Set the duplicate->parent to the cur_bone->parent
           */
          ebone->parent = ebone_iter->parent;
          ebone->flag &= ~BONE_CONNECTED;
        }

        /* Update custom handle links. */
        if (ebone_iter->bbone_prev && ebone_iter->bbone_prev->temp.ebone) {
          ebone->bbone_prev = ebone_iter->bbone_prev->temp.ebone;
        }
        if (ebone_iter->bbone_next && ebone_iter->bbone_next->temp.ebone) {
          ebone->bbone_next = ebone_iter->bbone_next->temp.ebone;
        }

        /* Lets try to fix any constraint sub-targets that might have been duplicated. */
        update_duplicate_subtarget(ebone, ob, false);
      }
    }

    /* correct the active bone */
    if (arm->act_edbone && arm->act_edbone->temp.ebone) {
      arm->act_edbone = arm->act_edbone->temp.ebone;
    }

    /* Deselect the old bones and select the new ones */
    for (ebone_iter = static_cast<EditBone *>(arm->edbo->first);
         ebone_iter && ebone_iter != ebone_first_dupe;
         ebone_iter = ebone_iter->next)
    {
      if (blender::animrig::bone_is_visible_editbone(arm, ebone_iter)) {
        ebone_iter->flag &= ~(BONE_SELECTED | BONE_TIPSEL | BONE_ROOTSEL);
      }
    }

    pose_edit_bone_duplicate(arm->edbo, ob);

    WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, ob);
    DEG_id_tag_update(&ob->id, ID_RECALC_SELECT);
  }

  ED_outliner_select_sync_from_edit_bone_tag(C);

  return OPERATOR_FINISHED;
}

void ARMATURE_OT_duplicate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Duplicate Selected Bone(s)";
  ot->idname = "ARMATURE_OT_duplicate";
  ot->description = "Make copies of the selected bones within the same armature";

  /* API callbacks. */
  ot->exec = armature_duplicate_selected_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna,
      "do_flip_names",
      false,
      "Flip Names",
      "Try to flip names of the bones, if possible, instead of adding a number extension");
}

/* Get the duplicated or existing mirrored copy of the bone. */
static EditBone *get_symmetrized_bone(bArmature *arm, EditBone *bone)
{
  if (bone == nullptr) {
    return nullptr;
  }
  if (bone->temp.ebone != nullptr) {
    return bone->temp.ebone;
  }

  EditBone *mirror = ED_armature_ebone_get_mirrored(arm->edbo, bone);
  return (mirror != nullptr) ? mirror : bone;
}

/**
 * near duplicate of #armature_duplicate_selected_exec,
 * except for parenting part (keep in sync)
 */
static wmOperatorStatus armature_symmetrize_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const int direction = RNA_enum_get(op->ptr, "direction");
  const int axis = 0;

  /* cancel if nothing selected */
  if (CTX_DATA_COUNT(C, selected_bones) == 0) {
    return OPERATOR_CANCELLED;
  }

  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));
  for (Object *obedit : objects) {
    EditBone *ebone_iter;
    /* The beginning of the duplicated mirrored bones in the edbo list */
    EditBone *ebone_first_dupe = nullptr;

    bArmature *arm = static_cast<bArmature *>(obedit->data);

    ED_armature_edit_sync_selection(arm->edbo); /* XXX why is this needed? */

    pre_edit_bone_duplicate(arm->edbo);

    /* Deselect ebones depending on input axis and direction.
     * A symmetrizable selection contains selected ebones of the input direction
     * and unique selected bones with an unique flippable name.
     *
     * Storing temp pointers to mirrored unselected ebones. */
    LISTBASE_FOREACH (EditBone *, ebone_iter, arm->edbo) {
      if (!(blender::animrig::bone_is_visible_editbone(arm, ebone_iter) &&
            (ebone_iter->flag & BONE_SELECTED)))
      {
        /* Skipping invisible selected bones. */
        continue;
      }

      char name_flip[MAXBONENAME];
      if (ebone_iter == nullptr) {
        continue;
      }

      BLI_string_flip_side_name(name_flip, ebone_iter->name, false, sizeof(name_flip));

      if (STREQ(name_flip, ebone_iter->name)) {
        /* Skipping ebones without flippable as they don't have the potential to be mirrored. */
        ebone_iter->flag &= ~(BONE_SELECTED | BONE_TIPSEL | BONE_ROOTSEL);
        continue;
      }

      EditBone *ebone = ED_armature_ebone_find_name(arm->edbo, name_flip);

      if (!ebone) {
        /* The ebone_iter is unique and mirror-able. */
        continue;
      }

      if (ebone->flag & BONE_SELECTED) {
        /* The mirrored ebone and the ebone_iter are selected.
         * Deselect based on the input direction and axis. */
        float axis_delta;

        axis_delta = ebone->head[axis] - ebone_iter->head[axis];
        if (axis_delta == 0.0f) {
          /* The ebone heads are overlapping. */
          axis_delta = ebone->tail[axis] - ebone_iter->tail[axis];

          if (axis_delta == 0.0f) {
            /* Both mirrored bones point to each other and overlap exactly.
             * In this case there's no well defined solution, so de-select both and skip. */
            ebone->flag &= ~(BONE_SELECTED | BONE_TIPSEL | BONE_ROOTSEL);
            ebone_iter->flag &= ~(BONE_SELECTED | BONE_TIPSEL | BONE_ROOTSEL);
            continue;
          }
        }

        /* Deselect depending on direction. */
        if (((axis_delta < 0.0f) ? -1 : 1) == direction) {
          /* Don't store temp ptr if the iter_bone gets deselected.
           * In this case, the ebone.temp should point to the ebone_iter. */
          ebone_iter->flag &= ~(BONE_SELECTED | BONE_TIPSEL | BONE_ROOTSEL);
          continue;
        }

        ebone->flag &= ~(BONE_SELECTED | BONE_TIPSEL | BONE_ROOTSEL);
      }

      /* Set temp pointer to mirrored ebones */
      ebone_iter->temp.ebone = ebone;
    }

    /* Find the selected bones and duplicate them as needed, with mirrored name. */
    for (ebone_iter = static_cast<EditBone *>(arm->edbo->first);
         ebone_iter && ebone_iter != ebone_first_dupe;
         ebone_iter = ebone_iter->next)
    {
      if (blender::animrig::bone_is_selected(arm, ebone_iter)) {
        if (ebone_iter->temp.ebone != nullptr) {
          /* This will be set if the mirror bone already exists (no need to make a new one)
           * but we do need to make sure that the 'pchan' settings (constraints etc)
           * is synchronized. */
          bPoseChannel *pchan;
          /* Make sure we clean up the old data before overwriting it */
          pchan = BKE_pose_channel_ensure(obedit->pose, ebone_iter->temp.ebone->name);
          BKE_pose_channel_free(pchan);
          /* Sync pchan data */
          copy_pchan(ebone_iter, ebone_iter->temp.ebone, obedit, obedit);
          /* Sync scale mode */
          ebone_iter->temp.ebone->inherit_scale_mode = ebone_iter->inherit_scale_mode;
          continue;
        }

        char name_flip[MAXBONENAME];

        BLI_string_flip_side_name(name_flip, ebone_iter->name, false, sizeof(name_flip));

        /* mirrored bones must have a side-suffix */
        if (!STREQ(name_flip, ebone_iter->name)) {
          EditBone *ebone;

          ebone = duplicateEditBone(ebone_iter, name_flip, arm->edbo, obedit);

          if (!ebone_first_dupe) {
            ebone_first_dupe = ebone;
          }
        }
      }
    }

    /* Run through the list and fix the pointers. */
    for (ebone_iter = static_cast<EditBone *>(arm->edbo->first);
         ebone_iter && ebone_iter != ebone_first_dupe;
         ebone_iter = ebone_iter->next)
    {
      if (ebone_iter->temp.ebone) {
        /* copy all flags except for ... */
        const int flag_copy = (~0) & ~(BONE_SELECTED | BONE_ROOTSEL | BONE_TIPSEL);

        EditBone *ebone = ebone_iter->temp.ebone;

        /* Copy flags in case bone is pre-existing data. */
        ebone->flag = (ebone->flag & ~flag_copy) | (ebone_iter->flag & flag_copy);

        if (ebone_iter->parent == nullptr) {
          /* If this bone has no parent,
           * Set the duplicate->parent to nullptr
           */
          ebone->parent = nullptr;
          ebone->flag &= ~BONE_CONNECTED;
        }
        else {
          /* the parent may have been duplicated, if not lookup the mirror parent */
          EditBone *ebone_parent = get_symmetrized_bone(arm, ebone_iter->parent);

          if (ebone_parent == ebone_iter->parent) {
            /* If the mirror lookup failed, (but the current bone has a parent)
             * then we can assume the parent has no L/R but is a center bone.
             * So just use the same parent for both.
             */

            if (ebone->head[axis] != 0.0f) {
              /* The mirrored bone doesn't start on the mirror axis, so assume that this one
               * should not be connected to the old parent */
              ebone->flag &= ~BONE_CONNECTED;
            }
          }

          ebone->parent = ebone_parent;
        }

        /* Update custom handle links. */
        ebone->bbone_prev = get_symmetrized_bone(arm, ebone_iter->bbone_prev);
        ebone->bbone_next = get_symmetrized_bone(arm, ebone_iter->bbone_next);

        /* Sync bbone handle types */
        ebone->bbone_prev_type = ebone_iter->bbone_prev_type;
        ebone->bbone_next_type = ebone_iter->bbone_next_type;

        ebone->bbone_mapping_mode = ebone_iter->bbone_mapping_mode;
        ebone->bbone_flag = ebone_iter->bbone_flag;
        ebone->bbone_prev_flag = ebone_iter->bbone_prev_flag;
        ebone->bbone_next_flag = ebone_iter->bbone_next_flag;

        /* Lets try to fix any constraint sub-targets that might have been duplicated. */
        update_duplicate_subtarget(ebone, obedit, true);
        /* Try to update constraint options so that they are mirrored as well
         * (need to supply bone_iter as well in case we are working with existing bones) */
        update_duplicate_constraint_settings(ebone, ebone_iter, obedit);
        /* Mirror bone shapes if possible */
        update_duplicate_custom_bone_shapes(C, ebone, obedit);
        /* Mirror any settings on the pose bone. */
        mirror_pose_bone(*obedit, *ebone);
        mirror_bone_collection_assignments(*arm, *ebone_iter, *ebone);
      }
    }

    ED_armature_edit_transform_mirror_update(obedit);

    /* Selected bones now have their 'temp' pointer set,
     * so we don't need this anymore */

    /* Deselect the old bones and select the new ones */
    for (ebone_iter = static_cast<EditBone *>(arm->edbo->first);
         ebone_iter && ebone_iter != ebone_first_dupe;
         ebone_iter = ebone_iter->next)
    {
      if (blender::animrig::bone_is_visible_editbone(arm, ebone_iter)) {
        ebone_iter->flag &= ~(BONE_SELECTED | BONE_TIPSEL | BONE_ROOTSEL);
      }
    }

    /* New bones will be selected, but some of the bones may already exist */
    for (ebone_iter = static_cast<EditBone *>(arm->edbo->first);
         ebone_iter && ebone_iter != ebone_first_dupe;
         ebone_iter = ebone_iter->next)
    {
      EditBone *ebone = ebone_iter->temp.ebone;
      if (ebone && EBONE_SELECTABLE(arm, ebone)) {
        ED_armature_ebone_select_set(ebone, true);
      }
    }

    /* correct the active bone */
    if (arm->act_edbone && arm->act_edbone->temp.ebone) {
      arm->act_edbone = arm->act_edbone->temp.ebone;
    }

    pose_edit_bone_duplicate(arm->edbo, obedit);

    WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, obedit);
    DEG_id_tag_update(&obedit->id, ID_RECALC_SELECT);
  }

  return OPERATOR_FINISHED;
}

void ARMATURE_OT_symmetrize(wmOperatorType *ot)
{
  /* NOTE: following conventions from #MESH_OT_symmetrize */

  /* subset of 'rna_enum_symmetrize_direction_items' */
  static const EnumPropertyItem arm_symmetrize_direction_items[] = {
      {-1, "NEGATIVE_X", 0, "-X to +X", ""},
      {+1, "POSITIVE_X", 0, "+X to -X", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Symmetrize";
  ot->idname = "ARMATURE_OT_symmetrize";
  ot->description = "Enforce symmetry, make copies of the selection or use existing";

  /* API callbacks. */
  ot->exec = armature_symmetrize_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "direction",
                          arm_symmetrize_direction_items,
                          -1,
                          "Direction",
                          "Which sides to copy from and to (when both are selected)");
}

/* ------------------------------------------ */

/* previously extrude_armature */
/* context; editmode armature */
/* if forked && mirror-edit: makes two bones with flipped names */
static wmOperatorStatus armature_extrude_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bool forked = RNA_boolean_get(op->ptr, "forked");
  bool changed_multi = false;
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  enum ExtrudePoint {
    SKIP_EXTRUDE,
    TIP_EXTRUDE,
    ROOT_EXTRUDE,
  };

  for (Object *ob : objects) {
    bArmature *arm = static_cast<bArmature *>(ob->data);
    bool forked_iter = forked;

    EditBone *newbone = nullptr, *ebone, *flipbone, *first = nullptr;
    int a, totbone = 0;
    ExtrudePoint do_extrude;

    /* since we allow root extrude too, we have to make sure selection is OK */
    LISTBASE_FOREACH (EditBone *, ebone, arm->edbo) {
      if (blender::animrig::bone_is_visible_editbone(arm, ebone)) {
        if (ebone->flag & BONE_ROOTSEL) {
          if (ebone->parent && (ebone->flag & BONE_CONNECTED)) {
            if (ebone->parent->flag & BONE_TIPSEL) {
              ebone->flag &= ~BONE_ROOTSEL;
            }
          }
        }
      }
    }

    /* Duplicate the necessary bones */
    for (ebone = static_cast<EditBone *>(arm->edbo->first); ((ebone) && (ebone != first));
         ebone = ebone->next)
    {
      if (!blender::animrig::bone_is_visible_editbone(arm, ebone)) {
        continue;
      }
      /* We extrude per definition the tip. */
      do_extrude = SKIP_EXTRUDE;
      if (ebone->flag & (BONE_TIPSEL | BONE_SELECTED)) {
        do_extrude = TIP_EXTRUDE;
      }
      else if (ebone->flag & BONE_ROOTSEL) {
        /* but, a bone with parent deselected we do the root... */
        if (ebone->parent && (ebone->parent->flag & BONE_TIPSEL)) {
          /* pass */
        }
        else {
          do_extrude = ROOT_EXTRUDE;
        }
      }

      if (do_extrude) {
        /* we re-use code for mirror editing... */
        flipbone = nullptr;
        if (arm->flag & ARM_MIRROR_EDIT) {
          flipbone = ED_armature_ebone_get_mirrored(arm->edbo, ebone);
          if (flipbone) {
            forked_iter = false; /* we extrude 2 different bones */
            if (flipbone->flag & (BONE_TIPSEL | BONE_ROOTSEL | BONE_SELECTED)) {
              /* don't want this bone to be selected... */
              flipbone->flag &= ~(BONE_TIPSEL | BONE_SELECTED | BONE_ROOTSEL);
            }
          }
          if ((flipbone == nullptr) && (forked_iter)) {
            flipbone = ebone;
          }
        }

        for (a = 0; a < 2; a++) {
          if (a == 1) {
            if (flipbone == nullptr) {
              break;
            }
            std::swap(flipbone, ebone);
          }

          totbone++;
          newbone = MEM_callocN<EditBone>("extrudebone");

          if (do_extrude == TIP_EXTRUDE) {
            copy_v3_v3(newbone->head, ebone->tail);
            copy_v3_v3(newbone->tail, newbone->head);
            newbone->parent = ebone;

            /* copies it, in case mirrored bone */
            newbone->flag = ebone->flag & (BONE_TIPSEL | BONE_RELATIVE_PARENTING);

            if (newbone->parent) {
              newbone->flag |= BONE_CONNECTED;
            }
          }
          else if (do_extrude == ROOT_EXTRUDE) {
            copy_v3_v3(newbone->head, ebone->head);
            copy_v3_v3(newbone->tail, ebone->head);
            newbone->parent = ebone->parent;

            newbone->flag = BONE_TIPSEL;

            if (newbone->parent && (ebone->flag & BONE_CONNECTED)) {
              newbone->flag |= BONE_CONNECTED;
            }
          }

          newbone->color = ebone->color;
          newbone->drawtype = ebone->drawtype;

          newbone->weight = ebone->weight;
          newbone->dist = ebone->dist;
          newbone->xwidth = ebone->xwidth;
          newbone->zwidth = ebone->zwidth;
          newbone->rad_head = ebone->rad_tail; /* don't copy entire bone. */
          newbone->rad_tail = ebone->rad_tail;
          newbone->segments = 1;
          newbone->layer = ebone->layer;

          /* Bendy-Bone parameters */
          newbone->roll1 = ebone->roll1;
          newbone->roll2 = ebone->roll2;
          newbone->curve_in_x = ebone->curve_in_x;
          newbone->curve_in_z = ebone->curve_in_z;
          newbone->curve_out_x = ebone->curve_out_x;
          newbone->curve_out_z = ebone->curve_out_z;
          newbone->ease1 = ebone->ease1;
          newbone->ease2 = ebone->ease2;

          copy_v3_v3(newbone->scale_in, ebone->scale_in);
          copy_v3_v3(newbone->scale_out, ebone->scale_out);

          STRNCPY_UTF8(newbone->name, ebone->name);

          if (flipbone && forked_iter) { /* only set if mirror edit */
            if (strlen(newbone->name) < (MAXBONENAME - 2)) {
              BLI_strncat(newbone->name, (a == 0) ? "_L" : "_R", sizeof(newbone->name));
            }
          }
          ED_armature_ebone_unique_name(arm->edbo, newbone->name, nullptr);

          /* Copy bone collection membership. */
          BLI_duplicatelist(&newbone->bone_collections, &ebone->bone_collections);

          /* Add the new bone to the list */
          BLI_addtail(arm->edbo, newbone);
          if (!first) {
            first = newbone;
          }

          /* restore ebone if we were flipping */
          if (a == 1 && flipbone) {
            std::swap(flipbone, ebone);
          }
        }
      }

      /* Deselect the old bone */
      ebone->flag &= ~(BONE_TIPSEL | BONE_SELECTED | BONE_ROOTSEL);
    }
    /* if only one bone, make this one active */
    if (totbone == 1 && first) {
      arm->act_edbone = first;
    }
    else {
      arm->act_edbone = newbone;
    }

    if (totbone == 0) {
      continue;
    }

    changed_multi = true;

    /* Transform the endpoints */
    ED_armature_edit_sync_selection(arm->edbo);

    WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, ob);
    DEG_id_tag_update(&ob->id, ID_RECALC_SELECT);
  }

  if (!changed_multi) {
    return OPERATOR_CANCELLED;
  }

  ED_outliner_select_sync_from_edit_bone_tag(C);

  return OPERATOR_FINISHED;
}

void ARMATURE_OT_extrude(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Extrude";
  ot->idname = "ARMATURE_OT_extrude";
  ot->description = "Create new bones from the selected joints";

  /* API callbacks. */
  ot->exec = armature_extrude_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  RNA_def_boolean(ot->srna, "forked", false, "Forked", "");
}

/* ********************** Bone Add *************************************/

/* Op makes a new bone and returns it with its tip selected. */

static wmOperatorStatus armature_bone_primitive_add_exec(bContext *C, wmOperator *op)
{
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  Object *obedit = CTX_data_edit_object(C);
  EditBone *bone;
  float obmat[3][3], curs[3], viewmat[3][3], totmat[3][3], imat[3][3];
  char name[MAXBONENAME];

  RNA_string_get(op->ptr, "name", name);

  copy_v3_v3(curs, CTX_data_scene(C)->cursor.location);

  /* Get inverse point for head and orientation for tail */
  invert_m4_m4(obedit->runtime->world_to_object.ptr(), obedit->object_to_world().ptr());
  mul_m4_v3(obedit->world_to_object().ptr(), curs);

  if (rv3d && (U.flag & USER_ADD_VIEWALIGNED)) {
    copy_m3_m4(obmat, rv3d->viewmat);
  }
  else {
    unit_m3(obmat);
  }

  copy_m3_m4(viewmat, obedit->object_to_world().ptr());
  mul_m3_m3m3(totmat, obmat, viewmat);
  invert_m3_m3(imat, totmat);

  ED_armature_edit_deselect_all(obedit);

  /* Create a bone. */
  bone = ED_armature_ebone_add(static_cast<bArmature *>(obedit->data), name);
  ANIM_armature_bonecoll_assign_active(static_cast<bArmature *>(obedit->data), bone);

  bArmature *arm = static_cast<bArmature *>(obedit->data);
  if (!ANIM_bonecoll_is_visible_editbone(arm, bone)) {
    const BoneCollectionReference *bcoll_ref = static_cast<const BoneCollectionReference *>(
        bone->bone_collections.first);
    BLI_assert_msg(bcoll_ref,
                   "Bone that is not visible due to its bone collections MUST be assigned to at "
                   "least one of them.");
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Bone was added to a hidden collection '%s'",
                bcoll_ref->bcoll->name);
  }

  copy_v3_v3(bone->head, curs);

  if (rv3d && (U.flag & USER_ADD_VIEWALIGNED)) {
    add_v3_v3v3(bone->tail, bone->head, imat[1]); /* bone with unit length 1 */
  }
  else {
    add_v3_v3v3(bone->tail, bone->head, imat[2]); /* bone with unit length 1, pointing up Z */
  }

  /* NOTE: notifier might evolve. */
  WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, obedit);
  DEG_id_tag_update(&obedit->id, ID_RECALC_SELECT);
  ED_outliner_select_sync_from_edit_bone_tag(C);

  return OPERATOR_FINISHED;
}

void ARMATURE_OT_bone_primitive_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Bone";
  ot->idname = "ARMATURE_OT_bone_primitive_add";
  ot->description = "Add a new bone located at the 3D cursor";

  /* API callbacks. */
  ot->exec = armature_bone_primitive_add_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna, "name", nullptr, MAXBONENAME, "Name", "Name of the newly created bone");
}

/* ********************** Subdivide *******************************/

/* Subdivide Operators:
 * This group of operators all use the same 'exec' callback, but they are called
 * through several different operators - a combined menu (which just calls the exec in the
 * appropriate ways), and two separate ones.
 */

static wmOperatorStatus armature_subdivide_exec(bContext *C, wmOperator *op)
{
  Object *obedit = CTX_data_edit_object(C);
  EditBone *newbone;
  int cuts, i;

  /* there may not be a number_cuts property defined (for 'simple' subdivide) */
  cuts = RNA_int_get(op->ptr, "number_cuts");

  /* loop over all editable bones */
  CTX_DATA_BEGIN_WITH_ID (C, EditBone *, ebone, selected_editable_bones, bArmature *, arm) {
    /* Keep track of the last bone in the editbone list. The newly created ones
     * will be appended after this one. */
    EditBone *last_bone_before_cutting = static_cast<EditBone *>(arm->edbo->last);
    BLI_assert_msg(last_bone_before_cutting,
                   "If there is no bone before subdividing, which bone is being subdivided here?");

    for (i = cuts + 1; i > 1; i--) {
      /* compute cut ratio first */
      float cutratio = 1.0f / float(i);
      float cutratioI = 1.0f - cutratio;

      float val1[3];
      float val2[3];
      float val3[3];

      newbone = MEM_mallocN<EditBone>("ebone subdiv");
      *newbone = *ebone;
      BLI_addtail(arm->edbo, newbone);

      /* calculate location of newbone->head */
      copy_v3_v3(val1, ebone->head);
      copy_v3_v3(val2, ebone->tail);
      copy_v3_v3(val3, newbone->head);

      val3[0] = val1[0] * cutratio + val2[0] * cutratioI;
      val3[1] = val1[1] * cutratio + val2[1] * cutratioI;
      val3[2] = val1[2] * cutratio + val2[2] * cutratioI;

      copy_v3_v3(newbone->head, val3);
      copy_v3_v3(newbone->tail, ebone->tail);
      copy_v3_v3(ebone->tail, newbone->head);

      newbone->rad_head = ((ebone->rad_head * cutratio) + (ebone->rad_tail * cutratioI));
      ebone->rad_tail = newbone->rad_head;

      newbone->flag |= BONE_CONNECTED;
      newbone->prop = nullptr;
      newbone->system_properties = nullptr;

      /* correct parent bones */
      LISTBASE_FOREACH (EditBone *, tbone, arm->edbo) {
        if (tbone->parent == ebone) {
          tbone->parent = newbone;
        }
      }
      newbone->parent = ebone;

      /* Copy bone collection membership. */
      BLI_duplicatelist(&newbone->bone_collections, &ebone->bone_collections);
    }

    /* Ensure the bones are uniquely named, in the right order to ensure "Bone" is subdivided into
     * "Bone", "Bone.001", "Bone.002", etc. This has to be in the opposite order as the cuts in the
     * code above.
     *
     * The code above cuts into fractions (for cuts=3 it cuts into 1/4, then 1/3, then 1/2), which
     * means that it MUST be run in that order. Since the loop below also must run in the order it
     * is now in, and that's the opposite order of the loop above, they cannot be combined.
     *
     * If the code above were refactored, it could just calculate the final bone length and create
     * (N-1) bones of that length, which can then be done in any order. Then the code below can be
     * integrated into the code above.
     */
    ListBase new_bones;
    new_bones.first = last_bone_before_cutting->next;
    new_bones.last = static_cast<EditBone *>(arm->edbo->last);
    LISTBASE_FOREACH_BACKWARD (EditBone *, newbone, &new_bones) {
      ED_armature_ebone_unique_name(arm->edbo, newbone->name, newbone);
    }
  }
  CTX_DATA_END;

  /* NOTE: notifier might evolve. */
  WM_event_add_notifier(C, NC_OBJECT | ND_BONE_SELECT, obedit);
  DEG_id_tag_update(&obedit->id, ID_RECALC_SELECT);
  ED_outliner_select_sync_from_edit_bone_tag(C);

  return OPERATOR_FINISHED;
}

void ARMATURE_OT_subdivide(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Subdivide";
  ot->idname = "ARMATURE_OT_subdivide";
  ot->description = "Break selected bones into chains of smaller bones";

  /* API callbacks. */
  ot->exec = armature_subdivide_exec;
  ot->poll = ED_operator_editarmature;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Properties */
  prop = RNA_def_int(ot->srna, "number_cuts", 1, 1, 1000, "Number of Cuts", "", 1, 10);
  /* Avoid re-using last var because it can cause
   * _very_ high poly meshes and annoy users (or worse crash) */
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

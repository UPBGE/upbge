/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Converter/BL_ArmatureObject.cpp
 *  \ingroup bgeconv
 */

#include "BL_ArmatureObject.h"

#include <tbb/parallel_for.h>

#include "ANIM_action.hh"
#include "BKE_armature.hh"
#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_scene.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_object_types.hh"
#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_string.h"
#include "DEG_depsgraph_query.hh"
#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "../gpu/intern/gpu_shader_create_info.hh"
#include "DNA_meshdata_types.h"
#include "DNA_mesh_types.h"
#include "RNA_access.hh"

#include "BL_Action.h"
#include "BL_SceneConverter.h"
#include "KX_Globals.h"

/**
 * Move here pose function for game engine so that we can mix with GE objects
 * Principle is as follow:
 * Use Blender structures so that BKE_pose_where_is can be used unchanged
 * Copy the constraint so that they can be enabled/disabled/added/removed at runtime
 * Don't copy the constraints for the pose used by the Action actuator, it does not need them.
 * Scan the constraint structures so that the KX equivalent of target objects are identified and
 * stored in separate list.
 * When it is about to evaluate the pose, set the KX object position in the object_to_world of the
 * corresponding Blender objects and restore after the evaluation.
 */
// static void game_copy_pose(bPose **dst, bPose *src, int copy_constraint)
//{
//  /* The game engine copies the current armature pose and then swaps
//   * the object pose pointer. this makes it possible to change poses
//   * without affecting the original blender data. */

//  if (!src) {
//    *dst = nullptr;
//    return;
//  }
//  else if (*dst == src) {
//    CM_Warning("game_copy_pose source and target are the same");
//    *dst = nullptr;
//    return;
//  }

//  bPose *out = (bPose *)MEM_dupallocN(src);
//  out->chanhash = nullptr;
//  out->agroups.first = out->agroups.last = nullptr;
//  out->ikdata = nullptr;
//  out->ikparam = MEM_dupallocN(src->ikparam);
//  // out->flag |= POSE_GAME_ENGINE;
//  BLI_duplicatelist(&out->chanbase, &src->chanbase);

//  /* remap pointers */
//  GHash *ghash = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "game_copy_pose gh");

//  bPoseChannel *pchan = (bPoseChannel *)src->chanbase.first;
//  bPoseChannel *outpchan = (bPoseChannel *)out->chanbase.first;
//  for (; pchan; pchan = pchan->next, outpchan = outpchan->next) {
//    BLI_ghash_insert(ghash, pchan, outpchan);
//  }

//  for (pchan = (bPoseChannel *)out->chanbase.first; pchan; pchan = pchan->next) {
//    pchan->parent = (bPoseChannel *)BLI_ghash_lookup(ghash, pchan->parent);
//    pchan->child = (bPoseChannel *)BLI_ghash_lookup(ghash, pchan->child);

//    if (copy_constraint) {
//      ListBase listb;
//      // copy all constraint for backward compatibility
//      // BKE_constraints_copy nullptrs listb, no need to make extern for this operation.
//      BKE_constraints_copy(&listb, &pchan->constraints, false);
//      pchan->constraints = listb;
//    }
//    else {
//      BLI_listbase_clear(&pchan->constraints);
//    }

//    if (pchan->custom) {
//      id_us_plus(&pchan->custom->id);
//    }

//    // fails to link, props are not used in the BGE yet.
//#if 0
//		if (pchan->prop) {
//			pchan->prop = IDP_CopyProperty(pchan->prop);
//		}
//#endif
//    pchan->prop = nullptr;
//  }

//  BLI_ghash_free(ghash, nullptr, nullptr);
//  // set acceleration structure for channel lookup
//  BKE_pose_channels_hash_ensure(out);
//  *dst = out;
//}

void RemoveArmatureModifiers(Object *obj)
{
  if (obj) {
    ModifierData *md = (ModifierData *)obj->modifiers.first;
    while (md) {
      ModifierData *next = md->next;
      if (md->type == eModifierType_Armature) {
        BLI_remlink(&obj->modifiers, md);
        BKE_modifier_free(md);
      }
      md = next;
    }
  }
}

bPose *BGE_pose_copy_and_capture_rest(const bPose *src,
                                      bool copy_constraints,
                                      Object *runtime_obj,
                                      Object *deformed_obj,
                                      blender::Array<blender::float3> &restPositions,
                                      int &restPositionsNum)
{
  if (!src || !runtime_obj || !deformed_obj) {
    return nullptr;
  }

  bPose *saved_pose = nullptr;
  BKE_pose_copy_data(&saved_pose, runtime_obj->pose, /*copy_constraints=*/true);

  // 2. Appliquer la rest pose sur l'objet runtime
  BKE_pose_rest(runtime_obj->pose, false);

  // 3. Forcer la mise à jour du dependency graph
  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  DEG_id_tag_update(&runtime_obj->id, ID_RECALC_GEOMETRY | ID_RECALC_TRANSFORM);
  BKE_scene_graph_update_tagged(depsgraph, CTX_data_main(C));

  // 4. Récupérer le mesh évalué dans la rest pose
  Object *deformed_eval = DEG_get_evaluated(depsgraph, deformed_obj);
  Mesh *rest_mesh = static_cast<Mesh *>(deformed_eval->data);
  blender::Span<blender::float3> rest_positions_span = rest_mesh->vert_positions();

  // 5. Copier les positions dans le buffer de référence
  restPositionsNum = rest_positions_span.size();
  restPositions = blender::Array<blender::float3>(restPositionsNum);
  for (int i = 0; i < restPositionsNum; ++i) {
    restPositions[i] = rest_positions_span[i];
  }

  // 6. Restaurer la pose d'origine
  BKE_pose_free(runtime_obj->pose);
  runtime_obj->pose = saved_pose;

  // 7. Créer la copie "clean" de la pose (optionnel : copy_constraints)
  bPose *out = (bPose *)MEM_callocN(sizeof(bPose), "BGE_pose_copy_clean");
  out->flag = src->flag;
  out->ctime = src->ctime;
  BLI_duplicatelist(&out->chanbase, &src->chanbase);

  // Remap parent/child pointers
  GHash *ghash = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "BGE_pose_copy_clean");
  bPoseChannel *src_pchan = (bPoseChannel *)src->chanbase.first;
  bPoseChannel *out_pchan = (bPoseChannel *)out->chanbase.first;
  for (; src_pchan; src_pchan = src_pchan->next, out_pchan = out_pchan->next) {
    BLI_ghash_insert(ghash, src_pchan, out_pchan);
  }
  for (out_pchan = (bPoseChannel *)out->chanbase.first; out_pchan; out_pchan = out_pchan->next) {
    out_pchan->parent = (bPoseChannel *)BLI_ghash_lookup(ghash, out_pchan->parent);
    out_pchan->child = (bPoseChannel *)BLI_ghash_lookup(ghash, out_pchan->child);

    // Contraintes
    if (copy_constraints) {
      ListBase listb;
      BKE_constraints_copy(&listb, &out_pchan->constraints, false);
      out_pchan->constraints = listb;
    }
    else {
      BLI_listbase_clear(&out_pchan->constraints);
    }

    // Nettoyage des propriétés custom
    out_pchan->prop = nullptr;
    out_pchan->system_properties = nullptr;

    // Gestion du custom object (pour le skinning custom)
    if (out_pchan->custom) {
      id_us_plus(&out_pchan->custom->id);
    }
  }
  BLI_ghash_free(ghash, nullptr, nullptr);

  // Nettoyage global
  out->chanhash = nullptr;
  out->agroups.first = out->agroups.last = nullptr;
  out->ikdata = nullptr;
  out->ikparam = nullptr;

  BKE_pose_channels_hash_ensure(out);

  return out;
}

// Only allowed for Poses with identical channels.
static void game_blend_poses(bPose *dst, bPose *src, float srcweight, short mode)
{
  float dstweight;

  if (mode == BL_Action::ACT_BLEND_BLEND) {
    dstweight = 1.0f - srcweight;
  }
  else if (mode == BL_Action::ACT_BLEND_ADD) {
    dstweight = 1.0f;
  }
  else {
    dstweight = 1.0f;
  }

  bPoseChannel *schan = (bPoseChannel *)src->chanbase.first;
  for (bPoseChannel *dchan = (bPoseChannel *)dst->chanbase.first; dchan;
       dchan = (bPoseChannel *)dchan->next, schan = (bPoseChannel *)schan->next) {
    // always blend on all channels since we don't know which one has been set
    /* quat interpolation done separate */
    if (schan->rotmode == ROT_MODE_QUAT) {
      float dquat[4], squat[4];

      copy_qt_qt(dquat, dchan->quat);
      copy_qt_qt(squat, schan->quat);
      // Normalize quaternions so that interpolation/multiplication result is correct.
      normalize_qt(dquat);
      normalize_qt(squat);

      if (mode == BL_Action::ACT_BLEND_BLEND) {
        interp_qt_qtqt(dchan->quat, dquat, squat, srcweight);
      }
      else {
        pow_qt_fl_normalized(squat, srcweight);
        mul_qt_qtqt(dchan->quat, dquat, squat);
      }

      normalize_qt(dchan->quat);
    }

    for (unsigned short i = 0; i < 3; i++) {
      /* blending for loc and scale are pretty self-explanatory... */
      dchan->loc[i] = (dchan->loc[i] * dstweight) + (schan->loc[i] * srcweight);
      dchan->scale[i] = 1.0f + ((dchan->scale[i] - 1.0f) * dstweight) +
                       ((schan->scale[i] - 1.0f) * srcweight);

      /* euler-rotation interpolation done here instead... */
      // FIXME: are these results decent?
      if (schan->rotmode) {
        dchan->eul[i] = (dchan->eul[i] * dstweight) + (schan->eul[i] * srcweight);
      }
    }
    for (bConstraint *dcon = (bConstraint *)dchan->constraints.first,
                     *scon = (bConstraint *)schan->constraints.first;
         dcon && scon;
         dcon = dcon->next, scon = scon->next) {
      /* no 'add' option for constraint blending */
      dcon->enforce = dcon->enforce * (1.0f - srcweight) + scon->enforce * srcweight;
    }
  }

  /* this pose is now in src time */
  dst->ctime = src->ctime;
}

BL_ArmatureObject::BL_ArmatureObject()
    : KX_GameObject(), m_lastframe(0.0), m_drawDebug(false), m_lastapplyframe(0.0)
{
  m_controlledConstraints = new EXP_ListValue<BL_ArmatureConstraint>();
  m_deformedObj = nullptr;
  m_sbModifier = nullptr;
  m_sbCoords = nullptr;
  m_shader = nullptr;
  ssbo_in_pos = nullptr;
  ssbo_in_idx = nullptr;
  ssbo_in_wgt = nullptr;
  ssbo_out = nullptr;
  ssbo_bone_mat = nullptr;
}

BL_ArmatureObject::~BL_ArmatureObject()
{
  m_poseChannels->Release();
  m_controlledConstraints->Release();

  // if (m_objArma) {
  //	BKE_id_free(bmain, m_objArma->data);
  //	/* avoid BKE_libblock_free(bmain, m_objArma)
  //	   try to access m_objArma->data */
  //	m_objArma->data = nullptr;
  //	BKE_id_free(bmain, m_objArma);
  //}
  bContext *C = KX_GetActiveEngine()->GetContext();
  if (m_sbModifier && m_deformedObj) {
    if (m_sbCoords) {
      MEM_freeN(m_sbCoords);
      m_sbModifier->vertcoos = nullptr;
    }
    BLI_remlink(&m_deformedObj->modifiers, m_sbModifier);
    BKE_modifier_free((ModifierData *)m_sbModifier);
    m_sbModifier = nullptr;
  }
  BKE_id_delete(CTX_data_main(C), m_runtime_obj);

  if (m_shader) {
    GPU_shader_free(m_shader);
    m_shader = nullptr;
  }
  if (ssbo_in_pos) {
    // 11. Nettoyage (optionnel, selon la gestion mémoire de ton moteur)
    GPU_storagebuf_free(ssbo_in_pos);
    GPU_storagebuf_free(ssbo_in_idx);
    GPU_storagebuf_free(ssbo_in_wgt);
    GPU_storagebuf_free(ssbo_bone_mat);
    GPU_storagebuf_free(ssbo_out);
    ssbo_in_pos = nullptr;
    ssbo_in_idx = nullptr;
    ssbo_in_wgt = nullptr;
    ssbo_out = nullptr;
    ssbo_bone_mat = nullptr;
  }
}

void BL_ArmatureObject::SetBlenderObject(Object *obj)
{
  // XXX: I copied below from the destructor verbatim. But why we shouldn't free it?
  //
  // if (m_objArma) {
  //	BKE_id_free(bmain, m_objArma->data);
  //	/* avoid BKE_libblock_free(bmain, m_objArma)
  //	   try to access m_objArma->data */
  //	m_objArma->data = nullptr;
  //	BKE_id_free(bmain, m_objArma);
  //}

  // Keep a copy of the original armature so we can fix drivers later
  m_origObjArma = obj;
  m_objArma = m_origObjArma;  // BKE_object_copy(bmain, armature);
  // m_objArma->data = BKE_armature_copy(bmain, (bArmature *)armature->data);
  // During object replication ob->data is increase, we decrease it now because we get a copy.
  // id_us_min(&((bArmature *)m_origObjArma->data)->id);
  // need this to get iTaSC working ok in the BGE
  // m_objArma->pose->flag |= POSE_GAME_ENGINE;

  if (m_objArma) {
    memcpy(m_object_to_world, m_objArma->object_to_world().ptr(), sizeof(m_object_to_world));
    LoadChannels();
  }

  bContext *C = KX_GetActiveEngine()->GetContext();
  m_runtime_obj = (Object *)BKE_id_copy_ex(
      CTX_data_main(C), &obj->id, nullptr, LIB_ID_CREATE_NO_DEG_TAG);

  bArmature *orig_arm = (bArmature *)m_origObjArma->data;
  bArmature *runtime_arm = (bArmature *)BKE_id_copy_ex(
      CTX_data_main(C),
      &orig_arm->id,
      nullptr,
      LIB_ID_CREATE_NO_DEG_TAG  // ou LIB_ID_CREATE_LOCALIZE
  );
  m_runtime_obj->data = runtime_arm;

  Main *bmain = CTX_data_main(C);
  LISTBASE_FOREACH(Object *, ob, &bmain->objects) {
    if (ob->parent == m_origObjArma && ob->type == OB_MESH) {
      m_deformedObj = ob;
      break;
    }
  }

  RemoveArmatureModifiers(m_deformedObj);

  BKE_constraints_free(&m_runtime_obj->constraints);

  // Supprimer les contraintes sur chaque pose channel
  if (m_runtime_obj->pose) {
    for (bPoseChannel *pchan = (bPoseChannel *)m_runtime_obj->pose->chanbase.first; pchan;
         pchan = pchan->next)
    {
      BKE_constraints_free(&pchan->constraints);
    }
  }

  if (m_runtime_obj->pose) {
    BKE_pose_free(m_runtime_obj->pose);
  }
  m_runtime_obj->pose = BGE_pose_copy_and_capture_rest(m_origObjArma->pose,
                                                       /*copy_constraints=*/false,
                                                       m_runtime_obj,
                                                       m_deformedObj,
                                                       m_refPositions,
                                                       m_refPositionsNum);

  m_objArma = m_runtime_obj;

  if (m_sbModifier == nullptr && m_deformedObj) {
    m_sbModifier = (SimpleDeformModifierDataBGE *)BKE_modifier_new(eModifierType_SimpleDeformBGE);
    STRNCPY(m_sbModifier->modifier.name, "sbModifier");
    BLI_addtail(&m_deformedObj->modifiers, m_sbModifier);
    BKE_modifier_unique_name(&m_deformedObj->modifiers, (ModifierData *)m_sbModifier);
    BKE_modifiers_persistent_uid_init(*m_deformedObj, m_sbModifier->modifier);
    DEG_relations_tag_update(CTX_data_main(C));
  }

  KX_GameObject::SetBlenderObject(m_runtime_obj);
}

void BL_ArmatureObject::LoadConstraints(BL_SceneConverter *converter)
{
  // first delete any existing constraint (should not have any)
  m_controlledConstraints->ReleaseAndRemoveAll();

  // list all the constraint and convert them to BL_ArmatureConstraint
  // get the persistent pose structure

  // and locate the constraint
  for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
       pchan = pchan->next) {
    for (bConstraint *pcon = (bConstraint *)pchan->constraints.first; pcon; pcon = pcon->next) {
      if (pcon->flag & CONSTRAINT_DISABLE) {
        continue;
      }
      // which constraint should we support?
      switch (pcon->type) {
        case CONSTRAINT_TYPE_TRACKTO:
        case CONSTRAINT_TYPE_DAMPTRACK:
        case CONSTRAINT_TYPE_KINEMATIC:
        case CONSTRAINT_TYPE_ROTLIKE:
        case CONSTRAINT_TYPE_LOCLIKE:
        case CONSTRAINT_TYPE_MINMAX:
        case CONSTRAINT_TYPE_SIZELIKE:
        case CONSTRAINT_TYPE_LOCKTRACK:
        case CONSTRAINT_TYPE_STRETCHTO:
        case CONSTRAINT_TYPE_CLAMPTO:
        case CONSTRAINT_TYPE_TRANSFORM:
        case CONSTRAINT_TYPE_DISTLIMIT:
        case CONSTRAINT_TYPE_TRANSLIKE: {
          const bConstraintTypeInfo *cti = BKE_constraint_typeinfo_get(pcon);
          KX_GameObject *gametarget = nullptr;
          KX_GameObject *gamesubtarget = nullptr;
          if (cti && cti->get_constraint_targets) {
            ListBase listb = {nullptr, nullptr};
            cti->get_constraint_targets(pcon, &listb);
            if (listb.first) {
              bConstraintTarget *target = (bConstraintTarget *)listb.first;
              if (target->tar && target->tar != m_objArma) {
                // only remember external objects, self target is handled automatically
                gametarget = converter->FindGameObject(target->tar);
              }
              if (target->next != nullptr) {
                // secondary target
                target = target->next;
                if (target->tar && target->tar != m_objArma) {
                  // only track external object
                  gamesubtarget = converter->FindGameObject(target->tar);
                }
              }
            }
            if (cti->flush_constraint_targets) {
              cti->flush_constraint_targets(pcon, &listb, 1);
            }
          }
          BL_ArmatureConstraint *constraint = new BL_ArmatureConstraint(
              this, pchan, pcon, gametarget, gamesubtarget);
          m_controlledConstraints->Add(constraint);
        }
      }
    }
  }

  // If we have constraints, make sure we get treated as an "animated" object
  if (m_controlledConstraints->GetCount() > 0) {
    GetActionManager();
  }
}

size_t BL_ArmatureObject::GetConstraintNumber() const
{
  return m_controlledConstraints->GetCount();
}

BL_ArmatureConstraint *BL_ArmatureObject::GetConstraint(const std::string &posechannel,
                                                        const std::string &constraintname)
{
  return m_controlledConstraints->FindIf(
      [&posechannel, &constraintname](BL_ArmatureConstraint *constraint) {
        return constraint->Match(posechannel, constraintname);
      });
}

BL_ArmatureConstraint *BL_ArmatureObject::GetConstraint(const std::string &posechannelconstraint)
{
  return static_cast<BL_ArmatureConstraint *>(
      m_controlledConstraints->FindValue(posechannelconstraint));
}

BL_ArmatureConstraint *BL_ArmatureObject::GetConstraint(int index)
{
  return static_cast<BL_ArmatureConstraint *>(m_controlledConstraints->GetValue(index));
}

/* this function is called to populate the m_poseChannels list */
void BL_ArmatureObject::LoadChannels()
{
  m_poseChannels = new EXP_ListValue<BL_ArmatureChannel>();
  for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan; pchan = (bPoseChannel *)pchan->next) {
    BL_ArmatureChannel *channel = new BL_ArmatureChannel(this, pchan);
    m_poseChannels->Add(channel);
  }
}

size_t BL_ArmatureObject::GetChannelNumber() const
{
  return m_poseChannels->GetCount();
}

BL_ArmatureChannel *BL_ArmatureObject::GetChannel(bPoseChannel *pchan)
{
  return m_poseChannels->FindIf(
      [&pchan](BL_ArmatureChannel *channel) { return channel->m_posechannel == pchan; });
}

BL_ArmatureChannel *BL_ArmatureObject::GetChannel(const std::string &str)
{
  return static_cast<BL_ArmatureChannel *>(m_poseChannels->FindValue(str));
}

BL_ArmatureChannel *BL_ArmatureObject::GetChannel(int index)
{
  if (index < 0 || index >= m_poseChannels->GetCount()) {
    return nullptr;
  }
  return static_cast<BL_ArmatureChannel *>(m_poseChannels->GetValue(index));
}

KX_PythonProxy *BL_ArmatureObject::NewInstance()
{
  return new BL_ArmatureObject(*this);
}

void BL_ArmatureObject::ProcessReplica()
{
  KX_GameObject::ProcessReplica();

  // Replicate each constraints.
  m_controlledConstraints = static_cast<EXP_ListValue<BL_ArmatureConstraint> *>(
      m_controlledConstraints->GetReplica());

  m_objArma = m_pBlenderObject;

  LoadChannels();
}

int BL_ArmatureObject::GetGameObjectType() const
{
  return OBJ_ARMATURE;
}

void BL_ArmatureObject::ReParentLogic()
{
  for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
    constraint->ReParent(this);
  }
  KX_GameObject::ReParentLogic();
}

void BL_ArmatureObject::Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map)
{
  for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
    constraint->Relink(obj_map);
  }
  KX_GameObject::Relink(obj_map);
}

bool BL_ArmatureObject::UnlinkObject(SCA_IObject *clientobj)
{
  // clientobj is being deleted, make sure we don't hold any reference to it
  bool res = false;
  for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
    res |= constraint->UnlinkObject(clientobj);
  }
  return res;
}

void BL_ArmatureObject::ApplyPose()
{
  if (m_lastapplyframe != m_lastframe) {
    // update the constraint if any, first put them all off so that only the active ones will be
    // updated
    for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
      constraint->UpdateTarget();
    }
    // update ourself
    UpdateBlenderObjectMatrix(m_runtime_obj);
    bContext *C = KX_GetActiveEngine()->GetContext();
    Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
    BKE_pose_where_is(depsgraph, GetScene()->GetBlenderScene(), m_runtime_obj);
    // restore ourself
    memcpy(m_runtime_obj->runtime->object_to_world.ptr(), m_object_to_world, sizeof(m_object_to_world));
    m_lastapplyframe = m_lastframe;
  }
}

void print_matrix(const float m[4][4])
{
  for (int i = 0; i < 4; ++i) {
    printf("[%.4f %.4f %.4f %.4f]\n", m[i][0], m[i][1], m[i][2], m[i][3]);
  }
}

void BL_ArmatureObject::SetPoseByAction(bAction *action, AnimationEvalContext *evalCtx)
{
  // 1. Appliquer l'action à l'armature (mise à jour de la pose)
  PointerRNA ptrrna = RNA_id_pointer_create(&m_runtime_obj->id);
  const blender::animrig::slot_handle_t slot_handle = blender::animrig::first_slot_handle(*action);
  BKE_pose_rest(m_runtime_obj->pose, false);
  animsys_evaluate_action(&ptrrna, action, slot_handle, evalCtx, false);

  // 2. Forcer la mise à jour de la pose et du mesh
  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ApplyPose();

  Object *deformed_eval = DEG_get_evaluated(depsgraph, m_deformedObj);
  Mesh *mesh = static_cast<Mesh *>(deformed_eval->data);
  blender::Span<MDeformVert> dverts = mesh->deform_verts();
  blender::Span<blender::float3> positions(m_refPositions.data(), m_refPositionsNum);

  // 3. Préparer le buffer de sortie CPU
  if (!m_sbCoords) {
    m_sbCoords = (float(*)[3])MEM_callocN(sizeof(float[3]) * positions.size(), __func__);
  }

  // 4. Calcul des matrices globales
  float premat[4][4], postmat[4][4], obinv[4][4];
  copy_m4_m4(premat, m_deformedObj->object_to_world().ptr());
  invert_m4_m4(obinv, m_deformedObj->object_to_world().ptr());
  mul_m4_m4m4(postmat, obinv, m_runtime_obj->object_to_world().ptr());
  invert_m4_m4(premat, postmat);

  // 5. Préparer les buffers GPU (positions, poids, indices, matrices)
  const int num_vertices = positions.size();
  const int num_bones = (m_runtime_obj && m_runtime_obj->pose) ?
                            BLI_listbase_count(&m_runtime_obj->pose->chanbase) :
                            0;

  std::vector<float> in_positions(num_vertices * 3);
  std::vector<int> in_indices(num_vertices * 4);    // 4 influences max/vertex
  std::vector<float> in_weights(num_vertices * 4);  // 4 influences max/vertex
  std::vector<float> bone_matrices(num_bones * 16, 0.0f);

  // 6. Remplir les buffers d'entrée
  for (int v = 0; v < num_vertices; ++v) {
    in_positions[v * 3 + 0] = positions[v].x;
    in_positions[v * 3 + 1] = positions[v].y;
    in_positions[v * 3 + 2] = positions[v].z;
    for (int i = 0; i < 4; ++i) {
      if (i < dverts[v].totweight) {
        in_indices[v * 4 + i] = dverts[v].dw[i].def_nr;
        in_weights[v * 4 + i] = dverts[v].dw[i].weight;
      }
      else {
        in_indices[v * 4 + i] = 0;
        in_weights[v * 4 + i] = 0.0f;
      }
    }
  }

  // 7. Calcul des matrices finales par bone
  // On suppose que l'ordre des bones dans pose->chanbase correspond à l'index utilisé dans les
  // indices
  std::vector<bPoseChannel *> bone_channels;
  bone_channels.reserve(num_bones);
  for (bPoseChannel *pchan = (bPoseChannel *)m_runtime_obj->pose->chanbase.first; pchan;
       pchan = pchan->next)
  {
    bone_channels.push_back(pchan);
  }

  for (int b = 0; b < num_bones; ++b) {
    bPoseChannel *pchan = bone_channels[b];
    // inv_rest_mat = inverse(pchan->bone->arm_mat)
    float inv_rest_mat[4][4];
    invert_m4_m4(inv_rest_mat, pchan->bone->arm_mat);

    // final_bone_matrix = postmat * pose_mat * inv_rest_mat * premat
    float tmp1[4][4], tmp2[4][4], final[4][4];
    mul_m4_m4m4(tmp1, pchan->pose_mat, inv_rest_mat);
    mul_m4_m4m4(tmp2, tmp1, premat);
    mul_m4_m4m4(final, postmat, tmp2);

    // Stocker dans bone_matrices (row-major)
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        bone_matrices[b * 16 + row * 4 + col] = final[row][col];
      }
    }
  }

  // 8. Créer les SSBO Blender GPU
  if (!ssbo_in_pos) {
    ssbo_in_pos = GPU_storagebuf_create(sizeof(float) * num_vertices * 3);
  }
  GPU_storagebuf_update(ssbo_in_pos, in_positions.data());
  if (!ssbo_in_idx) {
    ssbo_in_idx = GPU_storagebuf_create(sizeof(int) * num_vertices * 4);
  }
  GPU_storagebuf_update(ssbo_in_idx, in_indices.data());
  if (!ssbo_in_wgt) {
    ssbo_in_wgt = GPU_storagebuf_create(sizeof(float) * num_vertices * 4);
  }
  GPU_storagebuf_update(ssbo_in_wgt, in_weights.data());
  if (!ssbo_bone_mat) {
    ssbo_bone_mat = GPU_storagebuf_create(sizeof(float) * num_bones * 16);
  }
  GPU_storagebuf_update(ssbo_bone_mat, bone_matrices.data());
  if (!ssbo_out) {
    ssbo_out = GPU_storagebuf_create(sizeof(float) * num_vertices * 3);
  }

  using namespace blender::gpu::shader;

  if (!m_shader) {
    ShaderCreateInfo info("BGE_Armature_Skinning");
    info.local_group_size(256, 1, 1);

    info.typedef_source_generated = R"(
layout(std430, binding = 0) buffer InPos {
  vec3 in_pos[];
};
layout(std430, binding = 1) buffer InIdx {
  ivec4 in_idx[];
};
layout(std430, binding = 2) buffer InWgt {
  vec4 in_wgt[];
};
layout(std430, binding = 3) buffer BoneMat {
  mat4 bone_mat[];
};
layout(std430, binding = 4) buffer OutPos {
  vec3 out_pos[];
};
)";

    info.compute_source("draw_colormanagement_lib.glsl");
    info.compute_source_generated = R"(
    void main() {
      uint v = gl_GlobalInvocationID.x;
      vec4 pos = vec4(in_pos[v], 1.0);
      vec3 skinned = vec3(0.0);
      float total_weight = 0.0;
      for (int i = 0; i < 4; ++i) {
        int bone = in_idx[v][i];
        float w = in_wgt[v][i];
        skinned += (bone_mat[bone] * pos).xyz * w;
        total_weight += w;
      }
      if (total_weight > 0.0)
        skinned /= total_weight;
      else
        skinned = in_pos[v];
      out_pos[v] = skinned;
    }
  )";
    m_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  }

  GPUShader *shader = m_shader;

  // 9. Bind et dispatch
  GPU_shader_bind(shader);
  GPU_storagebuf_bind(ssbo_in_pos, 0);
  GPU_storagebuf_bind(ssbo_in_idx, 1);
  GPU_storagebuf_bind(ssbo_in_wgt, 2);
  GPU_storagebuf_bind(ssbo_bone_mat, 3);
  GPU_storagebuf_bind(ssbo_out, 4);
  GPU_compute_dispatch(shader, (num_vertices + 255) / 256, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  // 10. Lire le résultat côté CPU
  GPU_storagebuf_read(ssbo_out, m_sbCoords);

  // 12. Mettre à jour le modificateur et notifier Blender
  m_sbModifier->vertcoos = m_sbCoords;
  DEG_id_tag_update(&m_deformedObj->id, ID_RECALC_GEOMETRY);
}

void BL_ArmatureObject::BlendInPose(bPose *blend_pose, float weight, short mode)
{
  game_blend_poses(m_runtime_obj->pose, blend_pose, weight, mode);
}

bool BL_ArmatureObject::UpdateTimestep(double curtime)
{
  if (curtime != m_lastframe) {
    /* Compute the timestep for the underlying IK algorithm,
     * in the GE, we use ctime to store the timestep.
     */
    m_runtime_obj->pose->ctime = (float)(curtime - m_lastframe);
    m_lastframe = curtime;
  }

  return false;
}

Object *BL_ArmatureObject::GetArmatureObject()
{
  return m_objArma;
}
Object *BL_ArmatureObject::GetOrigArmatureObject()
{
  return m_origObjArma;
}

void BL_ArmatureObject::GetPose(bPose **pose) const
{
  /* If the caller supplies a null pose, create a new one. */
  /* Otherwise, copy the armature's pose channels into the caller-supplied pose */

  if (!*pose) {
    /* probably not to good of an idea to
     * duplicate everything, but it clears up
     * a crash and memory leakage when
     * &SCA_ActionActuator::m_pose is freed
     */
    BKE_pose_copy_data(pose, m_runtime_obj->pose, 1);
  }
  else {
    if (*pose == m_runtime_obj->pose) {
      // no need to copy if the pointers are the same
      return;
    }

    extract_pose_from_pose(*pose, m_runtime_obj->pose);
  }
}

bPose *BL_ArmatureObject::GetPose() const
{
  return m_objArma->pose;
}

double BL_ArmatureObject::GetLastFrame()
{
  return m_lastframe;
}

bool BL_ArmatureObject::GetBoneMatrix(Bone *bone, MT_Matrix4x4 &matrix)
{
  ApplyPose();
  bPoseChannel *pchan = BKE_pose_channel_find_name(m_objArma->pose, bone->name);
  if (pchan) {
    matrix.setValue(&pchan->pose_mat[0][0]);
  }

  return (pchan != nullptr);
}

bool BL_ArmatureObject::GetDrawDebug() const
{
  return m_drawDebug;
}

void BL_ArmatureObject::DrawDebug(RAS_DebugDraw &debugDraw)
{
  const MT_Vector3 &scale = NodeGetWorldScaling();
  const MT_Matrix3x3 &rot = NodeGetWorldOrientation();
  const MT_Vector3 &pos = NodeGetWorldPosition();

  for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
       pchan = pchan->next) {
    MT_Vector3 head = rot * (MT_Vector3(pchan->pose_head) * scale) + pos;
    MT_Vector3 tail = rot * (MT_Vector3(pchan->pose_tail) * scale) + pos;
    debugDraw.DrawLine(tail, head, MT_Vector4(1.0f, 0.0f, 0.0f, 1.0f));
  }
  m_drawDebug = false;
}

float BL_ArmatureObject::GetBoneLength(Bone *bone) const
{
  return (float)(MT_Vector3(bone->head) - MT_Vector3(bone->tail)).length();
}

#ifdef WITH_PYTHON

// PYTHON
PyObject *BL_ArmatureObject::game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  BL_ArmatureObject *obj = new BL_ArmatureObject();

  PyObject *proxy = py_base_new(type, PyTuple_Pack(1, obj->GetProxy()), kwds);
  if (!proxy) {
    delete obj;
    return nullptr;
  }

  return proxy;
}

PyTypeObject BL_ArmatureObject::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "BL_ArmatureObject",
                                        sizeof(EXP_PyObjectPlus_Proxy),
                                        0,
                                        py_base_dealloc,
                                        0,
                                        0,
                                        0,
                                        0,
                                        py_base_repr,
                                        0,
                                        &KX_GameObject::Sequence,
                                        &KX_GameObject::Mapping,
                                        0,
                                        0,
                                        0,
                                        nullptr,
                                        nullptr,
                                        0,
                                        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        Methods,
                                        0,
                                        0,
                                        &KX_GameObject::Type,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        game_object_new};

PyMethodDef BL_ArmatureObject::Methods[] = {
    EXP_PYMETHODTABLE_NOARGS(BL_ArmatureObject, update),
    EXP_PYMETHODTABLE_NOARGS(BL_ArmatureObject, draw),
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef BL_ArmatureObject::Attributes[] = {

    EXP_PYATTRIBUTE_RO_FUNCTION("constraints", BL_ArmatureObject, pyattr_get_constraints),
    EXP_PYATTRIBUTE_RO_FUNCTION("channels", BL_ArmatureObject, pyattr_get_channels),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *BL_ArmatureObject::pyattr_get_constraints(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef)
{
  BL_ArmatureObject *self = static_cast<BL_ArmatureObject *>(self_v);
  return self->m_controlledConstraints->GetProxy();
}

PyObject *BL_ArmatureObject::pyattr_get_channels(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
  BL_ArmatureObject *self = static_cast<BL_ArmatureObject *>(self_v);
  return self->m_poseChannels->GetProxy();
}

EXP_PYMETHODDEF_DOC_NOARGS(
    BL_ArmatureObject,
    update,
    "update()\n"
    "Make sure that the armature will be updated on next graphic frame.\n"
    "This is automatically done if a KX_ArmatureActuator with mode run is active\n"
    "or if an action is playing. This function is useful in other cases.\n")
{
  UpdateTimestep(KX_GetActiveEngine()->GetFrameTime());
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(BL_ArmatureObject, draw, "Draw Debug Armature")
{
  /* Armature bones are updated later, so we only set to true a flag
   * to request a debug draw later in ApplyPose after updating bones. */
  m_drawDebug = true;
  Py_RETURN_NONE;
}

#endif  // WITH_PYTHON

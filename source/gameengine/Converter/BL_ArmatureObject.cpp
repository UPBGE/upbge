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

#include "ANIM_action.hh"
#include "BKE_animsys.h"
#include "BKE_armature.hh"
#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_deform.hh"
#include "BKE_fcurve.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_scene.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_set.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_anim_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"
#include "RNA_access.hh"

#include "BL_Action.h"
#include "BL_SceneConverter.h"
#include "KX_Globals.h"
#include "KX_Scene.h"

using namespace blender;

// Only allowed for Poses with identical channels.
void BL_ArmatureObject::GameBlendPose(blender::bPose *dst, blender::bPose *src, float srcweight, short mode)
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

  blender::bPoseChannel *schan = (blender::bPoseChannel *)src->chanbase.first;
  for (blender::bPoseChannel *dchan = (blender::bPoseChannel *)dst->chanbase.first; dchan;
       dchan = (blender::bPoseChannel *)dchan->next, schan = (blender::bPoseChannel *)schan->next)
  {
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
    for (blender::bConstraint *dcon = (blender::bConstraint *)dchan->constraints.first,
                     *scon = (blender::bConstraint *)schan->constraints.first;
         dcon && scon;
         dcon = dcon->next, scon = scon->next)
    {
      /* no 'add' option for constraint blending */
      dcon->enforce = dcon->enforce * (1.0f - srcweight) + scon->enforce * srcweight;
    }
  }

  /* this pose is now in src time */
  dst->ctime = src->ctime;
}

BL_ArmatureObject::BL_ArmatureObject()
    : KX_GameObject(),
      m_lastframe(0.0),
      m_drawDebug(false),
      m_lastapplyframe(0.0),
      m_poseDirtyGeneration(1),
      m_poseAppliedGeneration(0)
{
  m_controlledConstraints = new EXP_ListValue<BL_ArmatureConstraint>();
  m_poseChannels = nullptr;
  m_previousArmature = nullptr;
  // m_replicaMeshes default constructed
}

BL_ArmatureObject::~BL_ArmatureObject()
{
  m_poseChannels->Release();
  m_poseChannels = nullptr;
  m_controlledConstraints->Release();

  if (!m_replicaMeshes.empty() && m_isReplica) {
    blender::bContext *C = KX_GetActiveEngine()->GetContext();
    blender::Main *bmain = C ? CTX_data_main(C) : nullptr;

    /* Restore original data pointers first */
    for (auto &item : m_replacedOriginalData) {
      blender::Object *child_ob = item.first;
      blender::ID *orig_data = item.second;
      if (child_ob && orig_data) {
        child_ob->data = orig_data;
      }
    }

    /* Delete replicas meshes */
    for (auto &item : m_replicaMeshes) {
      blender::Mesh *replica = item.second;
      if (replica) {
        /* Delete the mesh from blender::Main database */
        BKE_id_delete(bmain, (blender::ID *)replica);
      }
    }

    m_replicaMeshes.clear();
    m_replacedOriginalData.clear();

    /* Update depsgraph after cleanup (same logic than in KX_GameObject::DiscardRenderedObject) */
    DEG_relations_tag_update(bmain);
  }
}

void BL_ArmatureObject::SetBlenderObject(blender::Object *obj)
{
  KX_GameObject::SetBlenderObject(obj);
  m_objArma = obj;

  if (m_objArma) {
    memcpy(m_object_to_world, m_objArma->object_to_world().ptr(), sizeof(m_object_to_world));
    LoadChannels();
  }
}

void BL_ArmatureObject::RemapParentChildren()
{
  /* When an armature is replicated, child objects that had an armature modifier
   * pointing to the original armature must be remapped to point to the new replica. */
  for (KX_GameObject *child : GetChildren()) {
    blender::Object *child_ob = child->GetBlenderObject();
    if (!child_ob) {
      continue;
    }
    for (ModifierData *md = (ModifierData *)child_ob->modifiers.first; md; md = md->next) {
      if (md->type == eModifierType_Armature) {
        ArmatureModifierData *amd = (ArmatureModifierData *)md;
        if (amd && amd->object == m_previousArmature) {
          amd->object = m_objArma;
          bool is_using_gpu_deform = amd->deform_method & ARM_DEFORM_METHOD_GPU && !m_isUpbgeDupliInstance &&
                                     !child->IsUpbgeDupliInstance();
          if (child->IsReplica() && is_using_gpu_deform) {
            if (m_replicaMeshes.find(child_ob) == m_replicaMeshes.end()) {
              blender::bContext *C = KX_GetActiveEngine()->GetContext();
              blender::Main *bmain = CTX_data_main(C);

              /* Cache the replica mesh data to avoid multiple lookups. */
              blender::Mesh *orig = (blender::Mesh *)child_ob->data;
              blender::Mesh *replica = (blender::Mesh *)BKE_id_copy_ex(bmain, (blender::ID *)orig, nullptr, 0);

              child_ob->data = id_cast<ID *>(replica);

              /* Store original data pointer to restore it later. */
              if (m_replacedOriginalData.find(child_ob) == m_replacedOriginalData.end()) {
                m_replacedOriginalData[child_ob] = (blender::ID *)orig;
              }
              DEG_id_tag_update(&child_ob->id, ID_RECALC_GEOMETRY);
              DEG_relations_tag_update(bmain);

              m_replicaMeshes[child_ob] = replica;
            }
          }
        }
      }
    }
  }
}

void BL_ArmatureObject::LoadConstraints(BL_SceneConverter *converter)
{
  // first delete any existing constraint (should not have any)
  m_controlledConstraints->ReleaseAndRemoveAll();

  // list all the constraint and convert them to BL_ArmatureConstraint
  // get the persistent pose structure

  // and locate the constraint
  for (blender::bPoseChannel *pchan = (blender::bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
       pchan = pchan->next) {
    for (blender::bConstraint *pcon = (blender::bConstraint *)pchan->constraints.first; pcon; pcon = pcon->next) {
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
            ListBaseT <bConstraintTarget> listb = {nullptr, nullptr};
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
        default:
          break;
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
      //    m_controlledConstraints->FindValue(posechannelconstraint));
      m_controlledConstraints->FindValue(posechannelconstraint));
}

BL_ArmatureConstraint *BL_ArmatureObject::GetConstraint(int index)
{
  return static_cast<BL_ArmatureConstraint *>(m_controlledConstraints->GetValue(index));
}

/* this function is called to populate the m_poseChannels list */
void BL_ArmatureObject::LoadChannels()
{
  m_poseChannelLookup.clear();
  m_poseChannels = new EXP_ListValue<BL_ArmatureChannel>();
  for (blender::bPoseChannel *pchan = (blender::bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
       pchan = (blender::bPoseChannel *)pchan->next) {
    BL_ArmatureChannel *channel = new BL_ArmatureChannel(this, pchan);
    m_poseChannels->Add(channel);
    m_poseChannelLookup.emplace(pchan->name, pchan);
  }
  MarkPoseDirty();
}

size_t BL_ArmatureObject::GetChannelNumber() const
{
  return m_poseChannels->GetCount();
}

BL_ArmatureChannel *BL_ArmatureObject::GetChannel(blender::bPoseChannel *pchan)
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
  m_previousArmature = m_objArma;

  KX_GameObject::ProcessReplica();

  // Replicate each constraints.
  m_controlledConstraints = static_cast<EXP_ListValue<BL_ArmatureConstraint> *>(
  // m_controlledConstraints->GetReplica());
  m_controlledConstraints->GetReplica());

  m_objArma = m_pBlenderObject;

  LoadChannels();
  m_lastapplyframe = -1.0;
  m_poseAppliedGeneration = 0;
  /* The source's targets are in the source's pose. */
  m_resolvedActions.clear();
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
  const bool pose_dirty = m_poseAppliedGeneration != m_poseDirtyGeneration;
  if (m_lastapplyframe == m_lastframe && !pose_dirty) {
    return;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  KX_Scene *scene = GetScene();
  blender::Depsgraph *depsgraph = scene ? scene->GetAnimationDepsgraph() : nullptr;
  if (depsgraph == nullptr) {
    blender::bContext *context = engine ? engine->GetContext() : nullptr;
    depsgraph = context ? CTX_data_depsgraph_pointer(context) : nullptr;
  }
  blender::Scene *blender_scene = scene ? scene->GetBlenderScene() : nullptr;
  if (m_objArma == nullptr || m_objArma->pose == nullptr || depsgraph == nullptr ||
      blender_scene == nullptr)
  {
    return;
  }

  // Update the constraint if any, first put them all off so that only the active ones will be
  // updated.
  for (BL_ArmatureConstraint *constraint : m_controlledConstraints) {
    constraint->UpdateTarget();
  }
  if (pose_dirty) {
    DEG_id_tag_update(&m_objArma->id, blender::ID_RECALC_GEOMETRY);
  }
  BKE_pose_where_is(depsgraph, blender_scene, m_objArma);

  m_lastapplyframe = m_lastframe;
  m_poseAppliedGeneration = m_poseDirtyGeneration;
}

void BL_ArmatureObject::MarkPoseDirty()
{
  m_poseDirtyGeneration++;
  if (m_poseDirtyGeneration == 0) {
    m_poseDirtyGeneration = 1;
    m_poseAppliedGeneration = 0;
  }
}

static void compute_bendy_bones_matrices(blender::Object *armOb)
{
  /* ============================================================
   * UPDATE BENDY BONES SEGMENTS CACHE FOR BGE
   * ============================================================
   * When flush_to_original is false, BKE_pose_where_is() is not called,
   * so bendy bones segments are not updated. We need to do it manually.
   * in GPU skinning mode as depsgraph is not tagged.
   */
  if (armOb->pose) {
    for (blender::bPoseChannel &pchan : armOb->pose->chanbase) {
      /* Only update bendy bones (segments > 1) */
      Bone *bone = pchan.bone_get(*armOb);
      if (bone != nullptr && bone->segments > 1) {
        BKE_pchan_bbone_segments_cache_compute({&pchan, bone}, *(bArmature *)(armOb->data));
      }
    }
  }
}

/** The curves Blender's own loop evaluates (anim_sys.cc's is_fcurve_evaluatable). */
static bool fcurve_evaluatable(const FCurve *fcu)
{
  if (fcu->flag & (FCURVE_MUTED | FCURVE_DISABLED)) {
    return false;
  }
  if (fcu->grp != nullptr && (fcu->grp->flag & AGRP_MUTED)) {
    return false;
  }
  return !BKE_fcurve_is_empty(fcu);
}

BL_ArmatureObject::ResolvedAction &BL_ArmatureObject::ResolveAction(bAction *action,
                                                                   const int32_t slot_handle,
                                                                   const Span<FCurve *> fcurves)
{
  const bPose *pose = m_objArma->pose;
  KX_Scene *scene = GetScene();
  const uint64_t update = scene ? scene->GetAnimationUpdateCount() : 0;
  auto find = [&]() -> ResolvedAction * {
    for (ResolvedAction &candidate : m_resolvedActions) {
      if (candidate.action == action && candidate.slot_handle == slot_handle) {
        return &candidate;
      }
    }
    return nullptr;
  };
  auto still_found = [&](const ResolvedAction &resolved) {
    if (resolved.pose != pose || (pose->flag & POSE_RECALC) ||
        resolved.curves.size() != fcurves.size())
    {
      return false;
    }
    for (const int64_t i : fcurves.index_range()) {
      const ResolvedCurve &curve = resolved.curves[i];
      if (curve.fcurve != fcurves[i] || curve.rna_path != fcurves[i]->rna_path ||
          curve.array_index != fcurves[i]->array_index)
      {
        return false;
      }
    }
    return true;
  };

  ResolvedAction *resolved = find();
  if (resolved != nullptr && still_found(*resolved)) {
    resolved->animation_update = update;
    return *resolved;
  }
  /* A character can play many actions over a game: keep only those played lately. */
  std::erase_if(m_resolvedActions, [&](const ResolvedAction &entry) {
    return entry.animation_update + 1 < update;
  });
  resolved = find();
  if (resolved == nullptr) {
    resolved = &m_resolvedActions.emplace_back();
  }
  resolved->action = action;
  resolved->slot_handle = slot_handle;
  /* Blender rebuilds a pose that is due (bones added or removed) at its next graph update, which
   * can free channels: found again after it. */
  resolved->pose = (pose->flag & POSE_RECALC) ? nullptr : pose;
  resolved->animation_update = update;
  resolved->curves.clear();
  resolved->curves.reserve(fcurves.size());

  Set<const void *> channels;
  for (const bPoseChannel &pchan : pose->chanbase) {
    channels.add(&pchan);
  }
  PointerRNA owner = RNA_id_pointer_create(&m_objArma->id);
  for (FCurve *fcu : fcurves) {
    ResolvedCurve curve{fcu, fcu->rna_path, fcu->array_index, nullptr, nullptr, nullptr, 0};
    PathResolvedRNA target;
    if (BKE_animsys_rna_path_resolve(&owner, fcu->rna_path, fcu->array_index, &target) &&
        !RNA_property_is_idprop(target.prop) &&
        (target.ptr.data == m_objArma || channels.contains(target.ptr.data)))
    {
      curve.prop = target.prop;
      curve.type = target.ptr.type;
      curve.data = target.ptr.data;
      curve.prop_index = target.prop_index;
    }
    resolved->curves.push_back(curve);
  }
  return *resolved;
}

void BL_ArmatureObject::ApplyAction(blender::bAction *action, const blender::AnimationEvalContext &evalCtx)
{
  if (!m_objArma || !action) {
    return;
  }
  const animrig::slot_handle_t slot_handle = animrig::first_slot_handle(*action);
  if (m_objArma->pose == nullptr) {
    PointerRNA ptrrna = RNA_id_pointer_create(&m_objArma->id);
    animsys_evaluate_action(&ptrrna, action, slot_handle, &evalCtx, false);
  }
  else {
    /* As Blender's loop, in the curves' own order, without finding each target again. */
    const ResolvedAction &resolved = ResolveAction(
        action, slot_handle, animrig::fcurves_for_action_slot(action->wrap(), slot_handle));
    PointerRNA owner = RNA_id_pointer_create(&m_objArma->id);
    PathResolvedRNA kept;
    kept.ptr.owner_id = &m_objArma->id;
    for (const ResolvedCurve &curve : resolved.curves) {
      if (!fcurve_evaluatable(curve.fcurve)) {
        continue;
      }
      PathResolvedRNA found;
      PathResolvedRNA *target = &kept;
      if (curve.prop != nullptr) {
        kept.ptr.type = curve.type;
        kept.ptr.data = curve.data;
        kept.prop = curve.prop;
        kept.prop_index = curve.prop_index;
      }
      else if (BKE_animsys_rna_path_resolve(
                   &owner, curve.fcurve->rna_path, curve.fcurve->array_index, &found))
      {
        target = &found;
      }
      else {
        continue;
      }
      BKE_animsys_write_to_rna_path(target, calculate_fcurve(target, curve.fcurve, &evalCtx));
    }
  }
  compute_bendy_bones_matrices(m_objArma);
  MarkPoseDirty();
}

void BL_ArmatureObject::BlendInPose(blender::bPose *blend_pose, float weight, short mode)
{
  GameBlendPose(m_objArma->pose, blend_pose, weight, mode);
  MarkPoseDirty();
}

bool BL_ArmatureObject::UpdateTimestep(double curtime)
{
  if (curtime != m_lastframe) {
    /* Compute the timestep for the underlying IK algorithm,
     * in the GE, we use ctime to store the timestep.
     */
    m_objArma->pose->ctime = (float)(curtime - m_lastframe);
    m_lastframe = curtime;
  }

  return false;
}

blender::Object *BL_ArmatureObject::GetArmatureObject()
{
  return m_objArma;
}
blender::Object *BL_ArmatureObject::GetOrigArmatureObject()
{
  return m_objArma;
}

void BL_ArmatureObject::GetPose(blender::bPose **pose) const
{
  /* If the caller supplies a null pose, create a new one. */
  /* Otherwise, copy the armature's pose channels into the caller-supplied pose */

  if (!*pose) {
    /* probably not to good of an idea to
     * duplicate everything, but it clears up
     * a crash and memory leakage when
     * &SCA_ActionActuator::m_pose is freed
     */
    BKE_pose_copy_data(pose, m_objArma->pose, 1);
  }
  else {
    if (*pose == m_objArma->pose) {
      // no need to copy if the pointers are the same
      return;
    }

    extract_pose_from_pose(*pose, m_objArma->pose);
  }
}

blender::bPose *BL_ArmatureObject::GetPose() const
{
  return m_objArma->pose;
}

blender::bPoseChannel *BL_ArmatureObject::FindPoseChannel(const std::string &name) const
{
  const auto channel = m_poseChannelLookup.find(name);
  return channel == m_poseChannelLookup.end() ? nullptr : channel->second;
}

double BL_ArmatureObject::GetLastFrame()
{
  return m_lastframe;
}

bool BL_ArmatureObject::GetBoneMatrix(blender::Bone *bone, MT_Matrix4x4 &matrix)
{
  float length = 0.0f;
  return bone != nullptr && GetBoneMatrix(bone->name, matrix, length);
}

bool BL_ArmatureObject::GetBoneMatrix(const std::string &name,
                                      MT_Matrix4x4 &matrix,
                                      float &length)
{
  ApplyPose();
  blender::bPoseChannel *pchan = FindPoseChannel(name);
  blender::Bone *bone = pchan != nullptr ? pchan->bone_get(*m_objArma) : nullptr;
  if (pchan == nullptr || bone == nullptr) {
    return false;
  }
  matrix.setValue(&pchan->pose_mat[0][0]);
  length = GetBoneLength(bone);
  return true;
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

  for (blender::bPoseChannel *pchan = (blender::bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
       pchan = pchan->next)
  {
    MT_Vector3 head = rot * (MT_Vector3(pchan->pose_head) * scale) + pos;
    MT_Vector3 tail = rot * (MT_Vector3(pchan->pose_tail) * scale) + pos;
    debugDraw.DrawLine(tail, head, MT_Vector4(1.0f, 0.0f, 0.0f, 1.0f));
  }
  m_drawDebug = false;
}

float BL_ArmatureObject::GetBoneLength(blender::Bone *bone) const
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

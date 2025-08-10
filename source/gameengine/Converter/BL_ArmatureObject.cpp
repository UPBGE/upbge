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
#include "BKE_attribute.hh"
#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_scene.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"
#include "DNA_mesh_types.h"
#include "BKE_object_types.hh"
#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_string.h"
#include "DEG_depsgraph_query.hh"
#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "../draw/intern/draw_cache_extract.hh"
#include "../gpu/intern/gpu_shader_create_info.hh"
#include "DNA_meshdata_types.h"
#include "RNA_access.hh"

#include "BL_Action.h"
#include "BL_SceneConverter.h"
#include "KX_Globals.h"
#include "KX_Camera.h"

static void disable_armature_modifiers(Object *ob, std::vector<ModifierStackBackup> &backups)
{
  if (!ob) {
    return;
  }
  int idx = 0;
  for (ModifierData *md = (ModifierData *)ob->modifiers.first; md;) {
    ModifierData *next = md->next;
    if (md->type == eModifierType_Armature) {
      backups.push_back({md, idx});
      BKE_modifier_remove_from_list(ob, md);
      // Don't free original armature modifier
    }
    else {
      ++idx;
    }
    md = next;
  }
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  bContext *C = KX_GetActiveEngine()->GetContext();
  DEG_relations_tag_update(CTX_data_main(C));
}

void BL_ArmatureObject::RestoreArmatureModifierList(Object *ob)
{
  for (const ModifierStackBackup &backup : m_modifiersListbackup) {
    ModifierData *md = backup.modifier;
    ModifierData *iter = (ModifierData *)ob->modifiers.first;
    int idx = 0;
    if (backup.position == 0 || !iter) {
      BLI_addhead(&ob->modifiers, md);
    }
    else {
      while (iter && idx < backup.position - 1) {
        iter = iter->next;
        ++idx;
      }
      if (iter) {
        BLI_insertlinkafter(&ob->modifiers, iter, md);
      }
      else {
        BLI_addtail(&ob->modifiers, md);
      }
    }
  }
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  bContext *C = KX_GetActiveEngine()->GetContext();
  DEG_relations_tag_update(CTX_data_main(C));
  BKE_scene_graph_update_tagged(CTX_data_ensure_evaluated_depsgraph(C), CTX_data_main(C));
  m_modifiersListbackup.clear();
}

static void capture_rest_positions_and_normals(Object *deformed_obj,
                                               blender::Array<blender::float4> &restPositions,
                                               blender::Array<blender::float4> &restNormals)
{
  if (!deformed_obj) {
    return;
  }

  // Get rest positions from original mesh directly
  // (easier this way but can cause issues if it has modifiers)
  Mesh *orig_mesh = static_cast<Mesh *>(deformed_obj->data);

  const int num_corners = orig_mesh->corners_num;
  auto corner_verts = orig_mesh->corner_verts();
  restPositions = blender::Array<blender::float4>(num_corners);
  tbb::parallel_for(0, num_corners, [&](int i) {
    int vert_idx = corner_verts[i];
    const blender::float3 &pos = orig_mesh->vert_positions()[vert_idx];
    restPositions[i] = blender::float4(pos.x, pos.y, pos.z, 1.0f);
  });
  using namespace blender::bke;
  auto faces = orig_mesh->faces();
  auto face_normals = orig_mesh->face_normals();
  auto vert_normals = orig_mesh->vert_normals();
  auto sharp_faces = orig_mesh->attributes()
                         .lookup_or_default<bool>("sharp_face", AttrDomain::Face, false)
                         .varray;

  restNormals = blender::Array<blender::float4>(num_corners);
  using namespace blender::math;
  tbb::parallel_for(0, orig_mesh->faces_num, [&](int face) {
    auto face_range = faces[face];
    if (sharp_faces && sharp_faces[face]) {
      // Flat: face normal for each corner
      const blender::float3 &nor = face_normals[face];
      blender::float3 safe_nor = normalize(nor);
      for (int corner : face_range) {
        restNormals[corner] = blender::float4(safe_nor.x, safe_nor.y, safe_nor.z, 0.0f);
      }
    }
    else {
      // Smooth: vert normal for each corner
      for (int corner : face_range) {
        int vert = corner_verts[corner];
        const blender::float3 &nor = vert_normals[vert];
        blender::float3 safe_nor = normalize(nor);
        restNormals[corner] = blender::float4(safe_nor.x, safe_nor.y, safe_nor.z, 0.0f);
      }
    }
  });
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
  m_poseChannels = nullptr;
  m_deformedObj = nullptr;
  m_useGPUDeform = false;
  m_deformedReplicaData = nullptr;
  m_shader = nullptr;
  ssbo_in_idx = nullptr;
  ssbo_in_wgt = nullptr;
  ssbo_bone_pose_mat = nullptr;
  ssbo_premat = nullptr;
  ssbo_postmat = nullptr;
  ssbo_rest_pose = nullptr;
  ssbo_rest_normals = nullptr;
  m_refPositions = {};
  m_refNormals = {};
  in_indices = {};
  in_weights = {};
  m_modifiersListbackup = {};
}

BL_ArmatureObject::~BL_ArmatureObject()
{
  m_poseChannels->Release();
  m_poseChannels = nullptr;
  m_controlledConstraints->Release();
  if (m_isReplica) {
    for (const ModifierStackBackup &backup : m_modifiersListbackup) {
      BKE_modifier_free(backup.modifier);
    }
    m_modifiersListbackup.clear();
  }
  if (m_deformedObj && m_useGPUDeform) {
    RestoreArmatureModifierList(m_deformedObj);
  }
  m_modifiersListbackup.clear();
  m_deformedObj = nullptr;

  if (m_shader) {
    GPU_shader_free(m_shader);
    m_shader = nullptr;
  }
  if (ssbo_in_idx) {
    GPU_storagebuf_free(ssbo_in_idx);
    GPU_storagebuf_free(ssbo_in_wgt);
    GPU_storagebuf_free(ssbo_bone_pose_mat);
    GPU_storagebuf_free(ssbo_premat);
    GPU_storagebuf_free(ssbo_postmat);
    GPU_storagebuf_free(ssbo_rest_pose);
    GPU_storagebuf_free(ssbo_rest_normals);
    ssbo_in_idx = nullptr;
    ssbo_in_wgt = nullptr;
    ssbo_bone_pose_mat = nullptr;
    ssbo_premat = nullptr;
    ssbo_postmat = nullptr;
    ssbo_rest_pose = nullptr;
    ssbo_rest_normals = nullptr;
    m_refPositions = {};
    m_refNormals = {};
  }
  if (m_deformedReplicaData) {
    bContext *C = KX_GetActiveEngine()->GetContext(); 
    BKE_id_delete(CTX_data_main(C), &m_deformedReplicaData->id);
    m_deformedReplicaData = nullptr;
  }
}

void BL_ArmatureObject::SetBlenderObject(Object *obj)
{
  m_objArma = obj;

  if (m_objArma) {
    memcpy(m_object_to_world, m_objArma->object_to_world().ptr(), sizeof(m_object_to_world));
    LoadChannels();
  }

  KX_GameObject::SetBlenderObject(m_objArma);
}

bool BL_ArmatureObject::GetUseGPUDeform()
{
  return m_useGPUDeform;
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
  RestoreArmatureModifierList(m_deformedObj);
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
    UpdateBlenderObjectMatrix(m_objArma);
    bContext *C = KX_GetActiveEngine()->GetContext();
    Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
    BKE_pose_where_is(depsgraph, GetScene()->GetBlenderScene(), m_objArma);

    m_lastapplyframe = m_lastframe;
  }
}

void BL_ArmatureObject::InitSkinningBuffers()
{
  if (in_indices.empty()) {
    bContext *C = KX_GetActiveEngine()->GetContext();
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    Object *deformed_eval = DEG_get_evaluated(depsgraph, m_deformedObj);
    Mesh *mesh = static_cast<Mesh *>(deformed_eval->data);
    blender::Span<MDeformVert> dverts = mesh->deform_verts();
    auto corner_verts = mesh->corner_verts();
    int num_corners = mesh->corners_num;

    // Prepare bones and groups
    std::vector<bPoseChannel *> bone_channels;
    int num_bones = (m_objArma && m_objArma->pose) ?
                        BLI_listbase_count(&m_objArma->pose->chanbase) :
                        0;
    bone_channels.reserve(num_bones);
    for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
         pchan = pchan->next)
    {
      bone_channels.push_back(pchan);
    }
    std::map<std::string, int> group_to_bone;
    for (int b = 0; b < num_bones; ++b) {
      group_to_bone[std::string(bone_channels[b]->name)] = b;
    }
    std::vector<const char *> group_names;
    for (bDeformGroup *dg = (bDeformGroup *)mesh->vertex_group_names.first; dg; dg = dg->next) {
      group_names.push_back(dg->name);
    }

    // Fill in_indices and in_weights ssbos for compute shader
    in_indices.resize(num_corners * 4, 0);
    in_weights.resize(num_corners * 4, 0.0f);

    tbb::parallel_for(0, num_corners, [&](int v) {
      int vert_idx = corner_verts[v];
      const MDeformVert &dvert = dverts[vert_idx];
      struct Influence {
        int bone_idx;
        float weight;
      };
      std::vector<Influence> influences;

      for (int j = 0; j < dvert.totweight; ++j) {
        int def_nr = dvert.dw[j].def_nr;
        if (def_nr >= 0 && def_nr < group_names.size()) {
          const char *group_name = group_names[def_nr];
          auto it = group_to_bone.find(std::string(group_name ? group_name : ""));
          if (it != group_to_bone.end()) {
            influences.push_back({it->second, dvert.dw[j].weight});
          }
        }
      }

      if (influences.empty()) {
        for (int j = 0; j < 4; ++j) {
          in_indices[v * 4 + j] = 0;
          in_weights[v * 4 + j] = 0.0f;
        }
        return;
      }

      std::sort(influences.begin(), influences.end(), [](const Influence &a, const Influence &b) {
        return a.weight > b.weight;
      });

      float total = 0.0f;
      for (const auto &inf : influences)
        total += inf.weight;
      if (total > 1.0f && total > 0.0f) {
        for (auto &inf : influences)
          inf.weight /= total;
      }

      for (int j = 0; j < 4; ++j) {
        if (j < influences.size()) {
          in_indices[v * 4 + j] = influences[j].bone_idx;
          in_weights[v * 4 + j] = influences[j].weight;
        }
        else {
          in_indices[v * 4 + j] = 0;
          in_weights[v * 4 + j] = 0.0f;
        }
      }
    });

    // Update in_indices and in_weights ssbos only 1 time
    if (!ssbo_in_idx) {
      ssbo_in_idx = GPU_storagebuf_create(sizeof(int) * num_corners * 4);
    }
    if (!ssbo_in_wgt) {
      ssbo_in_wgt = GPU_storagebuf_create(sizeof(float) * num_corners * 4);
    }
    GPU_storagebuf_update(ssbo_in_idx, in_indices.data());
    GPU_storagebuf_update(ssbo_in_wgt, in_weights.data());
  }
}

/* For gpu sknning, we delay many variables initialisation here to have "up to date" informations.
 * It is a bit tricky in case BL_ArmatureObject is a replica (needs to have right parent/child -> armature/deformed object,
 * a render cache for the deformed object....
 */
void BL_ArmatureObject::SetPoseByAction(bAction *action, AnimationEvalContext *evalCtx)
{
  using namespace blender::gpu::shader;
  using namespace blender::draw;
  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

  /* Do the remapping parent/child both for cpu/gpu deform,
   * but don't duplicate mesh or alloc any ssbos or ssbos data
   * when running on CPU */
  if (!m_deformedObj) {
    std::vector<KX_GameObject *> children = GetChildren();
    for (KX_GameObject *child : children) {
      m_deformedObj = child->GetBlenderObject();
      LISTBASE_FOREACH (ModifierData *, md, &m_deformedObj->modifiers) {
        if (md->type == eModifierType_Armature) {
          ArmatureModifierData *amd = (ArmatureModifierData *)md;
          if (amd) {
            amd->object = child->GetBlenderObject()->parent;
            m_useGPUDeform = (amd->upbge_deformflag & ARM_DEF_GPU) != 0;
          }
        }
      }
      if (child->IsReplica() && m_useGPUDeform) {
        /* We need to replicate Mesh for deformation on GPU */
        if (!m_deformedReplicaData) {
          Mesh *orig = (Mesh *)m_deformedObj->data;
          m_deformedReplicaData = (Mesh *)BKE_id_copy_ex(CTX_data_main(C), (ID *)orig, nullptr, 0);
          m_deformedObj->data = m_deformedReplicaData;
        }
      }
    }
  }

  /* Don't fill ssbos and ssbos data when running on CPU */
  if (m_refPositions.is_empty() && m_useGPUDeform) {
    capture_rest_positions_and_normals(m_deformedObj,
                                       m_refPositions,
                                       m_refNormals);
    BKE_scene_graph_update_tagged(depsgraph, CTX_data_main(C));
  }

  // 1. apply action to armature
  PointerRNA ptrrna = RNA_id_pointer_create(&m_objArma->id);
  const blender::animrig::slot_handle_t slot_handle = blender::animrig::first_slot_handle(*action);
  BKE_pose_rest(m_objArma->pose, false);
  animsys_evaluate_action(&ptrrna, action, slot_handle, evalCtx, false);

  // 2. update pose
  ApplyPose();

  /* IF CPU ARMATURE STOP HERE */
  if (!m_useGPUDeform) {
    return;
  }

  if (m_deformedObj && m_modifiersListbackup.empty()) {
    disable_armature_modifiers(m_deformedObj, m_modifiersListbackup);
  }

  Object *deformed_eval = DEG_get_evaluated(depsgraph, m_deformedObj);
  Mesh *mesh = static_cast<Mesh *>(deformed_eval->data);

  MeshBatchCache *cache = nullptr;
  if (mesh->runtime && mesh->runtime->batch_cache) {
    cache = static_cast<MeshBatchCache *>(mesh->runtime->batch_cache);
  }

  blender::gpu::VertBuf *vbo_pos = nullptr;
  blender::gpu::VertBuf *vbo_nor = nullptr;

  if (cache) {
    vbo_pos = cache->final.buff.vbos.lookup(VBOType::Position).get();
    vbo_nor = cache->final.buff.vbos.lookup(VBOType::CornerNormal).get();
  }

  if (!vbo_pos || !vbo_nor) {
    return;
  }

  int num_corners = mesh->corner_verts().size();

  // 3. Prepare bone matrices
  const int num_bones = (m_objArma && m_objArma->pose) ?
                            BLI_listbase_count(&m_objArma->pose->chanbase) :
                            0;

  if (!ssbo_bone_pose_mat) {
    ssbo_bone_pose_mat = GPU_storagebuf_create(sizeof(float) * num_bones * 16);
  }

  std::vector<float> bone_pose_matrices(num_bones * 16, 0.0f);

  std::vector<bPoseChannel *> bone_channels;
  bone_channels.reserve(num_bones);
  for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
       pchan = pchan->next)
  {
    bone_channels.push_back(pchan);
  }
  for (int b = 0; b < num_bones; ++b) {
    bPoseChannel *pchan = bone_channels[b];
    float bone_rest_inv[4][4];
    invert_m4_m4(bone_rest_inv, pchan->bone->arm_mat);
    float bone_deform[4][4];
    mul_m4_m4m4(bone_deform, pchan->pose_mat, bone_rest_inv);
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        bone_pose_matrices[b * 16 + row * 4 + col] = bone_deform[row][col];
      }
    }
  }
  GPU_storagebuf_update(ssbo_bone_pose_mat, bone_pose_matrices.data());

  InitSkinningBuffers();

  // 4. Prepare transform matrices
  float premat[4][4], postmat[4][4], obinv[4][4];
  copy_m4_m4(premat, m_deformedObj->object_to_world().ptr());
  invert_m4_m4(obinv, m_deformedObj->object_to_world().ptr());
  mul_m4_m4m4(postmat, obinv, m_objArma->object_to_world().ptr());
  invert_m4_m4(premat, postmat);

  if (!ssbo_premat) {
    ssbo_premat = GPU_storagebuf_create(sizeof(float) * 16);
  }
  GPU_storagebuf_update(ssbo_premat, &premat[0][0]);
  if (!ssbo_postmat) {
    ssbo_postmat = GPU_storagebuf_create(sizeof(float) * 16);
  }
  GPU_storagebuf_update(ssbo_postmat, &postmat[0][0]);

  // 5. Prepare rest positions and normals
  if (!ssbo_rest_pose) {
    ssbo_rest_pose = GPU_storagebuf_create(sizeof(float) * 4 * num_corners);
    GPU_storagebuf_update(ssbo_rest_pose, m_refPositions.data());
  }
  if (!ssbo_rest_normals) {
    ssbo_rest_normals = GPU_storagebuf_create(sizeof(float) * 4 * num_corners);
    GPU_storagebuf_update(ssbo_rest_normals, m_refNormals.data());
  }

  // 6. Compile skinning shader
  if (!m_shader) {
    ShaderCreateInfo info("BGE_Armature_Skinning_CPU_Logic");
    info.local_group_size(256, 1, 1);
    info.compute_source("draw_colormanagement_lib.glsl");
    info.storage_buf(0, Qualifier::write, "vec4", "positions[]");
    info.storage_buf(1, Qualifier::write, "uint", "normals[]");
    info.storage_buf(2, Qualifier::read, "ivec4", "in_idx[]");
    info.storage_buf(3, Qualifier::read, "vec4", "in_wgt[]");
    info.storage_buf(4, Qualifier::read, "mat4", "bone_pose_mat[]");
    info.storage_buf(5, Qualifier::read, "mat4", "premat[]");
    info.storage_buf(6, Qualifier::read, "vec4", "rest_positions[]");
    info.storage_buf(7, Qualifier::read, "vec4", "rest_normals[]");
    info.storage_buf(8, Qualifier::read, "mat4", "postmat[]");
    info.compute_source_generated = R"(
uint normal_pack(vec3 normal)
{
  normal = normalize(normal);
  ivec3 enc = ivec3(clamp(round(normal * 511.0), -512.0, 511.0));
  uint x = uint(enc.x) & 0x3FFu;
  uint y = uint(enc.y) & 0x3FFu;
  uint z = uint(enc.z) & 0x3FFu;
  return x | (y << 10) | (z << 20);
}

vec3 safe_normalize(vec3 v)
{
  float len = length(v);
  if (len > 1e-8)
    return v / len;
  else
    return vec3(0.0, 0.0, 1.0);
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= rest_positions.length()) return;
  vec4 rest_pos = premat[0] * rest_positions[v];
  vec4 skinned = vec4(0.0);
  float total_weight = 0.0;
  for (int i = 0; i < 4; ++i) {
    int bone_idx = in_idx[v][i];
    float w = in_wgt[v][i];
    if (w > 0.0) {
      mat4 deform_mat = bone_pose_mat[bone_idx];
      vec4 transformed = deform_mat * rest_pos;
      skinned += transformed * w;
      total_weight += w;
    }
  }
  // Correction Blender-like :
  vec4 finalpos = skinned + rest_pos * (1.0 - total_weight);
  positions[v] = postmat[0] * finalpos;
  // Calcul du skinning des normales avec la même matrice
  vec4 n4 = premat[0] * rest_normals[v];
  vec3 n = n4.xyz;
  vec3 skinned_n = vec3(0.0);
  float total_weight_n = 0.0;
  for (int i = 0; i < 4; ++i) {
    int bone_idx = in_idx[v][i];
    float w = in_wgt[v][i];
    if (w > 0.0) {
      mat3 rot = mat3(bone_pose_mat[bone_idx]);
      mat3 normal_matrix = transpose(inverse(rot));
      skinned_n += (normal_matrix * n) * w;
      total_weight_n += w;
    }
  }
  if (total_weight_n < 1.0) {
    skinned_n += n * (1.0 - total_weight_n);
  }
  skinned_n = normalize(skinned_n);
  vec4 finalnor = postmat[0] * vec4(skinned_n, 0.0);
  normals[v] = normal_pack(finalnor.xyz);
}
    )";
    m_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  }

  // 7. Dispatch compute shader
  GPUShader *shader = m_shader;
  GPU_shader_bind(shader);
  vbo_pos->bind_as_ssbo(0);
  vbo_nor->bind_as_ssbo(1);
  GPU_storagebuf_bind(ssbo_in_idx, 2);
  GPU_storagebuf_bind(ssbo_in_wgt, 3);
  GPU_storagebuf_bind(ssbo_bone_pose_mat, 4);
  GPU_storagebuf_bind(ssbo_premat, 5);
  GPU_storagebuf_bind(ssbo_rest_pose, 6);
  GPU_storagebuf_bind(ssbo_rest_normals, 7);
  GPU_storagebuf_bind(ssbo_postmat, 8);

  const int group_size = 256;
  const int num_groups = (num_corners + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  GPU_storagebuf_unbind(ssbo_in_idx);
  GPU_storagebuf_unbind(ssbo_in_wgt);
  GPU_storagebuf_unbind(ssbo_bone_pose_mat);
  GPU_storagebuf_unbind(ssbo_premat);
  GPU_storagebuf_unbind(ssbo_rest_pose);
  GPU_storagebuf_unbind(ssbo_rest_normals);
  GPU_storagebuf_unbind(ssbo_postmat);
  GPU_shader_unbind();

  // Notify the dependency graph that the deformed mesh's transform has changed.
  // This updates the object_to_world matrices used by EEVEE without invalidating
  // render caches, ensuring correct shading after GPU skinning.
  DEG_id_tag_update(&m_deformedObj->id, ID_RECALC_TRANSFORM);
}

void BL_ArmatureObject::BlendInPose(bPose *blend_pose, float weight, short mode)
{
  game_blend_poses(m_objArma->pose, blend_pose, weight, mode);
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

Object *BL_ArmatureObject::GetArmatureObject()
{
  return m_objArma;
}
Object *BL_ArmatureObject::GetOrigArmatureObject()
{
  return m_objArma;
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

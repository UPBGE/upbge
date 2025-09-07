﻿/*
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
#include "BKE_armature.hh"
#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_deform.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_scene.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_threads.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "../draw/intern/draw_cache_extract.hh"
#include "../gpu/intern/gpu_shader_create_info.hh"
#include "RNA_access.hh"

#include "BL_Action.h"
#include "BL_SceneConverter.h"
#include "KX_Globals.h"

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

void BL_ArmatureObject::CaptureRestPositionsAndNormals(
    Object *deformed_obj,
    blender::Array<blender::float4> &restPositions,
    blender::Array<blender::float4> &restNormals)
{
  if (!deformed_obj) {
    return;
  }

  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *ob_eval = DEG_get_evaluated(depsgraph, deformed_obj);
  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);

  const int num_corners = mesh_eval->corners_num;

  auto corner_verts = mesh_eval->corner_verts();
  auto vert_positions = mesh_eval->vert_positions();
  auto corner_normals = mesh_eval->corner_normals();

  restPositions = blender::Array<blender::float4>(num_corners);
  blender::threading::parallel_for(
      blender::IndexRange(num_corners), 4096, [&](const blender::IndexRange range) {
        for (int i : range) {
          const int v = corner_verts[i];
          const blender::float3 &p = vert_positions[v];
          restPositions[i] = blender::float4(p.x, p.y, p.z, 1.0f);
        }
      });

  restNormals = blender::Array<blender::float4>(num_corners);
  blender::threading::parallel_for(
      blender::IndexRange(num_corners), 4096, [&](const blender::IndexRange range) {
        for (int i : range) {
          const blender::float3 n = corner_normals[i];
          restNormals[i] = blender::float4(n.x, n.y, n.z, 0.0f);
        }
      });

  if (!m_skinStatic) {
    m_skinStatic = new BGE_SkinStaticBuffers();
  }
  m_skinStatic->num_corners = num_corners;

  // Stockage CPU partagé
  m_skinStatic->refPositions = restPositions;
  m_skinStatic->refNormals = restNormals;

  // SSBOs rest_* dimensionnés à num_corners
  if (!m_skinStatic->ssbo_rest_pose) {
    m_skinStatic->ssbo_rest_pose = GPU_storagebuf_create(sizeof(float) * 4 * num_corners);
  }
  GPU_storagebuf_update(m_skinStatic->ssbo_rest_pose, m_skinStatic->refPositions.data());

  if (!m_skinStatic->ssbo_rest_normals) {
    m_skinStatic->ssbo_rest_normals = GPU_storagebuf_create(sizeof(float) * 4 * num_corners);
  }
  GPU_storagebuf_update(m_skinStatic->ssbo_rest_normals, m_skinStatic->refNormals.data());
}

// Only allowed for Poses with identical channels.
void BL_ArmatureObject::GameBlendPose(bPose *dst, bPose *src, float srcweight, short mode)
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
  m_previousArmature = nullptr;
  m_deformedObj = nullptr;
  m_useGPUDeform = false;
  m_deformedReplicaData = nullptr;
  ssbo_bone_pose_mat = nullptr;
  ssbo_premat = nullptr;
  ssbo_postmat = nullptr;
  m_modifiersListbackup = {};
  m_skinStatic = nullptr;
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

  /* Restore orig_mesh->is_using_skinning = 0,
   * to extract positions on float3 next time mesh will be reconstructed */
  if (m_deformedObj && !m_isReplica) {
    Mesh *orig_mesh = (Mesh *)m_deformedObj->data;
    orig_mesh->is_using_skinning = 0;
  }
  m_deformedObj = nullptr;

  if (m_skinStatic) {
    if (--m_skinStatic->ref_count == 0) {
      if (m_skinStatic->ssbo_in_idx)
        GPU_storagebuf_free(m_skinStatic->ssbo_in_idx);
      if (m_skinStatic->ssbo_in_wgt)
        GPU_storagebuf_free(m_skinStatic->ssbo_in_wgt);
      if (m_skinStatic->ssbo_rest_pose)
        GPU_storagebuf_free(m_skinStatic->ssbo_rest_pose);
      if (m_skinStatic->ssbo_rest_normals)
        GPU_storagebuf_free(m_skinStatic->ssbo_rest_normals);
      if (m_skinStatic->shader)
        GPU_shader_free(m_skinStatic->shader);

      m_skinStatic->in_indices.clear();
      m_skinStatic->in_indices.shrink_to_fit();
      m_skinStatic->in_weights.clear();
      m_skinStatic->in_weights.shrink_to_fit();
      m_skinStatic->refPositions = {};
      m_skinStatic->refNormals = {};

      delete m_skinStatic;
    }
    m_skinStatic = nullptr;
  }

  // Free per-instance/per-frame SSBOs only
  if (ssbo_bone_pose_mat) {  // guard remains but we no longer free static ssbos here
    GPU_storagebuf_free(ssbo_bone_pose_mat);
    GPU_storagebuf_free(ssbo_premat);
    GPU_storagebuf_free(ssbo_postmat);
    ssbo_bone_pose_mat = nullptr;
    ssbo_premat = nullptr;
    ssbo_postmat = nullptr;
  }
  if (m_deformedReplicaData) {
    bContext *C = KX_GetActiveEngine()->GetContext(); 
    BKE_id_delete(CTX_data_main(C), &m_deformedReplicaData->id);
    m_deformedReplicaData = nullptr;
  }
}

void BL_ArmatureObject::SetBlenderObject(Object *obj)
{
  KX_GameObject::SetBlenderObject(obj);
  m_objArma = obj;

  if (m_objArma) {
    memcpy(m_object_to_world, m_objArma->object_to_world().ptr(), sizeof(m_object_to_world));
    LoadChannels();
  }
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
  m_previousArmature = m_objArma;

  KX_GameObject::ProcessReplica();

  // Replicate each constraints.
  m_controlledConstraints = static_cast<EXP_ListValue<BL_ArmatureConstraint> *>(
      m_controlledConstraints->GetReplica());

  m_objArma = m_pBlenderObject;

  // Share the static skin buffers with the original if already created
  if (m_skinStatic) {
    m_skinStatic->ref_count++;
  }

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
    bContext *C = KX_GetActiveEngine()->GetContext();
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    BKE_pose_where_is(depsgraph, GetScene()->GetBlenderScene(), m_objArma);

    m_lastapplyframe = m_lastframe;
  }
}

void BL_ArmatureObject::InitSkinningBuffers()
{
  // Si déjà construit (par l'original), ne rien refaire pour les répliques.
  if (m_skinStatic && m_skinStatic->ssbo_in_idx && m_skinStatic->ssbo_in_wgt) {
    return;
  }

  if (m_skinStatic->in_indices.empty()) {
    bContext *C = KX_GetActiveEngine()->GetContext();
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    Object *deformed_eval = DEG_get_evaluated(depsgraph, m_deformedObj);
    Mesh *mesh = static_cast<Mesh *>(deformed_eval->data);

    const ListBase *defbase = mesh ? BKE_id_defgroup_list_get(&mesh->id) : nullptr;
    if (const ID *id_target = static_cast<const ID *>(m_deformedObj->data);
        BKE_id_supports_vertex_groups(id_target))
    {
      defbase = BKE_id_defgroup_list_get(id_target);
    }

    blender::Span<MDeformVert> dverts = mesh->deform_verts();
    auto corner_verts_span = mesh->corner_verts();
    const int num_corners = mesh->corners_num;

    // bones -> index
    std::map<std::string, int> bone_name_to_index;
    if (m_objArma && m_objArma->pose) {
      int idx = 0;
      for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
           pchan = pchan->next)
      {
        if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
          bone_name_to_index[pchan->name] = idx++;
        }
      }
    }

    // groups dans l'ordre du mesh
    std::vector<std::string> group_names;
    if (defbase) {
      for (bDeformGroup *dg = (bDeformGroup *)defbase->first; dg; dg = dg->next) {
        group_names.push_back(dg->name);
      }
    }

    // Remplir PAR CORNER
    m_skinStatic->in_indices.resize(num_corners * 4, 0);
    m_skinStatic->in_weights.resize(num_corners * 4, 0.0f);

    constexpr float kContribThreshold = 1e-4f;
    blender::threading::parallel_for(
        blender::IndexRange(num_corners), 4096, [&](const blender::IndexRange range) {
          for (int c : range) {
            const int v = corner_verts_span[c];
            if (dverts.is_empty()) {
              for (int j = 0; j < 4; ++j) {
                m_skinStatic->in_indices[c * 4 + j] = 0;
                m_skinStatic->in_weights[c * 4 + j] = 0.0f;
              }
              continue;
            }
            const MDeformVert &dvert = dverts[v];

            struct Inf {
              int bone_idx;
              float w;
            };
            std::vector<Inf> acc;
            acc.reserve(dvert.totweight);

            float total_raw = 0.0f;
            for (int j = 0; j < dvert.totweight; ++j) {
              const int def_nr = dvert.dw[j].def_nr;
              if (def_nr < 0 || (size_t)def_nr >= group_names.size())
                continue;
              auto it = bone_name_to_index.find(group_names[def_nr]);
              if (it == bone_name_to_index.end())
                continue;
              const float w = dvert.dw[j].weight;
              if (w <= 0.0f)
                continue;
              total_raw += w;
              const int bi = it->second;
              bool merged = false;
              for (auto &e : acc) {
                if (e.bone_idx == bi) {
                  e.w += w;
                  merged = true;
                  break;
                }
              }
              if (!merged)
                acc.push_back({bi, w});
            }

            if (total_raw <= kContribThreshold || acc.empty()) {
              for (int j = 0; j < 4; ++j) {
                m_skinStatic->in_indices[c * 4 + j] = 0;
                m_skinStatic->in_weights[c * 4 + j] = 0.0f;
              }
              continue;
            }

            std::sort(
                acc.begin(), acc.end(), [](const Inf &a, const Inf &b) { return a.w > b.w; });
            float sum = 0.0f;
            for (auto &e : acc)
              sum += e.w;
            const float inv = sum > 0.0f ? 1.0f / sum : 0.0f;

            for (int j = 0; j < 4; ++j) {
              if (j < (int)acc.size()) {
                m_skinStatic->in_indices[c * 4 + j] = acc[j].bone_idx;
                m_skinStatic->in_weights[c * 4 + j] = acc[j].w * inv;
              }
              else {
                m_skinStatic->in_indices[c * 4 + j] = 0;
                m_skinStatic->in_weights[c * 4 + j] = 0.0f;
              }
            }
          }
        });

    if (!m_skinStatic) {
      m_skinStatic = new BGE_SkinStaticBuffers();
    }
    // Vérification (les SSBO rest_* sont aussi au domaine corner).
    if (m_skinStatic->num_corners != 0) {
      BLI_assert(m_skinStatic->num_corners == num_corners);
    }
    m_skinStatic->num_corners = num_corners;

    if (!m_skinStatic->ssbo_in_idx) {
      m_skinStatic->ssbo_in_idx = GPU_storagebuf_create(sizeof(int) * num_corners * 4);
    }
    if (!m_skinStatic->ssbo_in_wgt) {
      m_skinStatic->ssbo_in_wgt = GPU_storagebuf_create(sizeof(float) * num_corners * 4);
    }
    GPU_storagebuf_update(m_skinStatic->ssbo_in_idx, m_skinStatic->in_indices.data());
    GPU_storagebuf_update(m_skinStatic->ssbo_in_wgt, m_skinStatic->in_weights.data());
  }
}

void BL_ArmatureObject::RemapParentChildren()
{
  /* Remapping parent/children */
  if (!m_deformedObj) {
    for (KX_GameObject *child : GetChildren()) {
      Object *child_ob = child->GetBlenderObject();
      if (!child_ob) {
        continue;
      }
      LISTBASE_FOREACH (ModifierData *, md, &child_ob->modifiers) {
        if (md->type == eModifierType_Armature) {
          ArmatureModifierData *amd = (ArmatureModifierData *)md;
          if (amd && amd->object == m_previousArmature) {
            amd->object = m_objArma;
          }
        }
      }
    }
  }
}

void BL_ArmatureObject::GetGpuDeformedObj()
{
  if (!m_deformedObj) {
    /* Get Armature modifier deformedObj */
    std::vector<KX_GameObject *> children = GetChildren();
    for (KX_GameObject *child : children) {
      bool is_bone_parented = child->GetBlenderObject()->partype == PARBONE;
      if (is_bone_parented || child->GetBlenderObject()->type != OB_MESH) {
        continue;
      }
      LISTBASE_FOREACH (ModifierData *, md, &child->GetBlenderObject()->modifiers) {
        if (md->type == eModifierType_Armature) {
          ArmatureModifierData *amd = (ArmatureModifierData *)md;
          if (amd && amd->object == this->GetBlenderObject()) {
            m_deformedObj = child->GetBlenderObject();
            m_useGPUDeform = (amd->upbge_deformflag & ARM_DEF_GPU) != 0 &&
                             !child->IsDupliInstance() && !m_is_dupli_instance;
          }
        }
      }
      if (m_deformedObj) {
        break;
      }
    }
  }
}

void BL_ArmatureObject::ApplyAction(bAction *action, const AnimationEvalContext &evalCtx)
{
  // Apply action to armature
  PointerRNA ptrrna = RNA_id_pointer_create(&m_objArma->id);
  const blender::animrig::slot_handle_t slot_handle = blender::animrig::first_slot_handle(*action);
  animsys_evaluate_action(&ptrrna, action, slot_handle, &evalCtx, false);
}

/* For gpu skinning, we delay many variables initialisation here to have "up to date" informations.
 * It is a bit tricky in case BL_ArmatureObject is a replica (needs to have right parent/child -> armature/deformed object,
 * a render cache for the deformed object....
 */
void BL_ArmatureObject::DoGpuSkinning() {
  if (!m_useGPUDeform) {
    return;
  }
  using namespace blender::gpu::shader;
  using namespace blender::draw;

  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

  KX_GameObject *kx_deformedObj = GetScene()->GetBlenderSceneConverter()->FindGameObject(
      m_deformedObj);

  if (kx_deformedObj->IsReplica()) {
    if (!m_deformedReplicaData) {
      Mesh *orig = (Mesh *)m_deformedObj->data;
      m_deformedReplicaData = (Mesh *)BKE_id_copy_ex(CTX_data_main(C), (ID *)orig, nullptr, 0);
      m_deformedObj->data = m_deformedReplicaData;
      DEG_id_tag_update(&m_deformedObj->id, ID_RECALC_GEOMETRY);
    }
  }

  Object *deformed_eval = DEG_get_evaluated(depsgraph, m_deformedObj);
  Mesh *mesh_eval = static_cast<Mesh *>(deformed_eval->data);
  Mesh *orig_mesh = (Mesh *)m_deformedObj->data;

  orig_mesh->is_using_skinning = 1;
  mesh_eval->is_running_skinning = 1;

  if (m_modifiersListbackup.empty()) {
    disable_armature_modifiers(m_deformedObj, m_modifiersListbackup);
    if (kx_deformedObj->IsReplica()) {
      kx_deformedObj->SetVisible(true, false);
    }
    return;
  }

  // Capture “rest” une seule fois (dans le conteneur partagé)
  if (!m_skinStatic || m_skinStatic->refPositions.is_empty()) {
    blender::Array<blender::float4> tmpPos, tmpNor;
    CaptureRestPositionsAndNormals(m_deformedObj, tmpPos, tmpNor);
  }

  MeshBatchCache *cache = nullptr;
  if (mesh_eval->runtime && mesh_eval->runtime->batch_cache) {
    cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  }

  blender::gpu::VertBuf *vbo_pos = nullptr;
  blender::gpu::VertBuf *vbo_nor = nullptr;

  if (cache && cache->final.buff.vbos.size() > 0) {
    auto pos_vbo_it = cache->final.buff.vbos.lookup_ptr(VBOType::Position);
    vbo_pos = pos_vbo_it ? pos_vbo_it->get() : nullptr;
    auto nor_vbo_it = cache->final.buff.vbos.lookup_ptr(VBOType::CornerNormal);
    vbo_nor = nor_vbo_it ? nor_vbo_it->get() : nullptr;
  }
  if (!vbo_pos || !vbo_nor) {
    /* GPU pipeline not ready */
    return;
  }

  InitSkinningBuffers();

  int num_corners = mesh_eval->corner_verts().size();

  // 3. Prepare bone matrices for GPU skinning
  // Build a list of deforming bone names and a mapping from name to index
  std::vector<std::string> bone_names;
  if (m_objArma && m_objArma->pose) {
    for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
         pchan = pchan->next)
    {
      // Only include bones marked as deforming
      if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
        std::string name(pchan->name);
        bone_names.push_back(name);
      }
    }
  }
  const int num_deform_bones = bone_names.size();

  // Allocate storage buffer for bone matrices if needed
  if (!ssbo_bone_pose_mat) {
    ssbo_bone_pose_mat = GPU_storagebuf_create(sizeof(float) * num_deform_bones * 16);
  }

  // Prepare the array of bone matrices (flattened 4x4 matrices)
  std::vector<float> bone_pose_matrices(num_deform_bones * 16, 0.0f);

  // Build a list of pose channels for deforming bones
  std::vector<bPoseChannel *> bone_channels;
  bone_channels.reserve(num_deform_bones);
  for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
       pchan = pchan->next)
  {
    if (pchan->bone->flag & BONE_NO_DEFORM) {
      continue;
    }
    bone_channels.push_back(pchan);
  }

  // For each deforming bone, compute the skinning matrix and store it
  for (int b = 0; b < num_deform_bones; ++b) {
    bPoseChannel *pchan = bone_channels[b];
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        bone_pose_matrices[b * 16 + row * 4 + col] = pchan->chan_mat[row][col];
      }
    }
  }
  // Upload bone matrices to the GPU buffer
  GPU_storagebuf_update(ssbo_bone_pose_mat, bone_pose_matrices.data());

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

  // 6. Compile skinning shader
  if (!m_skinStatic->shader) {
    ShaderCreateInfo info("BGE_Armature_Skinning_CPU_Logic");
    info.local_group_size(256, 1, 1);
    info.compute_source("draw_colormanagement_lib.glsl");
    info.compute_source("gpu_shader_math_vector_lib.glsl");
    info.storage_buf(0, Qualifier::write, "vec4", "positions[]");
    info.storage_buf(1, Qualifier::write, "uint", "normals[]");
    info.storage_buf(2, Qualifier::read, "ivec4", "in_idx[]");  // per corner
    info.storage_buf(3, Qualifier::read, "vec4", "in_wgt[]");   // per corner
    info.storage_buf(4, Qualifier::read, "mat4", "bone_pose_mat[]");
    info.storage_buf(5, Qualifier::read, "mat4", "premat[]");
    info.storage_buf(6, Qualifier::read, "vec4", "rest_positions[]");  // per corner
    info.storage_buf(7, Qualifier::read, "vec4", "rest_normals[]");    // per corner
    info.storage_buf(8, Qualifier::read, "mat4", "postmat[]");

    info.compute_source_generated = R"GLSL(
#ifndef CONTRIB_THRESHOLD
#define CONTRIB_THRESHOLD 1e-4
#endif
#ifndef NORMAL_EPSILON
#define NORMAL_EPSILON 1e-4
#endif

int convert_normalized_f32_to_i10(float x) {
  const int signed_int_10_max = 511;
  const int signed_int_10_min = -512;
  int qx = int(x * float(signed_int_10_max));
  return clamp(qx, signed_int_10_min, signed_int_10_max);
}

void main() {
  uint corner_index = gl_GlobalInvocationID.x;
  if (corner_index >= rest_positions.length()) {
    return;
  }

  // Positions: LBS + complément Blender
  vec4 rest_pos_object = premat[0] * rest_positions[corner_index];
  vec4 accumulated_pos = vec4(0.0);
  float total_weight = 0.0;
  for (int i = 0; i < 4; ++i) {
    int   bone_index = in_idx[corner_index][i];
    float weight     = in_wgt[corner_index][i];
    if (weight > 0.0) {
      accumulated_pos += (bone_pose_mat[bone_index] * rest_pos_object) * weight;
      total_weight    += weight;
    }
  }
  vec4 skinned_pos_object = (total_weight <= CONTRIB_THRESHOLD)
                                ? rest_pos_object
                                : (accumulated_pos + rest_pos_object * (1.0 - total_weight));
  positions[corner_index] = postmat[0] * skinned_pos_object;

  // Normales: blend de l’inverse-transposée par os (comme master)
  mat3 pre_normal   = transpose(inverse(mat3(premat[0])));
  mat3 post_normal  = transpose(inverse(mat3(postmat[0])));
  vec3 rest_n_object = pre_normal * rest_normals[corner_index].xyz;

  vec3 accumulated_n = vec3(0.0);
  float total_normal_weight = 0.0;
  for (int i = 0; i < 4; ++i) {
    int   bone_index = in_idx[corner_index][i];
    float weight     = in_wgt[corner_index][i];
    if (weight > NORMAL_EPSILON) {
      mat3 bone_normal_mat = transpose(inverse(mat3(bone_pose_mat[bone_index])));
      accumulated_n       += (bone_normal_mat * rest_n_object) * weight;
      total_normal_weight += weight;
    }
  }
  if (total_normal_weight < 1.0) {
    accumulated_n += rest_n_object * (1.0 - total_normal_weight);
  }
  vec3 final_n = safe_normalize(post_normal * accumulated_n);

  int nx = convert_normalized_f32_to_i10(final_n.x);
  int ny = convert_normalized_f32_to_i10(final_n.y);
  int nz = convert_normalized_f32_to_i10(final_n.z);
  normals[corner_index] = uint((nx & 0x3FF) | ((ny & 0x3FF) << 10) | ((nz & 0x3FF) << 20));
}
)GLSL";
    m_skinStatic->shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  }

  // 7. Dispatch compute shader
  GPU_shader_bind(m_skinStatic->shader);
  vbo_pos->bind_as_ssbo(0);
  vbo_nor->bind_as_ssbo(1);
  GPU_storagebuf_bind(m_skinStatic->ssbo_in_idx, 2);
  GPU_storagebuf_bind(m_skinStatic->ssbo_in_wgt, 3);
  GPU_storagebuf_bind(ssbo_bone_pose_mat, 4);
  GPU_storagebuf_bind(ssbo_premat, 5);
  GPU_storagebuf_bind(m_skinStatic->ssbo_rest_pose, 6);
  GPU_storagebuf_bind(m_skinStatic->ssbo_rest_normals, 7);
  GPU_storagebuf_bind(ssbo_postmat, 8);

  const int group_size = 256;
  const int num_groups = (num_corners + group_size - 1) / group_size;
  GPU_compute_dispatch(m_skinStatic->shader, num_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_storagebuf_unbind(m_skinStatic->ssbo_in_idx);
  GPU_storagebuf_unbind(m_skinStatic->ssbo_in_wgt);
  GPU_storagebuf_unbind(ssbo_bone_pose_mat);
  GPU_storagebuf_unbind(ssbo_premat);
  GPU_storagebuf_unbind(m_skinStatic->ssbo_rest_pose);
  GPU_storagebuf_unbind(m_skinStatic->ssbo_rest_normals);
  GPU_storagebuf_unbind(ssbo_postmat);
  GPU_shader_unbind();

  // Notify the dependency graph that the deformed mesh's transform has changed.
  // This updates the object_to_world matrices used by EEVEE without invalidating
  // render caches, ensuring correct shading after GPU skinning.
  DEG_id_tag_update(&m_deformedObj->id, ID_RECALC_TRANSFORM);
}

void BL_ArmatureObject::BlendInPose(bPose *blend_pose, float weight, short mode)
{
  GameBlendPose(m_objArma->pose, blend_pose, weight, mode);
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

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
  m_skinStatic = nullptr;
  m_ssbo_bone_pose_mat = nullptr;
  m_ssbo_premat = nullptr;
  m_ssbo_postmat = nullptr;
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

  /* Restore orig_mesh->is_using_skinning = 0,
   * to extract positions on float3 next time mesh will be reconstructed */
  if (m_deformedObj && !m_isReplica) {
    Mesh *orig_mesh = (Mesh *)m_deformedObj->data;
    orig_mesh->is_using_skinning = 0;
  }
  m_deformedObj = nullptr;

  if (m_skinStatic) {
    if (--m_skinStatic->ref_count == 0) {
      if (m_skinStatic->shader) {
        GPU_shader_free(m_skinStatic->shader);
      }
      if (m_skinStatic->ssbo_in_idx) {
        GPU_storagebuf_free(m_skinStatic->ssbo_in_idx);
        GPU_storagebuf_free(m_skinStatic->ssbo_in_wgt);
        GPU_storagebuf_free(m_skinStatic->ssbo_topology);
        GPU_storagebuf_free(m_skinStatic->ssbo_rest_positions);
      }
      delete m_skinStatic;
    }
    m_skinStatic = nullptr;
  }

  if (m_ssbo_bone_pose_mat) {
    GPU_storagebuf_free(m_ssbo_bone_pose_mat);
    GPU_storagebuf_free(m_ssbo_premat);
    GPU_storagebuf_free(m_ssbo_postmat);
    m_ssbo_bone_pose_mat = nullptr;
    m_ssbo_premat = nullptr;
    m_ssbo_postmat = nullptr;
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

void BL_ArmatureObject::InitStaticSkinningBuffers()
{
  if (!m_skinStatic) {
    m_skinStatic = new BGE_SkinStaticBuffers();
  }
  if (m_skinStatic->in_indices.empty()) {
    bContext *C = KX_GetActiveEngine()->GetContext();
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    Object *deformed_eval = DEG_get_evaluated(depsgraph, m_deformedObj);
    Mesh *mesh = static_cast<Mesh *>(deformed_eval->data);

    const ListBase *defbase = nullptr;
    if (mesh) {
      defbase = BKE_id_defgroup_list_get(&mesh->id);
    }
    const ID *id_target = static_cast<const ID *>(m_deformedObj->data);
    if (BKE_id_supports_vertex_groups(id_target)) {
      defbase = BKE_id_defgroup_list_get(id_target);
    }

    blender::Span<MDeformVert> dverts = mesh->deform_verts();
    const auto corner_verts = mesh->corner_verts();
    const int num_corners = mesh->corners_num;
    const int verts_num = mesh->verts_num;

    // 1) Build the ordered list of deforming bones and a name->index map.
    std::vector<std::string> bone_names;
    std::map<std::string, int> bone_name_to_index;
    if (m_objArma && m_objArma->pose) {
      int idx = 0;
      for (bPoseChannel *pchan = (bPoseChannel *)m_objArma->pose->chanbase.first; pchan;
           pchan = pchan->next)
      {
        if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
          std::string name(pchan->name);
          bone_names.push_back(name);
          bone_name_to_index[name] = idx++;
        }
      }
    }

    // 2) Get the vertex group names in mesh order.
    std::vector<std::string> group_names;
    if (defbase) {
      for (bDeformGroup *dg = (bDeformGroup *)defbase->first; dg; dg = dg->next) {
        group_names.push_back(dg->name);
      }
    }

    // 3) Fill index and weight buffers (max 4 influences per corner) in parallel.
    m_skinStatic->in_indices.resize(num_corners * 4, 0);
    m_skinStatic->in_weights.resize(num_corners * 4, 0.0f);
    constexpr float kContribThreshold = 1e-4f;

    blender::threading::parallel_for(
        blender::IndexRange(num_corners), 4096, [&](const blender::IndexRange range) {
          for (int v : range) {
            const int vert_idx = corner_verts[v];
            const MDeformVert &dvert = dverts[vert_idx];

            struct Influence {
              int bone_idx;
              float weight;
            };
            std::map<int, float> bone_weight_map;
            for (int j = 0; j < dvert.totweight; ++j) {
              const int def_nr = dvert.dw[j].def_nr;
              if (def_nr >= 0 && def_nr < (int)group_names.size()) {
                const std::string &group_name = group_names[def_nr];
                auto it = bone_name_to_index.find(group_name);
                if (it != bone_name_to_index.end()) {
                  bone_weight_map[it->second] += dvert.dw[j].weight;
                }
              }
            }

            std::vector<Influence> influences;
            influences.reserve(bone_weight_map.size());
            float total_raw = 0.0f;
            for (const auto &kv : bone_weight_map) {
              influences.push_back({kv.first, kv.second});
              total_raw += kv.second;
            }

            if (total_raw <= kContribThreshold || influences.empty()) {
              for (int j = 0; j < 4; ++j) {
                m_skinStatic->in_indices[v * 4 + j] = 0;
                m_skinStatic->in_weights[v * 4 + j] = 0.0f;
              }
              continue;
            }

            std::sort(influences.begin(),
                      influences.end(),
                      [](const Influence &a, const Influence &b) { return a.weight > b.weight; });

            float total = 0.0f;
            for (const auto &inf : influences) {
              total += inf.weight;
            }
            if (total > 0.0f) {
              for (auto &inf : influences) {
                inf.weight /= total;
              }
            }

            for (int j = 0; j < 4; ++j) {
              if (j < (int)influences.size()) {
                m_skinStatic->in_indices[v * 4 + j] = influences[j].bone_idx;
                m_skinStatic->in_weights[v * 4 + j] = influences[j].weight;
              }
              else {
                m_skinStatic->in_indices[v * 4 + j] = 0;
                m_skinStatic->in_weights[v * 4 + j] = 0.0f;
              }
            }
          }
        });

    // 4) Upload SSBOs for influences.
    if (!m_skinStatic->ssbo_in_idx) {
      m_skinStatic->ssbo_in_idx = GPU_storagebuf_create(sizeof(int) * num_corners * 4);
    }
    if (!m_skinStatic->ssbo_in_wgt) {
      m_skinStatic->ssbo_in_wgt = GPU_storagebuf_create(sizeof(float) * num_corners * 4);
    }
    GPU_storagebuf_update(m_skinStatic->ssbo_in_idx, m_skinStatic->in_indices.data());
    GPU_storagebuf_update(m_skinStatic->ssbo_in_wgt, m_skinStatic->in_weights.data());

    // 5) Pack topology into a single buffer.
    const auto faces = mesh->faces();
    const int faces_num = faces.size();

    // face_offsets
    std::vector<int> face_sizes(faces_num, 0);
    blender::threading::parallel_for(
        blender::IndexRange(faces_num), 4096, [&](const blender::IndexRange range) {
          for (int f : range) {
            face_sizes[f] = (int)faces[f].size();
          }
        });

    std::vector<int> face_offsets(faces_num + 1, 0);
    {
      int ofs = 0;
      for (int f = 0; f < faces_num; ++f) {
        face_offsets[f] = ofs;
        ofs += face_sizes[f];
      }
      face_offsets[faces_num] = ofs;
    }

    // corner_to_face
    std::vector<int> corner_to_face(num_corners, 0);
    blender::threading::parallel_for(
        blender::IndexRange(faces_num), 4096, [&](const blender::IndexRange range) {
          for (int f : range) {
            const int beg = face_offsets[f];
            const int cnt = face_sizes[f];
            for (int i = 0; i < cnt; ++i) {
              corner_to_face[beg + i] = f;
            }
          }
        });

    // corner_verts
    std::vector<int> corner_verts_vec(corner_verts.begin(), corner_verts.end());

    // vert_to_face_offsets and vert_to_face (CSR)
    const blender::OffsetIndices<int> v2f_off = mesh->vert_to_face_map_offsets();
    const blender::GroupedSpan<int> v2f = mesh->vert_to_face_map();

    std::vector<int> v2f_offsets(verts_num + 1, 0);
    for (int v = 0; v < verts_num + 1; ++v) {
      v2f_offsets[v] = v2f_off[v].start();
    }
    const int total_v2f = v2f_offsets[verts_num];

    std::vector<int> v2f_indices;
    v2f_indices.resize(std::max(total_v2f, 0));
    blender::threading::parallel_for(
        blender::IndexRange(verts_num), 4096, [&](const blender::IndexRange range) {
          for (int v : range) {
            const blender::Span<int> faces_v = v2f[v];
            const int dst = v2f_offsets[v];
            if (!faces_v.is_empty()) {
              std::copy(faces_v.begin(), faces_v.end(), v2f_indices.begin() + dst);
            }
          }
        });

    // Offsets for each sub-array in the packed buffer.
    m_skinStatic->face_offsets_offset = 0;
    m_skinStatic->corner_to_face_offset = m_skinStatic->face_offsets_offset +
                                          int(face_offsets.size());
    m_skinStatic->corner_verts_offset = m_skinStatic->corner_to_face_offset +
                                        int(corner_to_face.size());
    m_skinStatic->vert_to_face_offsets_offset = m_skinStatic->corner_verts_offset +
                                                int(corner_verts_vec.size());
    m_skinStatic->vert_to_face_offset = m_skinStatic->vert_to_face_offsets_offset +
                                        int(v2f_offsets.size());
    int topo_total_size = m_skinStatic->vert_to_face_offset + int(v2f_indices.size());

    // Final packing.
    std::vector<int> topo;
    topo.reserve(topo_total_size);
    topo.insert(topo.end(), face_offsets.begin(), face_offsets.end());
    topo.insert(topo.end(), corner_to_face.begin(), corner_to_face.end());
    topo.insert(topo.end(), corner_verts_vec.begin(), corner_verts_vec.end());
    topo.insert(topo.end(), v2f_offsets.begin(), v2f_offsets.end());
    topo.insert(topo.end(), v2f_indices.begin(), v2f_indices.end());

    // Create and upload the unique SSBO.
    if (!m_skinStatic->ssbo_topology) {
      m_skinStatic->ssbo_topology = GPU_storagebuf_create(sizeof(int) * topo_total_size);
    }
    GPU_storagebuf_update(m_skinStatic->ssbo_topology, topo.data());

    blender::Span<blender::float3> vert_positions = mesh->vert_positions();
    blender::Array<blender::float4> rest_positions = blender::Array<blender::float4>(num_corners);
    blender::threading::parallel_for(
        blender::IndexRange(num_corners), 4096, [&](const blender::IndexRange range) {
          for (int i : range) {
            int vert_idx = corner_verts[i];
            const blender::float3 &pos = vert_positions[vert_idx];
            rest_positions[i] = blender::float4(pos.x, pos.y, pos.z, 1.0f);
          }
        });

    if (!m_skinStatic->ssbo_rest_positions) {
      m_skinStatic->ssbo_rest_positions = GPU_storagebuf_create(sizeof(blender::float4) *
                                                                num_corners);
    }
    GPU_storagebuf_update(m_skinStatic->ssbo_rest_positions, rest_positions.data());
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
void BL_ArmatureObject::DoGpuSkinning()
{
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
    /* We need to replicate Mesh for deformation on GPU in some files and not in others...
     * It ensures data to be deformed will be unique */
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

  /* Set this variable to extract vbo_pos with float4 */
  orig_mesh->is_using_skinning = 1;
  /* Set this variable to indicate that the action is currently played.
   * Will be reset just after render.
   * Place this flag on runtime/evaluated mesh (the one used for rendering) */
  mesh_eval->is_running_skinning = 1;

  if (m_modifiersListbackup.empty()) {
    disable_armature_modifiers(m_deformedObj, m_modifiersListbackup);
    /* 1. Wait the next frame that we have vbos_pos on float4 in render cache.
     * (Disable_armature_modifiers tags m_deformedObj for geometry recalc, with
     * the newly assigned mesh, with float4).
     * 2. Also Restore visibility for the next render frame (previously disabled
     * in ReplicateBlenderObject to avoid seeing the mesh with wrong pose) if m_deformedObj
     * is a replica */
    if (kx_deformedObj->IsReplica()) {
      kx_deformedObj->SetVisible(true, false);
    }
    return;
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

  // Prepare skinning Static resources (shared between replicas)
  InitStaticSkinningBuffers();

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
  if (!m_ssbo_bone_pose_mat) {
    m_ssbo_bone_pose_mat = GPU_storagebuf_create(sizeof(float) * num_deform_bones * 16);
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
  GPU_storagebuf_update(m_ssbo_bone_pose_mat, bone_pose_matrices.data());

  // 4. Prepare transform matrices
  float premat[4][4], postmat[4][4], obinv[4][4];
  copy_m4_m4(premat, m_deformedObj->object_to_world().ptr());
  invert_m4_m4(obinv, m_deformedObj->object_to_world().ptr());
  mul_m4_m4m4(postmat, obinv, m_objArma->object_to_world().ptr());
  invert_m4_m4(premat, postmat);

  if (!m_ssbo_premat) {
    m_ssbo_premat = GPU_storagebuf_create(sizeof(float) * 16);
  }
  GPU_storagebuf_update(m_ssbo_premat, &premat[0][0]);
  if (!m_ssbo_postmat) {
    m_ssbo_postmat = GPU_storagebuf_create(sizeof(float) * 16);
  }
  GPU_storagebuf_update(m_ssbo_postmat, &postmat[0][0]);

  // 5. Compile skinning shader
  if (!m_skinStatic->shader) {
    ShaderCreateInfo info("BGE_Armature_Skinning_CPU_Logic");
    info.local_group_size(256, 1, 1);
    info.compute_source("draw_colormanagement_lib.glsl");
    info.storage_buf(0, Qualifier::write, "vec4", "positions[]");
    info.storage_buf(1, Qualifier::write, "uint", "normals[]");
    info.storage_buf(2, Qualifier::read, "ivec4", "in_idx[]");
    info.storage_buf(3, Qualifier::read, "vec4", "in_wgt[]");
    info.storage_buf(4, Qualifier::read, "mat4", "bone_pose_mat[]");
    info.storage_buf(5, Qualifier::read, "mat4", "premat[]");
    info.storage_buf(6, Qualifier::read, "mat4", "postmat[]");
    info.storage_buf(7, Qualifier::read, "int", "topo[]");
    info.storage_buf(8, Qualifier::read, "vec4", "rest_positions[]");
    info.push_constant(Type::int_t, "face_offsets_offset");
    info.push_constant(Type::int_t, "corner_to_face_offset");
    info.push_constant(Type::int_t, "corner_verts_offset");
    info.push_constant(Type::int_t, "vert_to_face_offsets_offset");
    info.push_constant(Type::int_t, "vert_to_face_offset");
    info.push_constant(Type::int_t, "normals_domain");

    info.compute_source_generated = R"GLSL(
#ifndef CONTRIB_THRESHOLD
#define CONTRIB_THRESHOLD 1e-4
#endif
#ifndef NORMAL_EPSILON
#define NORMAL_EPSILON 1e-4
#endif

// Utility accessors for the packed buffer.
int face_offsets(int i) { return topo[face_offsets_offset + i]; }
int corner_to_face(int i) { return topo[corner_to_face_offset + i]; }
int corner_verts(int i) { return topo[corner_verts_offset + i]; }
int vert_to_face_offsets(int i) { return topo[vert_to_face_offsets_offset + i]; }
int vert_to_face(int i) { return topo[vert_to_face_offset + i]; }

// 10_10_10_2 packing utility (W is ignored).
int pack_i10_trunc(float x)
{
  const int signed_int_10_max = 511;
  const int signed_int_10_min = -512;
  float s = x * float(signed_int_10_max);
  int q = int(s); // truncate towards zero
  q = clamp(q, signed_int_10_min, signed_int_10_max);
  return q & 0x3FF;
}

uint pack_norm(vec3 n)
{
  int nx = pack_i10_trunc(n.x);
  int ny = pack_i10_trunc(n.y);
  int nz = pack_i10_trunc(n.z);
  // W=0 to match the C++ PackedNormal.
  return uint(nx) | (uint(ny) << 10) | (uint(nz) << 20);
}

// Reskin a corner in armature-object space for local reuse.
vec4 skin_corner_pos_object(int corner) {
  // Replace rest_positions[corner] with positions[corner] if you use the positions VBO as input.
  vec4 rest_pos_object = premat[0] * rest_positions[corner];
  vec4 acc = vec4(0.0);
  float tw = 0.0;
  for (int i = 0; i < 4; ++i) {
    int   b = in_idx[corner][i];
    float w = in_wgt[corner][i];
    if (w > 0.0) {
      acc += (bone_pose_mat[b] * rest_pos_object) * w;
      tw  += w;
    }
  }
  return (tw <= CONTRIB_THRESHOLD) ? rest_pos_object : (acc + rest_pos_object * (1.0 - tw));
}

// Face normal (Newell) calculated on skinned positions (final space).
vec3 newell_face_normal_skinned(int f){
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  vec3 n = vec3(0.0);
  vec3 v_prev = (postmat[0] * skin_corner_pos_object(end - 1)).xyz;
  for (int i = beg; i < end; ++i) {
    vec3 v_curr = (postmat[0] * skin_corner_pos_object(i)).xyz;
    n += cross(v_prev, v_curr);
    v_prev = v_curr;
  }
  return normalize(n);
}

vec3 smooth_point_normal_skinned(int corner) {
  int v = corner_verts(corner);
  int beg = vert_to_face_offsets(v);
  int end = vert_to_face_offsets(v + 1);
  vec3 n = vec3(0.0);
  for (int i = beg; i < end; ++i) {
    int f = vert_to_face(i);
    n += newell_face_normal_skinned(f);
  }
  return normalize(n);
}

void main() {
  uint c = gl_GlobalInvocationID.x;
  if (c >= positions.length()) {
    return;
  }

  // 1) Skinned position (write to VBO).
  vec4 p_obj = skin_corner_pos_object(int(c));
  vec3 p     = (postmat[0] * p_obj).xyz;
  positions[c] = vec4(p, 1.0);

  // 2) Normal calculation.
  vec3 n;
  if (normals_domain == 1) {
    // Face: flat normal for all corners of the face.
    int f = corner_to_face(int(c));
    n = newell_face_normal_skinned(f);
  }
  else {
    // Point: average all incident faces' normals (true smooth, Blender style).
    n = smooth_point_normal_skinned(int(c));
  }

  normals[c] = pack_norm(n);
}
)GLSL";
    m_skinStatic->shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  }

  // 6. Dispatch compute shader
  GPU_shader_bind(m_skinStatic->shader);
  vbo_pos->bind_as_ssbo(0);
  vbo_nor->bind_as_ssbo(1);
  GPU_storagebuf_bind(m_skinStatic->ssbo_in_idx, 2);
  GPU_storagebuf_bind(m_skinStatic->ssbo_in_wgt, 3);
  GPU_storagebuf_bind(m_ssbo_bone_pose_mat, 4);
  GPU_storagebuf_bind(m_ssbo_premat, 5);
  GPU_storagebuf_bind(m_ssbo_postmat, 6);
  GPU_storagebuf_bind(m_skinStatic->ssbo_topology, 7);
  GPU_storagebuf_bind(m_skinStatic->ssbo_rest_positions, 8);
  GPU_shader_uniform_1i(
      m_skinStatic->shader, "face_offsets_offset", m_skinStatic->face_offsets_offset);
  GPU_shader_uniform_1i(
      m_skinStatic->shader, "corner_to_face_offset", m_skinStatic->corner_to_face_offset);
  GPU_shader_uniform_1i(
      m_skinStatic->shader, "corner_verts_offset", m_skinStatic->corner_verts_offset);
  GPU_shader_uniform_1i(m_skinStatic->shader,
                        "vert_to_face_offsets_offset",
                        m_skinStatic->vert_to_face_offsets_offset);
  GPU_shader_uniform_1i(
      m_skinStatic->shader, "vert_to_face_offset", m_skinStatic->vert_to_face_offset);

  int domain = mesh_eval->normals_domain() == blender::bke::MeshNormalDomain::Face ? 1 : 0;
  GPU_shader_uniform_1i(m_skinStatic->shader, "normals_domain", domain);

  const int group_size = 256;
  const int num_groups = (num_corners + group_size - 1) / group_size;
  GPU_compute_dispatch(m_skinStatic->shader, num_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_storagebuf_unbind(m_skinStatic->ssbo_in_idx);
  GPU_storagebuf_unbind(m_skinStatic->ssbo_in_wgt);
  GPU_storagebuf_unbind(m_ssbo_bone_pose_mat);
  GPU_storagebuf_unbind(m_ssbo_premat);
  GPU_storagebuf_unbind(m_ssbo_postmat);
  GPU_storagebuf_unbind(m_skinStatic->ssbo_topology);
  GPU_storagebuf_unbind(m_skinStatic->ssbo_rest_positions);
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

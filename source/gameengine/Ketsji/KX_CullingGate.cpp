/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file gameengine/Ketsji/KX_CullingGate.cpp
 *  \ingroup ketsji
 */

#include "KX_CullingGate.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BKE_context.hh"
#include "BKE_light.h"
#include "BKE_object.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_action_types.h"
#include "DNA_armature_types.h"
#include "DNA_light_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BL_ArmatureObject.h"
#include "KX_Camera.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "KX_LodManager.h"

using namespace blender;

/* Objects are classified in blocks small enough for their SoA slice, the two
 * accumulators and the masks to stay in L1 (256 * 6 floats = 6 KiB). */
static constexpr int BLOCK = 256;
static constexpr float BIG = FLT_MAX;
/* Bias added to the caster relevance of objects that do not cast shadows: it
 * drives the value below zero without producing NaN (the relevance is always
 * a finite distance). */
static constexpr float NOT_A_CASTER = -FLT_MAX;
/* An AABB of a vertex set rotated rigidly around its bone box stays within
 * sqrt(3) times its per axis distance to that box. */
static constexpr float SQRT3 = 1.7320508f;

static void world_matrix(KX_GameObject *gameobj, float r_mat[4][4])
{
  gameobj->NodeGetWorldTransform().getValue(&r_mat[0][0]);
}

static KX_CullingGate::Table table_of(const KX_GameObject *gameobj)
{
  return KX_CullingGate::Table(gameobj->GetCullingTable());
}

/* Must match EEVEE Light::attenuation_radius_get() and the caller in
 * Light::shape_parameters_set(): the gate and the renderer have to agree on
 * where a light stops contributing, or lights would pop at the border. */
static float light_influence_radius(const Light &la, float threshold)
{
  if (la.mode & LA_CUSTOM_ATTENUATION) {
    return la.att_dist;
  }
  const float3 color = BKE_light_color(la);
  const float max_power = std::max({color.x, color.y, color.z}) *
                          fabsf(BKE_light_power(la) / 100.0f);
  const float power = std::max({la.diff_fac, la.spec_fac, la.volume_fac}) * max_power;
  /* Inverse square law: power / d^2 reaches the threshold at d. */
  return sqrtf(power / threshold);
}

KX_CullingGate::KX_CullingGate() = default;

KX_CullingGate::~KX_CullingGate()
{
  FreeGPUResources();
}

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void KX_CullingGate::Register(KX_GameObject *gameobj,
                              Depsgraph *depsgraph,
                              const Scene * /*scene*/)
{
  Object *ob_orig = gameobj->GetBlenderObject();
  if (!ob_orig || gameobj->IsDupliInstance() ||
      (ob_orig->gameflag & (OB_OVERLAY_COLLECTION | OB_DUPLI_UPBGE)) ||
      (ob_orig->transflag & OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY))
  {
    /* Overlay objects live in their own pass, duplis share their Blender
     * object and Blender driven objects get their transform from Blender. */
    gameobj->SetCullingSlot(SLOT_NEVER, TABLE_OBJECT);
    return;
  }

  switch (gameobj->GetGameObjectType()) {
    case SCA_IObject::OBJ_CAMERA:
      gameobj->SetCullingSlot(SLOT_NEVER, TABLE_OBJECT);
      return;
    case SCA_IObject::OBJ_ARMATURE:
      /* Armatures draw nothing. They get a record only when a skinned mesh
       * registers (RegisterArmature), which overrides this. */
      gameobj->SetCullingSlot(SLOT_NEVER, TABLE_OBJECT);
      return;
    default:
      break;
  }

  Object *ob_eval = DEG_get_evaluated(depsgraph, ob_orig);
  if (!ob_eval || ob_eval == ob_orig) {
    /* Not in the depsgraph yet, retry after the next update. */
    return;
  }

  if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_LIGHT) {
    RegisterLight(gameobj, ob_orig, ob_eval);
    return;
  }
  RegisterObject(gameobj, ob_orig, ob_eval);
}

/* Margin between the rest bone box and the rest mesh box, in armature space.
 * Returns < 0 when no bone bound can be built. */
static float skin_margin(BL_ArmatureObject *arm, KX_GameObject *mesh_go, const Object *ob_mesh)
{
  const Object *ob_arma = arm->GetArmatureObject();
  if (!ob_arma || !ob_arma->pose) {
    return -1.0f;
  }

  float bmin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
  float bmax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  for (const bPoseChannel &pchan : ob_arma->pose->chanbase) {
    if (!pchan.bone) {
      continue;
    }
    for (int k = 0; k < 3; k++) {
      bmin[k] = std::min({bmin[k], pchan.bone->arm_head[k], pchan.bone->arm_tail[k]});
      bmax[k] = std::max({bmax[k], pchan.bone->arm_head[k], pchan.bone->arm_tail[k]});
    }
  }
  if (bmin[0] > bmax[0]) {
    return -1.0f;
  }

  /* Undeformed mesh data: the rest shape the armature modifier starts from. */
  const std::optional<Bounds<float3>> rest = BKE_object_boundbox_get(ob_mesh);
  if (!rest) {
    return -1.0f;
  }

  /* Mesh space to armature space, the space the armature modifier deforms in. */
  float arm_mat[4][4], arm_imat[4][4], mesh_mat[4][4], rel[4][4];
  world_matrix(arm, arm_mat);
  world_matrix(mesh_go, mesh_mat);
  invert_m4_m4(arm_imat, arm_mat);
  mul_m4_m4m4(rel, arm_imat, mesh_mat);

  float margin = 0.0f;
  const float lc[3] = {(rest->min.x + rest->max.x) * 0.5f,
                       (rest->min.y + rest->max.y) * 0.5f,
                       (rest->min.z + rest->max.z) * 0.5f};
  const float le[3] = {(rest->max.x - rest->min.x) * 0.5f,
                       (rest->max.y - rest->min.y) * 0.5f,
                       (rest->max.z - rest->min.z) * 0.5f};
  for (int k = 0; k < 3; k++) {
    const float c = rel[0][k] * lc[0] + rel[1][k] * lc[1] + rel[2][k] * lc[2] + rel[3][k];
    const float e = fabsf(rel[0][k]) * le[0] + fabsf(rel[1][k]) * le[1] +
                    fabsf(rel[2][k]) * le[2];
    margin = std::max({margin, bmin[k] - (c - e), (c + e) - bmax[k]});
  }
  return margin * SQRT3;
}

void KX_CullingGate::RegisterObject(KX_GameObject *gameobj, Object *ob_orig, Object *ob_eval)
{
  switch (ob_eval->type) {
    case OB_MESH:
    case OB_CURVES_LEGACY:
    case OB_SURF:
    case OB_FONT:
    case OB_CURVES:
    case OB_POINTCLOUD:
    case OB_VOLUME:
      break;
    default:
      /* Empties draw nothing, metaballs are evaluated as a family. */
      gameobj->SetCullingSlot(SLOT_NEVER, TABLE_OBJECT);
      return;
  }

  const int visibility = BKE_object_visibility(ob_eval, DAG_EVAL_VIEWPORT);
  if (visibility == 0 || (visibility & (OB_VISIBLE_INSTANCES | OB_VISIBLE_PARTICLES))) {
    /* Instances and particles are drawn through this object: its own box
     * does not bound them. */
    gameobj->SetCullingSlot(SLOT_NEVER, TABLE_OBJECT);
    return;
  }

  const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_eval_cached_get(ob_eval);
  if (!bounds) {
    gameobj->SetCullingSlot(SLOT_NEVER, TABLE_OBJECT);
    return;
  }

  /* Skinned by the parent game armature: bound from the posed bones. */
  int arm_index = -1;
  float margin = 0.0f;
  KX_GameObject *parent = gameobj->GetParent();
  if (ob_orig->type == OB_MESH && parent &&
      parent->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE)
  {
    BL_ArmatureObject *arm = static_cast<BL_ArmatureObject *>(parent);
    bool skinned = false;
    for (const ModifierData &md : ob_orig->modifiers) {
      if (md.type == eModifierType_Armature &&
          reinterpret_cast<const ArmatureModifierData &>(md).object == arm->GetArmatureObject())
      {
        skinned = true;
        break;
      }
    }
    if (skinned) {
      margin = skin_margin(arm, gameobj, ob_orig);
      if (margin >= 0.0f) {
        arm_index = RegisterArmature(arm);
      }
    }
  }

  bool dynamic = ob_eval->type != OB_MESH || ob_orig->modifiers.first != nullptr ||
                 gameobj->GetLodManager() != nullptr || (ob_orig->gameflag & OB_SOFT_BODY);
  if (ob_orig->type == OB_MESH && id_cast<const Mesh *>(ob_orig->data)->key) {
    dynamic = true;
  }

  const int slot = int(m_obOwner.size());
  m_obOwner.push_back(gameobj);
  m_obOrig.push_back(ob_orig);
  m_obEval.push_back(ob_eval);
  m_cx.push_back(0.0f);
  m_cy.push_back(0.0f);
  m_cz.push_back(0.0f);
  m_ex.push_back(0.0f);
  m_ey.push_back(0.0f);
  m_ez.push_back(0.0f);
  m_casterBias.push_back((ob_orig->visibility_flag & OB_HIDE_SHADOW) ? NOT_A_CASTER : 0.0f);
  m_localBox.push_back((bounds->min.x + bounds->max.x) * 0.5f);
  m_localBox.push_back((bounds->min.y + bounds->max.y) * 0.5f);
  m_localBox.push_back((bounds->min.z + bounds->max.z) * 0.5f);
  m_localBox.push_back((bounds->max.x - bounds->min.x) * 0.5f);
  m_localBox.push_back((bounds->max.y - bounds->min.y) * 0.5f);
  m_localBox.push_back((bounds->max.z - bounds->min.z) * 0.5f);
  m_obFlags.push_back(dynamic ? OB_FLAG_DYNAMIC_BOUNDS : 0);
  m_obUserMask.push_back(gameobj->GetVisible() ? 0xFF : 0x00);
  m_obMoved.push_back(0);
  m_obArm.push_back(arm_index);
  m_obMargin.push_back(margin);
  m_obPending.push_back(0);
  m_obTagFrame.push_back(0);
  m_obOccStreak.push_back(0);
  /* Evaluated and drawn this pass by the regular path: start relevant. */
  m_state[TABLE_OBJECT].push_back((STATE_DRAW | STATE_EVAL) & m_obUserMask.back());

  if (arm_index >= 0) {
    m_arms[arm_index].skinned_count++;
    UpdateSkinnedBox(slot);
  }
  else {
    float mat[4][4];
    world_matrix(gameobj, mat);
    UpdateWorldBox(slot, mat);
  }

  m_idToRef.add_overwrite(&ob_orig->id, Ref{TABLE_OBJECT, slot});
  gameobj->SetCullingSlot(slot, TABLE_OBJECT);
}

void KX_CullingGate::RegisterLight(KX_GameObject *gameobj, Object *ob_orig, Object *ob_eval)
{
  if (ob_orig->type != OB_LAMP || !ob_orig->data) {
    gameobj->SetCullingSlot(SLOT_NEVER, TABLE_OBJECT);
    return;
  }
  const int slot = int(m_lights.size());
  LightRec rec = {};
  rec.owner = gameobj;
  rec.ob_orig = ob_orig;
  rec.ob_eval = ob_eval;
  /* Suns and unknown lights stay relevant until the first refresh. */
  rec.is_sun = 1;
  rec.user_mask = gameobj->GetVisible() ? 0xFF : 0x00;
  m_lights.push_back(rec);
  m_state[TABLE_LIGHT].push_back((STATE_DRAW | STATE_EVAL) & rec.user_mask);
  m_idToRef.add_overwrite(&ob_orig->id, Ref{TABLE_LIGHT, slot});
  gameobj->SetCullingSlot(slot, TABLE_LIGHT);
}

int KX_CullingGate::RegisterArmature(BL_ArmatureObject *arm)
{
  if (arm->GetCullingSlot() >= 0 && arm->GetCullingTable() == TABLE_ARMATURE) {
    return arm->GetCullingSlot();
  }
  const int slot = int(m_arms.size());
  ArmRec rec = {};
  rec.owner = arm;
  rec.bmin[0] = rec.bmin[1] = rec.bmin[2] = FLT_MAX;
  rec.bmax[0] = rec.bmax[1] = rec.bmax[2] = -FLT_MAX;
  /* Impossible frame value: forces the first pose box build. */
  rec.pose_frame = -DBL_MAX;
  m_arms.push_back(rec);
  m_state[TABLE_ARMATURE].push_back(STATE_EVAL);
  if (Object *ob = arm->GetBlenderObject()) {
    m_idToRef.add_overwrite(&ob->id, Ref{TABLE_ARMATURE, slot});
  }
  arm->SetCullingSlot(slot, TABLE_ARMATURE);
  RefreshArmatures();
  return slot;
}

/* Writes the cached evaluated object back to the visibility of its original. */
static void restore_eval_visibility(Object *ob_orig, Object *ob_eval)
{
  ob_eval->visibility_flag = (ob_eval->visibility_flag & ~OB_HIDE_VIEWPORT) |
                             (ob_orig->visibility_flag & OB_HIDE_VIEWPORT);
}

void KX_CullingGate::Unregister(KX_GameObject *gameobj)
{
  const int slot = gameobj->GetCullingSlot();
  const Table table = table_of(gameobj);
  gameobj->SetCullingSlot(SLOT_UNREGISTERED, TABLE_OBJECT);
  if (slot < 0) {
    return;
  }

  /* The evaluated copy must be visible again if the original outlives the
   * game object (removal of a non replicated object). */
  if (m_evalPointersStale) {
    Depsgraph *depsgraph = CTX_data_depsgraph_on_load(KX_GetActiveEngine()->GetContext());
    if (depsgraph) {
      if (table == TABLE_OBJECT) {
        Object *ev = DEG_get_evaluated(depsgraph, m_obOrig[slot]);
        m_obEval[slot] = (ev != m_obOrig[slot]) ? ev : nullptr;
      }
      else if (table == TABLE_LIGHT) {
        Object *ev = DEG_get_evaluated(depsgraph, m_lights[slot].ob_orig);
        m_lights[slot].ob_eval = (ev != m_lights[slot].ob_orig) ? ev : nullptr;
      }
    }
  }

  switch (table) {
    case TABLE_OBJECT:
      if ((m_obFlags[slot] & OB_FLAG_APPLIED_HIDDEN) && m_obEval[slot]) {
        restore_eval_visibility(m_obOrig[slot], m_obEval[slot]);
      }
      UnregisterObject(slot);
      break;
    case TABLE_LIGHT:
      if (m_lights[slot].applied_hidden && m_lights[slot].ob_eval) {
        restore_eval_visibility(m_lights[slot].ob_orig, m_lights[slot].ob_eval);
      }
      UnregisterLight(slot);
      break;
    case TABLE_ARMATURE:
      UnregisterArmature(slot);
      break;
    default:
      break;
  }
}

/* Swap-remove of element i of a vector. */
template<typename T> static void swap_remove(std::vector<T> &v, int i)
{
  v[i] = v.back();
  v.pop_back();
}

void KX_CullingGate::UnregisterObject(int slot)
{
  const int last = int(m_obOwner.size()) - 1;

  if (m_obArm[slot] >= 0) {
    m_arms[m_obArm[slot]].skinned_count--;
  }
  m_idToRef.remove(&m_obOrig[slot]->id);
  if (m_occInflight.valid) {
    /* Replayed on the in flight occlusion results when they arrive. */
    m_occMoveLog.emplace_back(slot, last);
  }

  if (slot != last) {
    m_idToRef.add_overwrite(&m_obOrig[last]->id, Ref{TABLE_OBJECT, slot});
    m_obOwner[last]->SetCullingSlot(slot, TABLE_OBJECT);
    for (int k = 0; k < 6; k++) {
      m_localBox[slot * 6 + k] = m_localBox[last * 6 + k];
    }
  }
  m_localBox.resize(size_t(last) * 6);

  swap_remove(m_obOwner, slot);
  swap_remove(m_obOrig, slot);
  swap_remove(m_obEval, slot);
  swap_remove(m_cx, slot);
  swap_remove(m_cy, slot);
  swap_remove(m_cz, slot);
  swap_remove(m_ex, slot);
  swap_remove(m_ey, slot);
  swap_remove(m_ez, slot);
  swap_remove(m_casterBias, slot);
  swap_remove(m_obFlags, slot);
  swap_remove(m_obUserMask, slot);
  swap_remove(m_obMoved, slot);
  swap_remove(m_obArm, slot);
  swap_remove(m_obMargin, slot);
  swap_remove(m_obPending, slot);
  swap_remove(m_obTagFrame, slot);
  swap_remove(m_obOccStreak, slot);
  swap_remove(m_state[TABLE_OBJECT], slot);
}

void KX_CullingGate::UnregisterLight(int slot)
{
  const int last = int(m_lights.size()) - 1;
  m_idToRef.remove(&m_lights[slot].ob_orig->id);
  if (slot != last) {
    m_idToRef.add_overwrite(&m_lights[last].ob_orig->id, Ref{TABLE_LIGHT, slot});
    m_lights[last].owner->SetCullingSlot(slot, TABLE_LIGHT);
  }
  swap_remove(m_lights, slot);
  swap_remove(m_state[TABLE_LIGHT], slot);
}

void KX_CullingGate::UnregisterArmature(int slot)
{
  const int last = int(m_arms.size()) - 1;
  if (Object *ob = m_arms[slot].owner->GetBlenderObject()) {
    m_idToRef.remove(&ob->id);
  }
  /* Skinned meshes of a removed armature fall back to their evaluated
   * bounds, re-read every frame. Rare event: a linear scan is fine. */
  for (size_t i = 0; i < m_obArm.size(); i++) {
    if (m_obArm[i] == slot) {
      m_obArm[i] = -1;
      m_obFlags[i] |= OB_FLAG_DYNAMIC_BOUNDS;
      m_obMoved[i] = 1;
    }
    else if (m_obArm[i] == last) {
      m_obArm[i] = slot;
    }
  }
  if (slot != last) {
    if (Object *ob = m_arms[last].owner->GetBlenderObject()) {
      m_idToRef.add_overwrite(&ob->id, Ref{TABLE_ARMATURE, slot});
    }
    m_arms[last].owner->SetCullingSlot(slot, TABLE_ARMATURE);
  }
  swap_remove(m_arms, slot);
  swap_remove(m_state[TABLE_ARMATURE], slot);
}

void KX_CullingGate::MarkBoundsDynamic(int slot, uint8_t table)
{
  if (slot >= 0 && table == TABLE_OBJECT) {
    m_obFlags[slot] |= OB_FLAG_DYNAMIC_BOUNDS;
    m_obMoved[slot] = 1;
  }
}

void KX_CullingGate::SetUserVisible(int slot, uint8_t table, bool visible)
{
  /* Armatures draw nothing: nothing to hide. */
  if (slot < 0) {
    return;
  }
  if (table == TABLE_OBJECT) {
    m_obUserMask[slot] = visible ? 0xFF : 0x00;
  }
  else if (table == TABLE_LIGHT) {
    m_lights[slot].user_mask = visible ? 0xFF : 0x00;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per frame refresh
 * \{ */

/* Local center/half size to world AABB. The half size of a transformed box
 * along a world axis is the dot of |row| with the local half size (Arvo):
 * exact for the box, no 8-corner loop. */
void KX_CullingGate::UpdateWorldBox(int slot, const float m[4][4])
{
  const float *lb = &m_localBox[size_t(slot) * 6];
  m_cx[slot] = m[0][0] * lb[0] + m[1][0] * lb[1] + m[2][0] * lb[2] + m[3][0];
  m_cy[slot] = m[0][1] * lb[0] + m[1][1] * lb[1] + m[2][1] * lb[2] + m[3][1];
  m_cz[slot] = m[0][2] * lb[0] + m[1][2] * lb[1] + m[2][2] * lb[2] + m[3][2];
  m_ex[slot] = fabsf(m[0][0]) * lb[3] + fabsf(m[1][0]) * lb[4] + fabsf(m[2][0]) * lb[5];
  m_ey[slot] = fabsf(m[0][1]) * lb[3] + fabsf(m[1][1]) * lb[4] + fabsf(m[2][1]) * lb[5];
  m_ez[slot] = fabsf(m[0][2]) * lb[3] + fabsf(m[1][2]) * lb[4] + fabsf(m[2][2]) * lb[5];
}

void KX_CullingGate::UpdateSkinnedBox(int slot)
{
  const ArmRec &arm = m_arms[m_obArm[slot]];
  if (arm.bmin[0] > arm.bmax[0]) {
    /* No pose yet: make the mesh always relevant. */
    m_cx[slot] = m_cy[slot] = m_cz[slot] = 0.0f;
    m_ex[slot] = m_ey[slot] = m_ez[slot] = BIG * 0.25f;
    return;
  }
  const float margin = m_obMargin[slot];
  float *lb = &m_localBox[size_t(slot) * 6];
  for (int k = 0; k < 3; k++) {
    lb[k] = (arm.bmin[k] + arm.bmax[k]) * 0.5f;
    lb[k + 3] = (arm.bmax[k] - arm.bmin[k]) * 0.5f + margin;
  }
  float mat[4][4];
  world_matrix(arm.owner, mat);
  UpdateWorldBox(slot, mat);
}

void KX_CullingGate::RefreshArmatures()
{
  for (ArmRec &rec : m_arms) {
    /* The game computes the pose of the original armature (ApplyPose), the
     * frame stamp tells when it changed. */
    const double frame = rec.owner->GetLastApplyFrame();
    if (frame == rec.pose_frame) {
      continue;
    }
    rec.pose_frame = frame;

    const Object *ob_arma = rec.owner->GetArmatureObject();
    float bmin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float bmax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    if (ob_arma && ob_arma->pose) {
      for (const bPoseChannel &pchan : ob_arma->pose->chanbase) {
        for (int k = 0; k < 3; k++) {
          bmin[k] = std::min({bmin[k], pchan.pose_head[k], pchan.pose_tail[k]});
          bmax[k] = std::max({bmax[k], pchan.pose_head[k], pchan.pose_tail[k]});
        }
      }
    }
    for (int k = 0; k < 3; k++) {
      rec.bmin[k] = bmin[k];
      rec.bmax[k] = bmax[k];
    }
  }
}

void KX_CullingGate::RefreshObjectBounds()
{
  const int n = int(m_obOwner.size());
  for (int i = 0; i < n; i++) {
    if (m_obArm[i] >= 0) {
      /* Follows the pose and the armature transform every frame. */
      UpdateSkinnedBox(i);
      continue;
    }
    const uint8_t flags = m_obFlags[i];
    if (!m_obMoved[i] && !(flags & OB_FLAG_DYNAMIC_BOUNDS)) {
      continue;
    }
    m_obMoved[i] = 0;
    if ((flags & OB_FLAG_DYNAMIC_BOUNDS) && m_obEval[i] && !m_evalPointersStale) {
      /* Bounds of the last evaluation: geometry that changes while culled is
       * only refreshed when it is evaluated again (one frame of latency). */
      const std::optional<Bounds<float3>> b = BKE_object_boundbox_eval_cached_get(m_obEval[i]);
      if (b) {
        float *lb = &m_localBox[size_t(i) * 6];
        lb[0] = (b->min.x + b->max.x) * 0.5f;
        lb[1] = (b->min.y + b->max.y) * 0.5f;
        lb[2] = (b->min.z + b->max.z) * 0.5f;
        lb[3] = (b->max.x - b->min.x) * 0.5f;
        lb[4] = (b->max.y - b->min.y) * 0.5f;
        lb[5] = (b->max.z - b->min.z) * 0.5f;
      }
    }
    float mat[4][4];
    world_matrix(m_obOwner[i], mat);
    UpdateWorldBox(i, mat);
  }
}

void KX_CullingGate::RefreshLights(const Scene *scene)
{
  const float threshold = std::max(1e-16f, scene->eevee.light_threshold);
  for (LightRec &rec : m_lights) {
    const Light &la = *id_cast<const Light *>(rec.ob_orig->data);
    float mat[4][4];
    world_matrix(rec.owner, mat);

    copy_v3_v3(rec.pos, mat[3]);
    /* Blender lights emit along their local -Z axis. */
    float z[3];
    copy_v3_v3(z, mat[2]);
    normalize_v3(z);
    negate_v3_v3(rec.dir, z);

    rec.is_sun = (la.type == LA_SUN);
    rec.shadow = (la.mode & LA_SHADOW) != 0;
    if (rec.is_sun) {
      continue;
    }

    float radius = light_influence_radius(la, threshold);
    float center[3];
    copy_v3_v3(center, rec.pos);

    if (la.type == LA_AREA) {
      /* The emitter extends around its origin. */
      const float sx = la.area_size * len_v3(mat[0]);
      const bool irregular = ELEM(la.area_shape, LA_AREA_RECT, LA_AREA_ELLIPSE);
      const float sy = (irregular ? la.area_sizey : la.area_size) * len_v3(mat[1]);
      radius += 0.5f * sqrtf(sx * sx + sy * sy);
    }
    else if (la.type == LA_SPOT) {
      /* Bounding sphere of the cone (apex at the light, length = radius).
       * Only valid for a circular cone: non uniform scale keeps the sphere. */
      const float s0 = len_v3(mat[0]), s1 = len_v3(mat[1]), s2 = len_v3(mat[2]);
      const float smax = std::max({s0, s1, s2}), smin = std::min({s0, s1, s2});
      const float half = std::min(la.spotsize * 0.5f, float(M_PI_2));
      if (smax - smin <= 1e-4f * smax && half < float(M_PI_2)) {
        const float c = cosf(half);
        if (half <= float(M_PI_4)) {
          /* Circumsphere of apex and base rim. */
          const float r = radius / (2.0f * c);
          madd_v3_v3v3fl(center, rec.pos, rec.dir, r);
          radius = r;
        }
        else {
          /* The base disc dominates. */
          madd_v3_v3v3fl(center, rec.pos, rec.dir, radius * c);
          radius = radius * sinf(half);
        }
      }
    }
    copy_v3_v3(rec.bound, center);
    rec.bound[3] = radius;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Classification
 * \{ */

/* acc[i] = min(acc[i], signed distance of the AABB support point along the
 * plane normal). The box touches the inner half space iff the distance of its
 * most inner corner, n.c + w + |n|.e, is >= 0. Branch-free, vectorizes. */
static void plane_min(const float p[4],
                      const float *cx,
                      const float *cy,
                      const float *cz,
                      const float *ex,
                      const float *ey,
                      const float *ez,
                      int n,
                      float *acc)
{
  const float nx = p[0], ny = p[1], nz = p[2], w = p[3];
  const float ax = fabsf(nx), ay = fabsf(ny), az = fabsf(nz);
  for (int i = 0; i < n; i++) {
    const float d = nx * cx[i] + ny * cy[i] + nz * cz[i] + w + ax * ex[i] + ay * ey[i] +
                    az * ez[i];
    acc[i] = std::min(acc[i], d);
  }
}

/* acc[i] = min(acc[i], r^2 - squared distance from the sphere center to the
 * AABB). Units differ from plane_min but only the sign is consumed, and
 * min/max preserve "all >= 0" / "any >= 0". */
static void sphere_min(const float s[4],
                       const float *cx,
                       const float *cy,
                       const float *cz,
                       const float *ex,
                       const float *ey,
                       const float *ez,
                       int n,
                       float *acc)
{
  const float sx = s[0], sy = s[1], sz = s[2], r2 = s[3];
  for (int i = 0; i < n; i++) {
    const float dx = std::max(fabsf(sx - cx[i]) - ex[i], 0.0f);
    const float dy = std::max(fabsf(sy - cy[i]) - ey[i], 0.0f);
    const float dz = std::max(fabsf(sz - cz[i]) - ez[i], 0.0f);
    acc[i] = std::min(acc[i], r2 - (dx * dx + dy * dy + dz * dz));
  }
}

/* Point where three planes meet (n.x + w = 0 each). */
static bool intersect_planes(const float a[4], const float b[4], const float c[4], float r[3])
{
  float bc[3], ca[3], ab[3];
  cross_v3_v3v3(bc, b, c);
  cross_v3_v3v3(ca, c, a);
  cross_v3_v3v3(ab, a, b);
  const float det = dot_v3v3(a, bc);
  if (fabsf(det) < 1e-20f) {
    return false;
  }
  for (int k = 0; k < 3; k++) {
    r[k] = -(a[3] * bc[k] + b[3] * ca[k] + c[3] * ab[k]) / det;
  }
  return true;
}

/* Frustum plane order (SG_Frustum): near, far, left, right, top, bottom.
 * Corner (f, h, v) = planes f in {near, far}, 2 + h in {left, right},
 * 4 + v in {top, bottom}, stored at f * 4 + h * 2 + v. */
struct FrustumHull {
  float corners[8][3];
  float centroid[3];
  bool valid;
};

static FrustumHull frustum_hull(const float planes[6][4])
{
  FrustumHull hull;
  hull.valid = true;
  zero_v3(hull.centroid);
  for (int f = 0; f < 2; f++) {
    for (int h = 0; h < 2; h++) {
      for (int v = 0; v < 2; v++) {
        float *c = hull.corners[f * 4 + h * 2 + v];
        hull.valid &= intersect_planes(planes[f], planes[2 + h], planes[4 + v], c);
        madd_v3_v3fl(hull.centroid, c, 1.0f / 8.0f);
      }
    }
  }
  return hull;
}

/* The 12 frustum edges: the two faces sharing it, then its two corners. */
static const int8_t FRUSTUM_EDGES[12][4] = {
    /* near/far x left/right. */
    {0, 2, 0, 1}, {0, 3, 2, 3}, {1, 2, 4, 5}, {1, 3, 6, 7},
    /* near/far x top/bottom. */
    {0, 4, 0, 2}, {0, 5, 1, 3}, {1, 4, 4, 6}, {1, 5, 5, 7},
    /* left/right x top/bottom. */
    {2, 4, 0, 4}, {2, 5, 1, 5}, {3, 4, 2, 6}, {3, 5, 3, 7},
};

/* Convex hull of the frustum and a light: a point light at L, or a sun at
 * infinity against its travel direction d (the frustum swept toward the
 * sun). Its planes are the frustum faces that see the light from their inner
 * side, plus one plane per silhouette edge (an edge between a kept and a
 * dropped face) spanned by the edge and the light. Exact for the hull; when
 * the corners are degenerate the kept faces alone are a conservative bound. */
static void caster_hull(const float frustum[6][4],
                        const FrustumHull &hull,
                        const float light[3],
                        bool is_sun,
                        float r_planes[][4],
                        int &r_count)
{
  bool front[6];
  r_count = 0;
  for (int p = 0; p < 6; p++) {
    /* A sun is the point at infinity along light: the sign of n.light. */
    const float d = is_sun ? dot_v3v3(frustum[p], light) :
                             dot_v3v3(frustum[p], light) + frustum[p][3];
    front[p] = d >= 0.0f;
    if (front[p]) {
      copy_v4_v4(r_planes[r_count++], frustum[p]);
    }
  }
  if (!hull.valid) {
    return;
  }
  for (const int8_t *edge : FRUSTUM_EDGES) {
    if (front[edge[0]] == front[edge[1]]) {
      continue;
    }
    const float *a = hull.corners[edge[2]];
    const float *b = hull.corners[edge[3]];
    float e[3], to_light[3], n[3];
    sub_v3_v3v3(e, b, a);
    if (is_sun) {
      copy_v3_v3(to_light, light);
    }
    else {
      sub_v3_v3v3(to_light, light, a);
    }
    cross_v3_v3v3(n, e, to_light);
    const float len = len_v3(n);
    if (len <= 1e-6f * len_v3(e) * len_v3(to_light)) {
      /* Edge points at the light: the plane is undefined, skip it. */
      continue;
    }
    mul_v3_fl(n, 1.0f / len);
    float w = -dot_v3v3(n, a);
    /* The hull contains the frustum, so it contains its centroid. */
    if (dot_v3v3(n, hull.centroid) + w < 0.0f) {
      negate_v3(n);
      w = -w;
    }
    copy_v3_v3(r_planes[r_count], n);
    r_planes[r_count][3] = w;
    r_count++;
  }
}

static bool sphere_in_planes(const float c[3], float r, const float planes[6][4])
{
  for (int p = 0; p < 6; p++) {
    if (planes[p][0] * c[0] + planes[p][1] * c[1] + planes[p][2] * c[2] + planes[p][3] < -r) {
      return false;
    }
  }
  return true;
}

void KX_CullingGate::ClassifyView(KX_Camera *cam,
                                  uint8_t *ob_mask,
                                  uint8_t *light_mask,
                                  bool accumulate)
{
  /* Inward normalized planes: n.x + w >= 0 inside. */
  float frustum[6][4];
  const std::array<MT_Vector4, 6> &fp = cam->GetFrustum().GetPlanes();
  for (int p = 0; p < 6; p++) {
    for (int k = 0; k < 4; k++) {
      frustum[p][k] = fp[p][k];
    }
  }

  /* Lights, and the caster volumes of the visible shadowed ones. */
  const FrustumHull hull = frustum_hull(frustum);
  m_volumes.clear();
  for (size_t l = 0; l < m_lights.size(); l++) {
    const LightRec &rec = m_lights[l];
    bool relevant;
    if (rec.is_sun) {
      relevant = true;
    }
    else {
      relevant = sphere_in_planes(rec.bound, rec.bound[3], frustum);
    }
    light_mask[l] = accumulate ? uint8_t(light_mask[l] | relevant) : uint8_t(relevant);
    if (!relevant || !rec.shadow) {
      continue;
    }

    /* A caster occludes a visible receiver from the light only if it lies on
     * the segment between them (for a sun: on the ray toward the sun), so
     * casters are inside the hull of the frustum and the light. Local lights
     * also bound it by their influence sphere. */
    Volume vol;
    vol.sphere[3] = -1.0f;
    if (rec.is_sun) {
      /* -d points to the sun: sweep against the travel direction. */
      float to_sun[3];
      negate_v3_v3(to_sun, rec.dir);
      caster_hull(frustum, hull, to_sun, true, vol.planes, vol.plane_count);
    }
    else {
      caster_hull(frustum, hull, rec.pos, false, vol.planes, vol.plane_count);
      copy_v3_v3(vol.sphere, rec.bound);
      vol.sphere[3] = rec.bound[3] * rec.bound[3];
    }
    m_volumes.push_back(vol);
  }

  /* Objects. The camera and caster relevances are kept apart: occlusion
   * only applies to the first, an object hidden from the camera can still
   * shadow what the camera sees. */
  const int n = int(m_obOwner.size());
  float cam_rel[BLOCK], cast_rel[BLOCK], tmp[BLOCK];
  for (int b0 = 0; b0 < n; b0 += BLOCK) {
    const int bn = std::min(BLOCK, n - b0);
    const float *cx = m_cx.data() + b0, *cy = m_cy.data() + b0, *cz = m_cz.data() + b0;
    const float *ex = m_ex.data() + b0, *ey = m_ey.data() + b0, *ez = m_ez.data() + b0;
    const float *bias = m_casterBias.data() + b0;

    std::fill(cam_rel, cam_rel + bn, BIG);
    for (int p = 0; p < 6; p++) {
      plane_min(frustum[p], cx, cy, cz, ex, ey, ez, bn, cam_rel);
    }

    std::fill(cast_rel, cast_rel + bn, NOT_A_CASTER);
    for (const Volume &vol : m_volumes) {
      std::fill(tmp, tmp + bn, BIG);
      for (int p = 0; p < vol.plane_count; p++) {
        plane_min(vol.planes[p], cx, cy, cz, ex, ey, ez, bn, tmp);
      }
      if (vol.sphere[3] >= 0.0f) {
        sphere_min(vol.sphere, cx, cy, cz, ex, ey, ez, bn, tmp);
      }
      for (int i = 0; i < bn; i++) {
        cast_rel[i] = std::max(cast_rel[i], tmp[i] + bias[i]);
      }
    }

    uint8_t *mask = ob_mask + b0;
    if (!accumulate) {
      std::fill(mask, mask + bn, 0);
    }
    for (int i = 0; i < bn; i++) {
      mask[i] |= uint8_t((cam_rel[i] >= 0.0f ? MASK_CAMERA : 0) |
                         (cast_rel[i] >= 0.0f ? MASK_CASTER : 0));
    }
  }
}

void KX_CullingGate::Refresh(const Scene *scene)
{
  RefreshArmatures();
  RefreshObjectBounds();
  RefreshLights(scene);
}

void KX_CullingGate::BeginPass(KX_Camera *cam,
                               const std::vector<KX_Camera *> &aux_cams,
                               const Scene *scene,
                               bool enabled,
                               bool use_occlusion)
{
  if (m_refreshedFrame != m_frame) {
    Refresh(scene);
    m_refreshedFrame = m_frame;
  }

  const size_t n = m_obOwner.size();
  const size_t nl = m_lights.size();
  m_obMaskMain.resize(n);
  m_obMaskAux.resize(n);
  m_lightMaskMain.resize(nl);
  m_lightMaskAux.resize(nl);

  if (!enabled || !cam) {
    std::fill(m_obMaskMain.begin(), m_obMaskMain.end(), MASK_CAMERA);
    std::fill(m_lightMaskMain.begin(), m_lightMaskMain.end(), 1);
    std::fill(m_obMaskAux.begin(), m_obMaskAux.end(), 0);
    std::fill(m_lightMaskAux.begin(), m_lightMaskAux.end(), 0);
  }
  else {
    ClassifyView(cam, m_obMaskMain.data(), m_lightMaskMain.data(), false);
    std::fill(m_obMaskAux.begin(), m_obMaskAux.end(), 0);
    std::fill(m_lightMaskAux.begin(), m_lightMaskAux.end(), 0);
    for (KX_Camera *aux : aux_cams) {
      if (aux && aux != cam && aux->hasValidProjectionMatrix()) {
        ClassifyView(aux, m_obMaskAux.data(), m_lightMaskAux.data(), true);
      }
    }
  }

  /* Occluded from the camera in the last two results, and only relevant to
   * the camera. The streak is only meaningful for the camera that rendered
   * the tested depth. */
  const bool occlusion = enabled && use_occlusion && OcclusionUsable(cam);

  /* State bytes. Hidden by the user: neither drawn nor worth evaluating. */
  uint8_t *ob_state = m_state[TABLE_OBJECT].data();
  int occluded = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t m = m_obMaskMain[i];
    const bool occ = occlusion && (m == MASK_CAMERA) && m_obOccStreak[i] >= 2;
    occluded += occ;
    const uint8_t s = uint8_t(((m && !occ) ? (STATE_DRAW | STATE_EVAL) : 0) |
                              (m_obMaskAux[i] ? STATE_EVAL : 0));
    ob_state[i] = s & m_obUserMask[i];
  }
  uint8_t *light_state = m_state[TABLE_LIGHT].data();
  for (size_t l = 0; l < nl; l++) {
    const uint8_t s = uint8_t((m_lightMaskMain[l] ? (STATE_DRAW | STATE_EVAL) : 0) |
                              (m_lightMaskAux[l] ? STATE_EVAL : 0));
    light_state[l] = s & m_lights[l].user_mask;
  }

  /* An armature is evaluated when one of its skinned meshes is. */
  uint8_t *arm_state = m_state[TABLE_ARMATURE].data();
  for (size_t a = 0; a < m_arms.size(); a++) {
    arm_state[a] = (m_arms[a].skinned_count == 0) ? STATE_EVAL : 0;
  }
  for (size_t i = 0; i < n; i++) {
    if (m_obArm[i] >= 0 && (ob_state[i] & STATE_EVAL)) {
      arm_state[m_obArm[i]] = STATE_EVAL;
    }
  }

  /* Counters for profiling. */
  m_stats = Stats();
  m_stats.objects_occluded = occluded;
  m_stats.objects = int(n);
  m_stats.lights = int(nl);
  m_stats.skinned = 0;
  for (size_t i = 0; i < n; i++) {
    m_stats.objects_drawn += (ob_state[i] & STATE_DRAW) != 0;
    m_stats.objects_evaluated += (ob_state[i] & STATE_EVAL) != 0;
    m_stats.skinned += m_obArm[i] >= 0;
  }
  for (size_t l = 0; l < nl; l++) {
    m_stats.lights_drawn += (light_state[l] & STATE_DRAW) != 0;
  }
  for (size_t a = 0; a < m_arms.size(); a++) {
    m_stats.armatures_deferred += (arm_state[a] & STATE_EVAL) == 0;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred tags
 * \{ */

bool KX_CullingGate::DeferIdTag(ID *id, int recalc)
{
  const Ref *ref = m_idToRef.lookup_ptr(id);
  if (!ref || (m_state[ref->table][ref->index] & STATE_EVAL)) {
    return false;
  }
  switch (ref->table) {
    case TABLE_OBJECT:
      if (m_obFlags[ref->index] & OB_FLAG_DYNAMIC_BOUNDS) {
        /* The tag may change the bounds: evaluate it so the gate sees the
         * new geometry. */
        return false;
      }
      m_obPending[ref->index] |= recalc;
      return true;
    case TABLE_LIGHT:
      m_lights[ref->index].pending_recalc |= recalc;
      return true;
    case TABLE_ARMATURE:
      m_arms[ref->index].pending_recalc |= recalc;
      return true;
    default:
      return false;
  }
}

void KX_CullingGate::FlushDeferredTags(int slot, uint8_t table)
{
  if (slot < 0) {
    return;
  }
  int *pending = nullptr;
  ID *id = nullptr;
  switch (table) {
    case TABLE_OBJECT:
      pending = &m_obPending[slot];
      id = &m_obOrig[slot]->id;
      break;
    case TABLE_LIGHT:
      pending = &m_lights[slot].pending_recalc;
      id = &m_lights[slot].ob_orig->id;
      break;
    case TABLE_ARMATURE: {
      pending = &m_arms[slot].pending_recalc;
      Object *ob = m_arms[slot].owner->GetBlenderObject();
      id = ob ? &ob->id : nullptr;
      break;
    }
    default:
      return;
  }
  if (*pending && id) {
    DEG_id_tag_update(id, *pending);
  }
  *pending = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw visibility
 * \{ */

void KX_CullingGate::RefreshEvalPointers(Depsgraph *depsgraph)
{
  for (size_t i = 0; i < m_obOrig.size(); i++) {
    Object *ev = DEG_get_evaluated(depsgraph, m_obOrig[i]);
    m_obEval[i] = (ev != m_obOrig[i]) ? ev : nullptr;
  }
  for (LightRec &rec : m_lights) {
    Object *ev = DEG_get_evaluated(depsgraph, rec.ob_orig);
    rec.ob_eval = (ev != rec.ob_orig) ? ev : nullptr;
  }
  m_evalPointersStale = false;
}

void KX_CullingGate::ApplyDrawState(Depsgraph *depsgraph)
{
  if (m_evalPointersStale) {
    RefreshEvalPointers(depsgraph);
  }

  /* Culled objects are written every pass, not only on transition: a tag
   * from outside the gate (physics, scripts) re-copies the original into the
   * evaluated object and would silently make it visible again. One store per
   * culled object is cheap next to what its evaluation and sync would cost. */
  const uint8_t *ob_state = m_state[TABLE_OBJECT].data();
  for (size_t i = 0; i < m_obOwner.size(); i++) {
    Object *ev = m_obEval[i];
    if (!ev) {
      continue;
    }
    if (!(ob_state[i] & STATE_DRAW)) {
      ev->visibility_flag |= OB_HIDE_VIEWPORT;
      m_obFlags[i] |= OB_FLAG_APPLIED_HIDDEN;
    }
    else if (m_obFlags[i] & OB_FLAG_APPLIED_HIDDEN) {
      restore_eval_visibility(m_obOrig[i], ev);
      m_obFlags[i] &= ~OB_FLAG_APPLIED_HIDDEN;
    }
  }

  const uint8_t *light_state = m_state[TABLE_LIGHT].data();
  for (size_t l = 0; l < m_lights.size(); l++) {
    LightRec &rec = m_lights[l];
    if (!rec.ob_eval) {
      continue;
    }
    if (!(light_state[l] & STATE_DRAW)) {
      rec.ob_eval->visibility_flag |= OB_HIDE_VIEWPORT;
      rec.applied_hidden = 1;
    }
    else if (rec.applied_hidden) {
      restore_eval_visibility(rec.ob_orig, rec.ob_eval);
      rec.applied_hidden = 0;
    }
  }
}

void KX_CullingGate::ApplyAuxView(KX_Camera *cam, const Scene *scene, Depsgraph *depsgraph)
{
  if (m_evalPointersStale) {
    RefreshEvalPointers(depsgraph);
  }
  /* Image renders run from the logic, before the render passes of the
   * frame: refresh without consuming the frame refresh of the main pass,
   * which must still see the motion that happens after this render. */
  Refresh(scene);

  const size_t n = m_obOwner.size();
  const size_t nl = m_lights.size();
  m_obMaskAux.resize(n);
  m_lightMaskAux.resize(nl);
  ClassifyView(cam, m_obMaskAux.data(), m_lightMaskAux.data(), false);

  for (size_t i = 0; i < n; i++) {
    Object *ev = m_obEval[i];
    if (!ev) {
      continue;
    }
    const bool draw = m_obMaskAux[i] && m_obUserMask[i];
    if (!draw) {
      ev->visibility_flag |= OB_HIDE_VIEWPORT;
      m_obFlags[i] |= OB_FLAG_APPLIED_HIDDEN;
    }
    else if (m_obFlags[i] & OB_FLAG_APPLIED_HIDDEN) {
      /* Possibly skipped by the last main pass: its evaluated transform can
       * be stale. Rigid transforms are pushed directly, no depsgraph. */
      m_obOwner[i]->TagForTransformUpdateEvaluated(true);
      restore_eval_visibility(m_obOrig[i], ev);
      m_obFlags[i] &= ~OB_FLAG_APPLIED_HIDDEN;
    }
  }
  for (size_t l = 0; l < nl; l++) {
    LightRec &rec = m_lights[l];
    if (!rec.ob_eval) {
      continue;
    }
    if (!(m_lightMaskAux[l] && rec.user_mask)) {
      rec.ob_eval->visibility_flag |= OB_HIDE_VIEWPORT;
      rec.applied_hidden = 1;
    }
    else if (rec.applied_hidden) {
      rec.owner->TagForTransformUpdateEvaluated(true);
      restore_eval_visibility(rec.ob_orig, rec.ob_eval);
      rec.applied_hidden = 0;
    }
  }
}

void KX_CullingGate::RestoreAll(Depsgraph *depsgraph)
{
  if (depsgraph && m_evalPointersStale) {
    RefreshEvalPointers(depsgraph);
  }
  for (size_t i = 0; i < m_obOwner.size(); i++) {
    if (!(m_state[TABLE_OBJECT][i] & STATE_EVAL)) {
      /* Push the transform the culled object never got. */
      FlushDeferredTags(int(i), TABLE_OBJECT);
      m_obOwner[i]->TagForTransformUpdate(false, true);
    }
    if ((m_obFlags[i] & OB_FLAG_APPLIED_HIDDEN) && m_obEval[i]) {
      restore_eval_visibility(m_obOrig[i], m_obEval[i]);
      m_obFlags[i] &= ~OB_FLAG_APPLIED_HIDDEN;
    }
    m_state[TABLE_OBJECT][i] = STATE_DRAW | STATE_EVAL;
  }
  for (size_t l = 0; l < m_lights.size(); l++) {
    LightRec &rec = m_lights[l];
    if (!(m_state[TABLE_LIGHT][l] & STATE_EVAL)) {
      FlushDeferredTags(int(l), TABLE_LIGHT);
      rec.owner->TagForTransformUpdate(false, true);
    }
    if (rec.applied_hidden && rec.ob_eval) {
      restore_eval_visibility(rec.ob_orig, rec.ob_eval);
      rec.applied_hidden = 0;
    }
    m_state[TABLE_LIGHT][l] = STATE_DRAW | STATE_EVAL;
  }
  for (size_t a = 0; a < m_arms.size(); a++) {
    FlushDeferredTags(int(a), TABLE_ARMATURE);
    m_state[TABLE_ARMATURE][a] = STATE_EVAL;
  }
}

/** \} */

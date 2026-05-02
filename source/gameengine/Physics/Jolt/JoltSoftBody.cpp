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
 * Contributor(s): UPBGE Contributors
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file JoltSoftBody.cpp
 *  \ingroup physjolt
 */

#include "JoltSoftBody.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"

#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_scene.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "MT_Matrix4x4.h"

#include <algorithm>
#include <cmath>
#include "MT_Transform.h"
#include "RAS_IDisplayArray.h"
#include "SG_Node.h"
#include "PHY_IMotionState.h"
#include "PHY_IPhysicsController.h"
#include "RAS_MeshObject.h"

#include "MEM_guardedalloc.h"

#include <cfloat>

using namespace blender;

namespace {

inline bool IsNearlyIdentityRotation(const MT_Matrix3x3 &m, float eps = 1.0e-6f)
{
  return (std::abs((float)m[0][0] - 1.0f) <= eps &&
          std::abs((float)m[0][1]) <= eps &&
          std::abs((float)m[0][2]) <= eps &&
          std::abs((float)m[1][0]) <= eps &&
          std::abs((float)m[1][1] - 1.0f) <= eps &&
          std::abs((float)m[1][2]) <= eps &&
          std::abs((float)m[2][0]) <= eps &&
          std::abs((float)m[2][1]) <= eps &&
          std::abs((float)m[2][2] - 1.0f) <= eps);
}

inline uint64_t MakeRotationCacheKey(const MT_Matrix3x3 &rotationDelta)
{
  MT_Quaternion q = rotationDelta.getRotation();
  const float qLen = (float)q.length();
  if (qLen > 1.0e-12f) {
    q /= qLen;
  }
  else {
    q.setValue(0.0f, 0.0f, 0.0f, 1.0f);
  }

  if (q.w() < 0.0f) {
    q.setValue(-q.x(), -q.y(), -q.z(), -q.w());
  }

  auto quantize = [](float c) -> uint16_t {
    c = std::clamp(c, -1.0f, 1.0f);
    const int32_t qv = (int32_t)std::lround(c * 32767.0f);
    return (uint16_t)(qv + 32768);
  };

  uint64_t key = 1469598103934665603ull; /* FNV-1a 64-bit offset basis */
  auto mix = [&key](uint16_t v) {
    key ^= (uint64_t)v;
    key *= 1099511628211ull; /* FNV-1a 64-bit prime */
  };

  mix(quantize((float)q.x()));
  mix(quantize((float)q.y()));
  mix(quantize((float)q.z()));
  mix(quantize((float)q.w()));
  return key;
}

static bool JoltIsSoftBodyModifier(const ModifierData *md)
{
  return md && md->type == eModifierType_SimpleDeformBGE &&
         STRPREFIX(md->name, "joltSbModifier");
}

static bool JoltSoftBodyHasRenderOnlySubsurf(Object *ob)
{
  if (!ob || ob->type != OB_MESH) {
    return false;
  }

  bool has_render_only_subsurf = false;
  for (ModifierData *md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
    if (!(md->mode & eModifierMode_Realtime)) {
      continue;
    }

    if (md->type != eModifierType_Subsurf) {
      const ModifierTypeInfo *mti = BKE_modifier_get_info(ModifierType(md->type));
      if (mti && mti->type != ModifierTypeType::OnlyDeform &&
          mti->type != ModifierTypeType::NonGeometrical && !has_render_only_subsurf) {
        return false;
      }
      continue;
    }

    SubsurfModifierData *smd = (SubsurfModifierData *)md;
    if (smd->levels > 0) {
      return false;
    }
    if (smd->renderLevels > smd->levels) {
      has_render_only_subsurf = true;
    }
  }

  return has_render_only_subsurf;
}

static bool JoltEnsureSoftBodyRenderSubsurfLevels(
    Object *ob,
    std::vector<std::pair<SubsurfModifierData *, short>> &overrides,
    bool &changed)
{
  changed = false;
  if (!overrides.empty()) {
    return true;
  }
  if (!JoltSoftBodyHasRenderOnlySubsurf(ob)) {
    return false;
  }

  for (ModifierData *md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
    if (md->type != eModifierType_Subsurf || !(md->mode & eModifierMode_Realtime)) {
      continue;
    }

    SubsurfModifierData *smd = (SubsurfModifierData *)md;
    if (smd->renderLevels > smd->levels) {
      overrides.push_back(std::make_pair(smd, smd->levels));
      smd->levels = smd->renderLevels;
      changed = true;
    }
  }

  return !overrides.empty();
}

static void JoltRestoreSoftBodyRenderSubsurfLevels(
    Object *ob, std::vector<std::pair<SubsurfModifierData *, short>> &overrides)
{
  if (overrides.empty()) {
    return;
  }

  for (const std::pair<SubsurfModifierData *, short> &item : overrides) {
    item.first->levels = item.second;
  }
  overrides.clear();

  if (ob) {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }
}

static ModifierData *JoltFirstTopologyModifier(Object *ob)
{
  for (ModifierData *md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info(ModifierType(md->type));
    if (!mti) {
      continue;
    }
    if (mti->type != ModifierTypeType::OnlyDeform &&
        mti->type != ModifierTypeType::NonGeometrical) {
      return md;
    }
  }

  return nullptr;
}

static void JoltInsertSoftBodyModifier(Object *ob,
                                       SimpleDeformModifierDataBGE *modifier,
                                       bool before_topology)
{
  ModifierData *before = before_topology ? JoltFirstTopologyModifier(ob) : nullptr;
  if (before) {
    BLI_insertlinkbefore(&ob->modifiers, before, modifier);
  }
  else {
    BLI_addtail(&ob->modifiers, modifier);
  }
}

inline JPH::Vec3 CalculateBoundsCenter(
    const JPH::Array<JPH::SoftBodySharedSettings::Vertex> &vertices)
{
  if (vertices.empty()) {
    return JPH::Vec3::sZero();
  }

  const JPH::Float3 &p0 = vertices[0].mPosition;
  float minX = p0[0], minY = p0[1], minZ = p0[2];
  float maxX = p0[0], maxY = p0[1], maxZ = p0[2];

  for (size_t i = 1; i < vertices.size(); ++i) {
    const JPH::Float3 &p = vertices[i].mPosition;
    minX = std::min(minX, p[0]);
    minY = std::min(minY, p[1]);
    minZ = std::min(minZ, p[2]);
    maxX = std::max(maxX, p[0]);
    maxY = std::max(maxY, p[1]);
    maxZ = std::max(maxZ, p[2]);
  }

  return JPH::Vec3(0.5f * (minX + maxX),
                   0.5f * (minY + maxY),
                   0.5f * (minZ + maxZ));
}

inline void RecenterVertices(JPH::Array<JPH::SoftBodySharedSettings::Vertex> &vertices,
                             JPH::Vec3Arg center)
{
  for (JPH::SoftBodySharedSettings::Vertex &v : vertices) {
    const JPH::Float3 &p = v.mPosition;
    v.mPosition = JPH::Float3(p[0] - center.GetX(),
                              p[1] - center.GetY(),
                              p[2] - center.GetZ());
  }
}

inline JPH::Vec3 CalculateRotatedBoundsCenter(
    const JPH::Array<JPH::SoftBodySharedSettings::Vertex> &vertices,
    const MT_Matrix3x3 &rotationDelta)
{
  if (vertices.empty()) {
    return JPH::Vec3::sZero();
  }

  const JPH::Float3 &p0 = vertices[0].mPosition;
  MT_Vector3 baseLocal(p0[0], -p0[2], p0[1]);
  MT_Vector3 baseRotated = rotationDelta * baseLocal;
  float minX = (float)baseRotated.x();
  float minY = (float)baseRotated.z();
  float minZ = (float)-baseRotated.y();
  float maxX = minX, maxY = minY, maxZ = minZ;

  for (size_t i = 1; i < vertices.size(); ++i) {
    const JPH::Float3 &jp = vertices[i].mPosition;
    MT_Vector3 blenderLocal(jp[0], -jp[2], jp[1]);
    MT_Vector3 rotated = rotationDelta * blenderLocal;

    const float x = (float)rotated.x();
    const float y = (float)rotated.z();
    const float z = (float)-rotated.y();
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    minZ = std::min(minZ, z);
    maxX = std::max(maxX, x);
    maxY = std::max(maxY, y);
    maxZ = std::max(maxZ, z);
  }

  return JPH::Vec3(0.5f * (minX + maxX),
                   0.5f * (minY + maxY),
                   0.5f * (minZ + maxZ));
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Construction / Destruction
 * \{ */

JoltSoftBody::JoltSoftBody(JoltPhysicsEnvironment *env, JoltPhysicsController *ctrl)
    : m_env(env), m_ctrl(ctrl)
{
}

JoltSoftBody::~JoltSoftBody()
{
  if (m_bodyDestructionHandledByEnvironment) {
    return;
  }

  if (!m_bodyID.IsInvalid() && m_env) {
    /* If physics is currently updating, defer body destruction. */
    if (m_env->IsPhysicsUpdating()) {
      JoltDeferredOp op;
      op.type = JoltDeferredOpType::DestroyBody;
      op.bodyID = m_bodyID;
      op.controller = nullptr;
      m_env->QueueDeferredOperation(op);
    }
    else {
      JPH::BodyInterface &bi = m_env->GetBodyInterface();
      if (bi.IsAdded(m_bodyID)) {
        bi.RemoveBody(m_bodyID);
      }
      bi.DestroyBody(m_bodyID);
    }
    /* Do NOT touch m_ctrl here: controller lifetime is managed externally.
     * Runtime teardown can destroy us from ~JoltPhysicsController(), while
     * environment teardown may delete us after controllers were already removed.
     * In both cases m_ctrl may be stale. */
  }
}

void JoltSoftBody::ConfigurePlasticity(const JoltSoftBodySettings &settings,
                                       const JPH::SoftBodySharedSettings &sharedSettings)
{
  m_plasticityEnabled = settings.plasticity;
  m_plasticThreshold = std::max(settings.plasticThreshold, 0.0f);
  m_plasticStrength = std::clamp(settings.plasticStrength, 0.0f, 1.0f);
  m_plasticMaxDeform = std::max(settings.plasticMaxDeform, 0.0f);
  m_plasticRepairRate = std::max(settings.plasticRepairRate, 0.0f);

  m_originalEdgeRestLengths.clear();
  m_plasticDirtyVertices.clear();
  m_hasPlasticDeformation = false;

  if (!m_plasticityEnabled || sharedSettings.mEdgeConstraints.empty()) {
    m_plasticityEnabled = false;
    return;
  }

  m_originalEdgeRestLengths.reserve(sharedSettings.mEdgeConstraints.size());
  for (const JPH::SoftBodySharedSettings::Edge &edge : sharedSettings.mEdgeConstraints) {
    m_originalEdgeRestLengths.push_back(edge.mRestLength);
  }
  m_plasticDirtyVertices.assign(sharedSettings.mVertices.size(), 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Create
 * \{ */

bool JoltSoftBody::Create(const float *vertices,
                          int numVerts,
                          const int *triangles,
                          int numTriangles,
                          const MT_Vector3 &position,
                          const MT_Matrix3x3 &worldOri,
                          const MT_Vector3 &worldScale,
                          const JoltSoftBodySettings &settings,
                          const std::unordered_map<int, int> &vertRemap)
{
  if (!m_env || numVerts < 3 || numTriangles < 1) {
    return false;
  }

  /* Build dense Blender->Jolt lookup table used in UpdateMesh().
   * Unmapped vertices keep -1 so we can skip them without hash lookups. */
  m_blenderVertToJoltVert.clear();
  if (!vertRemap.empty()) {
    int maxBlenderVert = -1;
    for (const auto &[blenderVert, joltVert] : vertRemap) {
      (void)joltVert;
      maxBlenderVert = std::max(maxBlenderVert, blenderVert);
    }

    if (maxBlenderVert >= 0) {
      m_blenderVertToJoltVert.assign((size_t)maxBlenderVert + 1, -1);
      for (const auto &[blenderVert, joltVert] : vertRemap) {
        if (blenderVert >= 0) {
          m_blenderVertToJoltVert[(size_t)blenderVert] = joltVert;
        }
      }
    }
  }

  /* ---- Build SoftBodySharedSettings ---- */
  JPH::Ref<JPH::SoftBodySharedSettings> sbSettings = new JPH::SoftBodySharedSettings();

  /* Per-particle inverse mass: distribute total mass evenly.
   * Per Jolt docs, mInvMass=0 pins the vertex (infinite mass). */
  float invMass = (settings.mass > 0.0f) ? (1.0f / settings.mass) * (float)numVerts : 1.0f;
  sbSettings->mVertices.resize(numVerts);
  const bool hasPinVertexWeights = !settings.pinVertexWeights.empty();
  std::vector<MT_Vector3> worldPositions;
  if (hasPinVertexWeights) {
    worldPositions.resize(numVerts);
  }

  for (int i = 0; i < numVerts; ++i) {
    float bx = vertices[i * 3 + 0];
    float by = vertices[i * 3 + 1];
    float bz = vertices[i * 3 + 2];

    /* Bake the full viewport world transform into each particle position so
     * the soft body starts with the correct shape, size, and orientation at
     * frame 0 with no visual jump.
     *
     * Blender's object transform order is T * R * S (scale first in local
     * space, then rotate). We replicate that here:
     *   1. Apply world scale component-wise (local space).
     *   2. Apply world rotation matrix (Blender Z-up space).
     * The resulting vector is still in Blender Z-up, then axis-swapped to
     * Jolt Y-up. */
    MT_Vector3 v(static_cast<double>(bx) * worldScale.x(),
                 static_cast<double>(by) * worldScale.y(),
                 static_cast<double>(bz) * worldScale.z());
    MT_Vector3 vr = worldOri * v;

    /* Blender (X,Y,Z) → Jolt (X,Z,-Y) */
    sbSettings->mVertices[i].mPosition = JPH::Float3(
        static_cast<float>(vr.x()),
        static_cast<float>(vr.z()),
        -static_cast<float>(vr.y()));
    sbSettings->mVertices[i].mInvMass = invMass;

    /* Store initial world positions for pin initialization below. */
    if (hasPinVertexWeights) {
      worldPositions[i] = vr;
    }
  }

  /* ---- Vertex pinning ----
   * Vertices whose weight in the pin vertex group >= pinWeightThreshold get
   * mInvMass = 0 (kinematic).  They stay at their initial world position unless
   * UpdatePinnedVertices() moves them to follow a pin object. */
  if (hasPinVertexWeights) {
    if (settings.hasPinObject) {
      m_pinInitialPos = settings.pinInitialPos;
      m_pinInitialOri = settings.pinInitialOri;
    }
    MT_Matrix3x3 invPinInitialOri = m_pinInitialOri.transposed();

    /* Deduplicate: multiple Blender verts can map to the same Jolt particle. */
    std::unordered_map<int, float> joltPinWeight;
    for (const auto &[joltIdx, w] : settings.pinVertexWeights) {
      auto it = joltPinWeight.find(joltIdx);
      if (it == joltPinWeight.end() || w > it->second) {
        joltPinWeight[joltIdx] = w;
      }
    }
    for (const auto &[joltIdx, w] : joltPinWeight) {
      if (w >= settings.pinWeightThreshold && joltIdx >= 0 && joltIdx < numVerts) {
        sbSettings->mVertices[joltIdx].mInvMass = 0.0f; /* kinematic */
        /* Store (joltIdx, local offset in the pin's initial frame). */
        MT_Vector3 initWorldPos = position + worldPositions[joltIdx];
        MT_Vector3 pinLocalOffset = invPinInitialOri * (initWorldPos - m_pinInitialPos);
        m_pinnedData.push_back({joltIdx, pinLocalOffset});
      }
    }
    m_noPinCollision = settings.noPinCollision;
    m_pinTransformFollow = settings.pinTransformFollow;
  }

  /* ---- Pre-center particles to eliminate the first-step origin jump ----
   * Jolt's mUpdatePosition=true shifts the body by the center of the LOCAL
   * AABB (mLocalBounds.GetCenter()) after each update. If the soft body is
   * spawned with a non-zero local bounds center, frame 1 applies that shift
   * and the game object appears to teleport.
   *
   * Subtracting the local bounds center from every particle makes
   * mLocalBounds.GetCenter() == (0,0,0) on the first update, so Jolt leaves
   * body.GetPosition() unchanged. We add the same offset to spawn position so
   * the body starts exactly where Jolt would have moved it, but with no jump.
   *
   * This is translation-only, so pairwise rest lengths/constraints are
   * unchanged. */
  JPH::Vec3 particleBoundsCenter(0.0f, 0.0f, 0.0f);
  if (numVerts > 0) {
    const JPH::Float3 &p0 = sbSettings->mVertices[0].mPosition;
    float minX = p0[0], minY = p0[1], minZ = p0[2];
    float maxX = p0[0], maxY = p0[1], maxZ = p0[2];

    for (int i = 1; i < numVerts; ++i) {
      const JPH::Float3 &p = sbSettings->mVertices[i].mPosition;
      minX = std::min(minX, p[0]);
      minY = std::min(minY, p[1]);
      minZ = std::min(minZ, p[2]);
      maxX = std::max(maxX, p[0]);
      maxY = std::max(maxY, p[1]);
      maxZ = std::max(maxZ, p[2]);
    }

    particleBoundsCenter = JPH::Vec3(0.5f * (minX + maxX),
                                     0.5f * (minY + maxY),
                                     0.5f * (minZ + maxZ));

    for (int i = 0; i < numVerts; ++i) {
      JPH::Float3 &p = sbSettings->mVertices[i].mPosition;
      p = JPH::Float3(p[0] - particleBoundsCenter.GetX(),
                      p[1] - particleBoundsCenter.GetY(),
                      p[2] - particleBoundsCenter.GetZ());
    }
  }

  /* Store COM offset (body position - game-object origin) in Blender world space.
   * WriteDynamicsToMotionState subtracts this so startup SG origin stays at the
   * authored object origin while the Jolt body can remain at COM. */
  m_bodyOriginOffset = JoltMath::ToMT(particleBoundsCenter);

  /* Add faces. */
  sbSettings->mFaces.resize(numTriangles);
  for (int i = 0; i < numTriangles; ++i) {
    sbSettings->mFaces[i] = JPH::SoftBodySharedSettings::Face(
        (JPH::uint32)triangles[i * 3 + 0],
        (JPH::uint32)triangles[i * 3 + 1],
        (JPH::uint32)triangles[i * 3 + 2]);
  }

  /* ---- Constraint generation ----
   * Jolt uses compliance (inverse stiffness) in XPBD formulation.
   * compliance = 0 means infinitely stiff (rigid constraint).
   * UI stiffness 0..1 maps directly: 0 = loose/soft, 1 = infinitely stiff.
   * Formula: compliance = (1 - stiffness) / max(stiffness, epsilon) */
  const float epsilon = 1e-6f;
  float sEdge = std::clamp(settings.linStiff, 0.0f, 1.0f);
  float sShear = std::clamp(settings.shearStiff, 0.0f, 1.0f);
  float sBend = std::clamp(settings.angStiff, 0.0f, 1.0f);

  float edgeCompliance = (1.0f - sEdge) / std::max(sEdge, epsilon);
  float shearCompliance = (1.0f - sShear) / std::max(sShear, epsilon);
  float bendCompliance = (settings.bendingConstraints) ?
                       ((1.0f - sBend) / std::max(sBend, epsilon)) : 0.0f;

  /* LRA (Long Range Attachment): prevent cloth from stretching beyond rest length.
   * The LRA type is stored per-vertex in VertexAttributes and used by
   * CreateConstraints() to attach each dynamic vertex to the nearest kinematic
   * (pinned, mInvMass=0) vertex via a max-distance constraint.
   * EuclideanDistance: straight-line distance (faster, good for flat meshes).
   * GeodesicDistance:  distance along edges (accurate for curved meshes). */
  JPH::SoftBodySharedSettings::ELRAType lraType =
      JPH::SoftBodySharedSettings::ELRAType::None;
  if (settings.lraConstraints) {
    lraType = (settings.lraType == 1) ?
              JPH::SoftBodySharedSettings::ELRAType::GeodesicDistance :
              JPH::SoftBodySharedSettings::ELRAType::EuclideanDistance;
  }

  JPH::SoftBodySharedSettings::VertexAttributes vertexAttribs(
      edgeCompliance,
      shearCompliance,
      bendCompliance,
      lraType);

  /* Use Distance bend type: generates bend constraints across edges in the mesh.
   * Jolt's CreateConstraints has no hop-count parameter — EBendType::Distance
   * always spans a fixed 2-edge neighbourhood internally.
   * EBendType::None skips bend constraints entirely when the flag is off. */
  JPH::SoftBodySharedSettings::EBendType bendType =
      settings.bendingConstraints ?
      JPH::SoftBodySharedSettings::EBendType::Distance :
      JPH::SoftBodySharedSettings::EBendType::None;

  sbSettings->CreateConstraints(&vertexAttribs, 1, bendType);

  /* Optimize constraint groups for parallel XPBD execution. */
  sbSettings->Optimize();

  ConfigurePlasticity(settings, *sbSettings);

  /* Cache for replica cloning (reuse shared settings across spawned instances). */
  m_sharedSettings = sbSettings;
  m_replicaParams.friction        = settings.friction;
  m_replicaParams.damping         = settings.damping;
  m_replicaParams.restitution     = settings.restitution;
  m_replicaParams.pressure        = settings.pressure;
  m_replicaParams.margin          = settings.margin;
  m_replicaParams.gravityFactor   = settings.gravityFactor;
  m_replicaParams.numIterations   = settings.numIterations;
  m_replicaParams.facesDoubleSided = settings.facesDoubleSided;

  /* Store the initial world orientation for CloneIntoReplica.
   * This orientation is baked into the particle positions above. */
  m_initialWorldOri = worldOri;

  /* ---- Create body ---- */
  /* Place the body at spawn_position + local_bounds_center so mUpdatePosition
   * sees local bounds center (0,0,0) on the first step and leaves the body
   * position untouched — eliminating the first-frame origin jump. */
  JPH::Vec3 basePos = JoltMath::ToJolt(position);
  JPH::RVec3 posJolt(basePos.GetX() + particleBoundsCenter.GetX(),
                     basePos.GetY() + particleBoundsCenter.GetY(),
                     basePos.GetZ() + particleBoundsCenter.GetZ());

  JPH::SoftBodyCreationSettings creationSettings(
      sbSettings.GetPtr(),
      posJolt,
      JPH::Quat::sIdentity(),
      JoltMakeObjectLayer(m_ctrl->GetCollisionGroup(), m_ctrl->GetCollisionMask(), JOLT_BP_DYNAMIC));

  creationSettings.mFriction         = settings.friction;   /* kDF */
  creationSettings.mLinearDamping    = settings.damping;    /* kDP */
  creationSettings.mRestitution      = settings.restitution;
  creationSettings.mPressure         = settings.pressure;   /* kPR — gas pressure for closed shapes */
  creationSettings.mVertexRadius     = settings.margin;     /* collision proxy radius per vertex */
  creationSettings.mGravityFactor    = settings.gravityFactor;
  creationSettings.mNumIterations    = (settings.numIterations > 0) ? (JPH::uint32)settings.numIterations : 5;
  /* Double-sided: collide from both triangle winding directions.
   * Required for thin cloth/flags that can be hit from either side. */
  creationSettings.mFacesDoubleSided = settings.facesDoubleSided;
  /* mUpdatePosition: keep body position synced to soft-body local bounds center.
   * Must be true so SynchronizeMotionStates can track the body correctly. */
  creationSettings.mUpdatePosition = true;

  JPH::BodyInterface &bi = m_env->GetBodyInterface();
  JPH::Body *body = bi.CreateSoftBody(creationSettings);
  if (!body) {
    return false;
  }

  m_bodyID = body->GetID();

  body->SetCollisionGroup(JPH::CollisionGroup(
      m_env->GetConstraintGroupFilter(),
      (JPH::CollisionGroup::GroupID)m_ctrl->GetCollisionGroup(),
      m_bodyID.GetIndexAndSequenceNumber()));
  if (m_ctrl->GetNewClientInfo()) {
    body->SetUserData(reinterpret_cast<JPH::uint64>(m_ctrl->GetNewClientInfo()));
  }

  bi.AddBody(m_bodyID, JPH::EActivation::Activate);
  m_env->NotifySoftBodyBodyAdded();

  return true;
}

void JoltSoftBody::ApplySpawnTransform(const MT_Vector3 &posDelta, const MT_Matrix3x3 &newWorldOri)
{
  if (!m_env || m_bodyID.IsInvalid()) {
    return;
  }

  /* Convert from the currently baked world orientation to the requested one. */
  MT_Matrix3x3 invInitialOri = m_initialWorldOri.transposed();
  MT_Matrix3x3 rotationDelta = newWorldOri * invInitialOri;

  /* Compute old/new game-object origins around which we rigid-transform caches. */
  JPH::BodyInterface &bi = m_env->GetBodyInterfaceNoLock();
  JPH::RVec3 oldBodyPos = bi.GetPosition(m_bodyID);
  const MT_Vector3 oldOrigin = JoltMath::ToMT(JPH::Vec3(oldBodyPos)) - m_bodyOriginOffset;
  const MT_Vector3 newOrigin = oldOrigin + posDelta;

  /* Rotate runtime particle data (position + velocity) in local space. */
  const JPH::BodyLockInterface &lockIf =
      m_env->GetPhysicsSystem()->GetBodyLockInterfaceNoLock();
  JPH::BodyLockWrite lock(lockIf, m_bodyID);
  if (lock.Succeeded() && lock.GetBody().IsSoftBody()) {
    JPH::SoftBodyMotionProperties *mp =
        static_cast<JPH::SoftBodyMotionProperties *>(lock.GetBody().GetMotionProperties());
    for (JPH::SoftBodyVertex &v : mp->GetVertices()) {
      MT_Vector3 localPos = JoltMath::ToMT(v.mPosition);
      MT_Vector3 localVel = JoltMath::ToMT(v.mVelocity);

      localPos = rotationDelta * localPos;
      localVel = rotationDelta * localVel;

      v.mPosition = JoltMath::ToJolt(localPos);
      v.mVelocity = JoltMath::ToJolt(localVel);
    }
  }

  /* NOTE: Do NOT modify m_sharedSettings here.  After Optimize() + CreateSoftBody(),
   * the Jolt body holds a RefConst<SoftBodySharedSettings> pointing to the same
   * object.  Mutating vertex positions while the solver references them is undefined
   * behaviour and corrupts internal constraint data, causing crashes on exit or
   * during simulation.  Only the runtime particle data (SoftBodyVertex in
   * SoftBodyMotionProperties) needs to be rotated — which was done above. */

  /* Rotate cached COM->origin offset so motion-state sync stays coherent. */
  m_bodyOriginOffset = rotationDelta * m_bodyOriginOffset;

  /* Re-express the pin frame in the new spawned frame.
   * Pinned local offsets remain valid in that frame and do not need per-vertex updates. */
  m_pinInitialPos = newOrigin + rotationDelta * (m_pinInitialPos - oldOrigin);
  m_pinInitialOri = rotationDelta * m_pinInitialOri;

  /* Pin frame changed: drop cached previous transform so the next pinned update
   * is forced to refresh against the new spawn-space pin basis. */
  m_hasLastPinTransform = false;

  /* This orientation is now baked into local particle coordinates. */
  m_initialWorldOri = newWorldOri;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Update Mesh and Pinned Vertices
 * \{ */

bool JoltSoftBody::NeedsPlasticityUpdate(bool hasContactVertices) const
{
  if (!m_plasticityEnabled || m_originalEdgeRestLengths.empty()) {
    return false;
  }
  if (m_plasticStrength > 0.0f && hasContactVertices) {
    return true;
  }
  return NeedsPlasticityRepairWakeUp();
}

bool JoltSoftBody::NeedsPlasticityRepairWakeUp() const
{
  return m_plasticityEnabled && !m_originalEdgeRestLengths.empty() &&
         m_plasticRepairRate > 0.0f && m_hasPlasticDeformation;
}

bool JoltSoftBody::UsesContactPlasticity() const
{
  return m_plasticityEnabled && m_plasticStrength > 0.0f;
}

void JoltSoftBody::ApplyPlasticityLocked(
    JPH::Body &body,
    float timeStep,
    const std::vector<JPH::uint32> *contactVertexIndices)
{
  const bool hasContactVertices = contactVertexIndices && !contactVertexIndices->empty();
  const bool bodyIsActive = body.IsActive();
  if (!NeedsPlasticityUpdate(hasContactVertices) || !m_sharedSettings || !body.IsSoftBody()) {
    return;
  }

  const bool plasticAllowed = m_plasticStrength > 0.0f && hasContactVertices;
  const float repairStep = (bodyIsActive && m_hasPlasticDeformation) ?
                               std::clamp(m_plasticRepairRate * std::max(timeStep, 0.0f),
                                          0.0f,
                                          1.0f) :
                               0.0f;
  if (!plasticAllowed && repairStep <= 0.0f) {
    return;
  }

  JPH::SoftBodyMotionProperties *motionProperties =
      static_cast<JPH::SoftBodyMotionProperties *>(body.GetMotionProperties());
  if (!motionProperties) {
    return;
  }

  const auto &vertices = motionProperties->GetVertices();
  auto &edges = m_sharedSettings->mEdgeConstraints;
  if (m_originalEdgeRestLengths.size() != edges.size()) {
    return;
  }

  if (hasContactVertices) {
    if (m_plasticDirtyVertices.size() != vertices.size()) {
      m_plasticDirtyVertices.assign(vertices.size(), 0);
    }
    else {
      std::fill(m_plasticDirtyVertices.begin(), m_plasticDirtyVertices.end(), 0);
    }

    for (JPH::uint32 vertexIndex : *contactVertexIndices) {
      if (vertexIndex < m_plasticDirtyVertices.size()) {
        m_plasticDirtyVertices[vertexIndex] = 1;
      }
    }
  }

  constexpr float kMinRestLength = 1.0e-6f;
  constexpr float kRestLengthEpsilon = 1.0e-4f;
  bool hasPlasticDeformation = false;
  for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
    JPH::SoftBodySharedSettings::Edge &edge = edges[edgeIndex];
    const JPH::uint32 vertex0 = edge.mVertex[0];
    const JPH::uint32 vertex1 = edge.mVertex[1];
    if (vertex0 >= vertices.size() || vertex1 >= vertices.size()) {
      continue;
    }

    const float originalRestLength = m_originalEdgeRestLengths[edgeIndex];
    if (originalRestLength <= kMinRestLength) {
      continue;
    }

    bool plasticUpdated = false;
    if (plasticAllowed) {
      const bool edgeInContactZone = vertex0 < m_plasticDirtyVertices.size() &&
                                     vertex1 < m_plasticDirtyVertices.size() &&
                                     (m_plasticDirtyVertices[vertex0] ||
                                      m_plasticDirtyVertices[vertex1]);

      if (edgeInContactZone) {
        const float currentRestLength = std::max(edge.mRestLength, kMinRestLength);
        const float currentLength =
            std::max((vertices[vertex1].mPosition - vertices[vertex0].mPosition).Length(),
                     kMinRestLength);
        const float strain = std::abs(currentLength - edge.mRestLength) / currentRestLength;

        if (strain > m_plasticThreshold) {
          float targetRestLength = edge.mRestLength +
                                   (currentLength - edge.mRestLength) * m_plasticStrength;
          if (m_plasticMaxDeform > 0.0f) {
            const float minRestLength = originalRestLength * std::max(0.0f,
                                                                      1.0f - m_plasticMaxDeform);
            const float maxRestLength = originalRestLength * (1.0f + m_plasticMaxDeform);
            targetRestLength = std::clamp(targetRestLength,
                                          std::max(minRestLength, kMinRestLength),
                                          std::max(maxRestLength, kMinRestLength));
          }
          edge.mRestLength = targetRestLength;
          plasticUpdated = true;
        }
      }
    }

    if (!plasticUpdated && repairStep > 0.0f) {
      edge.mRestLength += (originalRestLength - edge.mRestLength) * repairStep;
      const float snapEpsilon = std::max(originalRestLength * kRestLengthEpsilon,
                                         kMinRestLength);
      if (std::abs(edge.mRestLength - originalRestLength) <= snapEpsilon) {
        edge.mRestLength = originalRestLength;
      }
    }

    const float deformationEpsilon = std::max(originalRestLength * kRestLengthEpsilon,
                                             kMinRestLength);
    if (std::abs(edge.mRestLength - originalRestLength) > deformationEpsilon) {
      hasPlasticDeformation = true;
    }
  }
  m_hasPlasticDeformation = hasPlasticDeformation;
}

bool JoltSoftBody::PreparePinTransformFollowBodyMove(const MT_Vector3 &pinPos,
                                                     const MT_Matrix3x3 &pinOri,
                                                     const JPH::RVec3 &currentBodyPos,
                                                     JPH::RVec3 &r_targetBodyPos) const
{
  if (!m_pinTransformFollow || !m_hasLastPinTransform) {
    return false;
  }

  constexpr float kPinPosEpsilonSq = 1.0e-12f;
  const MT_Vector3 pinPosDelta = pinPos - m_lastPinPos;
  const MT_Matrix3x3 pinOriDelta = pinOri * m_lastPinOri.transposed();
  if (pinPosDelta.length2() <= kPinPosEpsilonSq && IsNearlyIdentityRotation(pinOriDelta)) {
    return false;
  }

  const MT_Matrix3x3 invLastPinOri = m_lastPinOri.transposed();
  const MT_Vector3 bodyWorldPos = JoltMath::ToMT(JPH::Vec3(currentBodyPos));
  const MT_Vector3 bodyPinLocal = invLastPinOri * (bodyWorldPos - m_lastPinPos);
  const MT_Vector3 targetBodyPos = pinPos + pinOri * bodyPinLocal;
  r_targetBodyPos = JoltMath::ToJoltR(targetBodyPos);
  return true;
}

void JoltSoftBody::UpdatePinnedVerticesLocked(const MT_Vector3 &pinPos,
                                              const MT_Matrix3x3 &pinOri,
                                              JPH::Body &body,
                                              const JPH::RVec3 *previousBodyWorldPos)
{
  if (m_pinnedData.empty() || !m_env || !body.IsSoftBody()) {
    return;
  }

  /* Conservative early-out for idle pinned setups:
   * - if the pin transform is unchanged from the previous frame; and
   * - the soft body is sleeping,
   * then re-writing pinned vertices is redundant. */
  constexpr float kPinPosEpsilonSq = 1.0e-12f;
  const MT_Vector3 pinPosDelta = pinPos - m_lastPinPos;
  const MT_Matrix3x3 pinOriDelta = pinOri * m_lastPinOri.transposed();
  const bool pinPosUnchanged = !m_hasLastPinTransform ||
                               pinPosDelta.length2() <= kPinPosEpsilonSq;
  const bool pinOriUnchanged = !m_hasLastPinTransform || IsNearlyIdentityRotation(pinOriDelta);

  if (m_hasLastPinTransform && pinPosUnchanged && pinOriUnchanged && !body.IsActive()) {
    return;
  }

  /* mPosition in SoftBodyMotionProperties is body-local (relative to body's
   * current world position, i.e. GetPosition()).  To pin a vertex at a target
   * Blender world position we must convert:
   *   joltLocal = joltWorld(target) - body.GetPosition() */
  /* Body world position in Jolt Y-up space (used to convert world→local). */
  JPH::RVec3 bodyWorldPos = body.GetPosition();
  JPH::RVec3 sourceBodyWorldPos = previousBodyWorldPos ? *previousBodyWorldPos :
                                                         bodyWorldPos;

  JPH::SoftBodyMotionProperties *mp =
      static_cast<JPH::SoftBodyMotionProperties *>(body.GetMotionProperties());
  if (!mp) {
    return;
  }

  auto &joltVerts = mp->GetVertices();

  if (m_pinTransformFollow && m_hasLastPinTransform &&
      (!pinPosUnchanged || !pinOriUnchanged)) {
    const MT_Matrix3x3 invLastPinOri = m_lastPinOri.transposed();

    for (JPH::SoftBodyVertex &vertex : joltVerts) {
      const MT_Vector3 oldWorldPos = JoltMath::ToMT(JPH::Vec3(sourceBodyWorldPos) +
                                                    vertex.mPosition);
      const MT_Vector3 pinLocalPos = invLastPinOri * (oldWorldPos - m_lastPinPos);
      const MT_Vector3 newWorldPos = pinPos + pinOri * pinLocalPos;
      const JPH::Vec3 newLocalPos = JoltMath::ToJolt(newWorldPos) - JPH::Vec3(bodyWorldPos);

      vertex.mPosition = newLocalPos;
      vertex.mPreviousPosition = newLocalPos;
      vertex.mVelocity = JPH::Vec3::sZero();
    }
  }

  for (const auto &[joltIdx, pinLocalOffset] : m_pinnedData) {
    if (joltIdx < 0 || joltIdx >= (int)joltVerts.size()) {
      continue;
    }

    /* Compute target Blender world position following the pin object. */
    MT_Vector3 newWorldPos = pinPos + pinOri * pinLocalOffset;

    /* Convert Blender Z-up world → Jolt Y-up world → body local. */
    JPH::Vec3 joltWorld(static_cast<float>(newWorldPos.x()),
                        static_cast<float>(newWorldPos.z()),
                        -static_cast<float>(newWorldPos.y()));
    joltVerts[joltIdx].mPosition = joltWorld - JPH::Vec3(bodyWorldPos);
    joltVerts[joltIdx].mPreviousPosition = joltVerts[joltIdx].mPosition;
    /* Keep body-local velocity at zero for pinned vertices.  Their world-space
     * motion comes from the soft body's own body velocity (which tracks the pin
     * object).  Any stale per-particle velocity would create artificial forces
     * on neighbouring unpinned vertices through edge/volume constraints and
     * cause visible jiggling, especially with CCD-enabled pin bodies. */
    joltVerts[joltIdx].mVelocity = JPH::Vec3::sZero();
  }

  m_lastPinPos = pinPos;
  m_lastPinOri = pinOri;
  m_hasLastPinTransform = true;
}

void JoltSoftBody::UpdatePinnedVertices(const MT_Vector3 &pinPos, const MT_Matrix3x3 &pinOri)
{
  if (m_pinnedData.empty() || m_bodyID.IsInvalid() || !m_env) {
    return;
  }

  /* NoLock interface is correct here because this runs before
   * PhysicsSystem::Update() (no parallel jobs active). */
  const JPH::BodyLockInterface &lockIf =
      m_env->GetPhysicsSystem()->GetBodyLockInterfaceNoLock();
  JPH::BodyLockWrite lock(lockIf, m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }

  UpdatePinnedVerticesLocked(pinPos, pinOri, lock.GetBody());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Update Mesh
 * \{ */

void JoltSoftBody::UpdateMesh(blender::Depsgraph *depsgraph)
{
  if (m_bodyID.IsInvalid() || !m_env) {
    return;
  }

  const JPH::BodyLockInterface &lockInterface =
      m_env->GetPhysicsSystem()->GetBodyLockInterfaceNoLock();
  JPH::BodyLockRead lock(lockInterface, m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }

  UpdateMeshLocked(depsgraph, lock.GetBody());
}

void JoltSoftBody::UpdateMeshLocked(blender::Depsgraph *depsgraph, const JPH::Body &body)
{
  /* Hidden-layer template objects must NOT write to the shared Blender modifier;
   * doing so would corrupt active replicas that share the same blender::Object*. */
  if (!m_isActive) {
    return;
  }
  if (!m_gameobj || !m_meshObject || !m_env || !body.IsSoftBody()) {
    return;
  }

  /* Sleeping soft bodies without pinned vertices have stable particle positions.
   * Skip expensive mesh/depsgraph work until they wake up again. */
  if (!body.IsActive() && m_pinnedData.empty() && m_hasMeshUpload) {
    return;
  }

  if (!depsgraph) {
    return;
  }

  /* 1. Get the evaluated Blender object for modifier and mesh access. */
  blender::Object *ob = m_gameobj->GetBlenderObject();
  bool subsurf_changed = false;
  const bool ensured_render_subsurf = JoltEnsureSoftBodyRenderSubsurfLevels(
      ob, m_subsurfLevelOverrides, subsurf_changed);
  if (ensured_render_subsurf) {
    m_useRenderSubsurfControl = true;
  }
  const bool use_render_subsurf = m_useRenderSubsurfControl;
  if (subsurf_changed) {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
    KX_KetsjiEngine *engine = KX_GetActiveEngine();
    if (engine && engine->GetContext()) {
      BKE_scene_graph_update_tagged(depsgraph, CTX_data_main(engine->GetContext()));
    }
  }

  blender::Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
  if (!ob_eval) {
    return;
  }
  blender::Mesh *me = (blender::Mesh *)ob_eval->data;
  if (!me) {
    return;
  }

  const int visual_verts = me->verts_num;

  /* Sanity: skip if the final display mesh changed vertex count at runtime. */
  if (m_numVisualVerts > 0 && visual_verts != m_numVisualVerts) {
    return;
  }

  /* 2. First time: create the deform modifier and allocate the coord buffer. */
  if (!m_sbModifier) {
    blender::Mesh *deform_me = use_render_subsurf ? (blender::Mesh *)ob->data : me;
    m_numVisualVerts = visual_verts;
    m_numBlenderVerts = deform_me ? deform_me->verts_num : 0;
    if (m_numBlenderVerts <= 0) {
      return;
    }

    m_sbModifier = (blender::SimpleDeformModifierDataBGE *)BKE_modifier_new(
        eModifierType_SimpleDeformBGE);
    STRNCPY(m_sbModifier->modifier.name, "joltSbModifier");
    JoltInsertSoftBodyModifier(ob, m_sbModifier, use_render_subsurf);
    BKE_modifier_unique_name(&ob->modifiers, (ModifierData *)m_sbModifier);
    BKE_modifiers_persistent_uid_init(*ob, m_sbModifier->modifier);
    m_env->RequestSoftBodyRelationsTagUpdate();

    m_sbCoords = (float(*)[3])MEM_new_zeroed(
        sizeof(float[3]) * (size_t)m_numBlenderVerts, "JoltSoftBody.sbCoords");
    m_hasMeshUpload = false;

    /* Pre-fill from the rest-pose vertex positions so that vertices not covered
     * by the physics mesh (e.g. isolated verts) keep their original location. */
    const blender::Span<blender::float3> restPos = deform_me->vert_positions();
    for (int v = 0; v < m_numBlenderVerts && v < (int)restPos.size(); ++v) {
      m_sbCoords[v][0] = restPos[v][0];
      m_sbCoords[v][1] = restPos[v][1];
      m_sbCoords[v][2] = restPos[v][2];
    }
  }

  /* 3. Read soft body local positions and apply directly to modifier buffer.
   * Jolt soft body vertex mPosition is already in LOCAL space of the body!
   * We just need to apply the object scale (if any) and convert coordinates
   * from Jolt (X,Y,Z) -> Blender (X,-Z,Y). */
  const JPH::SoftBodyMotionProperties *mp =
      static_cast<const JPH::SoftBodyMotionProperties *>(body.GetMotionProperties());
  if (!mp) {
    return;
  }
  const JPH::Array<JPH::SoftBodyVertex> &joltVerts = mp->GetVertices();

  /* Body world position in Blender Z-up (Jolt Y-up → Blender Z-up: X,Y,Z → X,-Z,Y). */
  JPH::RVec3 bodyJoltPos = body.GetPosition();
  const float bodyBx = (float)bodyJoltPos.GetX();
  const float bodyBy = -(float)bodyJoltPos.GetZ();
  const float bodyBz =  (float)bodyJoltPos.GetY();

  /* Determine the world matrix Blender will use when rendering A.
   * UpdateParents runs AFTER UpdateMesh, so a parented node has not been
   * propagated yet.  Pre-compute the expected render matrix:
   *   parent_world * A_local
   * so that modifier coords land in A's true local space and are not
   * double-transformed when UpdateParents later applies the parent rotation.
   *
    * For unparented soft bodies this reduces to the object's pending local
    * transform.  Using cached world values here can be one frame stale because
    * physics sync marks the node modified before UpdateSoftBodies runs. */
  SG_Node *sgNode = m_gameobj->GetSGNode();
  const SG_Node *sgParent = sgNode->GetSGParent();

  MT_Vector3 renderPos;
  MT_Matrix3x3 renderInvOri;
  float invSx, invSy, invSz;
  bool useNoRotationFastPath = false;

  if (sgParent) {
    /* parent_world * A_local gives the transform UpdateParents will compute. */
    MT_Transform combined = sgParent->GetWorldTransform() * sgNode->GetLocalTransform();
    MT_Matrix4x4 tmat(combined.toMatrix());
    /* Extract column lengths for scale (same method as KX_NormalParentRelation). */
    float sx = MT_Vector3(tmat.getElement(0, 0), tmat.getElement(1, 0), tmat.getElement(2, 0)).length();
    float sy = MT_Vector3(tmat.getElement(0, 1), tmat.getElement(1, 1), tmat.getElement(2, 1)).length();
    float sz = MT_Vector3(tmat.getElement(0, 2), tmat.getElement(1, 2), tmat.getElement(2, 2)).length();
    invSx = (sx > 1e-6f) ? 1.0f / sx : 1.0f;
    invSy = (sy > 1e-6f) ? 1.0f / sy : 1.0f;
    invSz = (sz > 1e-6f) ? 1.0f / sz : 1.0f;
    renderPos = combined.getOrigin();
    /* Pure rotation = basis with scale removed; inverse rotation = transpose. */
    renderInvOri = combined.getBasis().scaled(invSx, invSy, invSz).transposed();
  }
  else {
    renderPos = m_gameobj->NodeGetLocalPosition();
    MT_Vector3 scale = m_gameobj->NodeGetLocalScaling();
    invSx = (std::abs(scale.x()) > 1e-6f) ? 1.0f / scale.x() : 1.0f;
    invSy = (std::abs(scale.y()) > 1e-6f) ? 1.0f / scale.y() : 1.0f;
    invSz = (std::abs(scale.z()) > 1e-6f) ? 1.0f / scale.z() : 1.0f;

    const MT_Matrix3x3 localOri = m_gameobj->NodeGetLocalOrientation();
    renderInvOri = localOri.transposed();

    /* Hot path for most runtime-spawned soft bodies: unparented with no rotation.
     * Skip per-vertex matrix multiply and go directly world→local with scale only. */
    useNoRotationFastPath = IsNearlyIdentityRotation(localOri);
  }

  const float renderPosX = (float)renderPos.x();
  const float renderPosY = (float)renderPos.y();
  const float renderPosZ = (float)renderPos.z();

  const float changeEpsilon = 1.0e-6f;
  bool coordsChanged = false;

  const int remapSize = (int)m_blenderVertToJoltVert.size();
  const int loopCount = std::min(m_numBlenderVerts, remapSize);
  for (int blVert = 0; blVert < loopCount; ++blVert) {
    int joltIdx = m_blenderVertToJoltVert[blVert];
    if (joltIdx < 0 || joltIdx >= (int)joltVerts.size()) {
      continue;
    }

    const JPH::Vec3 &joltPos = joltVerts[joltIdx].mPosition;

    float newX;
    float newY;
    float newZ;

    if (useNoRotationFastPath) {
      newX = (bodyBx + joltPos.GetX() - renderPosX) * invSx;
      newY = (bodyBy - joltPos.GetZ() - renderPosY) * invSy;
      newZ = (bodyBz + joltPos.GetY() - renderPosZ) * invSz;
    }
    else {
      /* Particle world position in Blender Z-up: body COM + body-local offset. */
      float wx = bodyBx + joltPos.GetX();
      float wy = bodyBy + (-joltPos.GetZ());
      float wz = bodyBz + joltPos.GetY();

      /* World → A's local space: inv(R) * (world − origin) / scale. */
      MT_Vector3 offset((double)(wx - renderPosX),
                        (double)(wy - renderPosY),
                        (double)(wz - renderPosZ));
      MT_Vector3 local = renderInvOri * offset;
      newX = (float)(local.x() * invSx);
      newY = (float)(local.y() * invSy);
      newZ = (float)(local.z() * invSz);
    }

    if (std::abs(m_sbCoords[blVert][0] - newX) > changeEpsilon ||
        std::abs(m_sbCoords[blVert][1] - newY) > changeEpsilon ||
        std::abs(m_sbCoords[blVert][2] - newZ) > changeEpsilon) {
      m_sbCoords[blVert][0] = newX;
      m_sbCoords[blVert][1] = newY;
      m_sbCoords[blVert][2] = newZ;
      coordsChanged = true;
    }
  }

  /* 4. Hand the coord buffer to the modifier and tag only when needed. */
  const bool needsUpload = !m_hasMeshUpload ||
                           (m_sbModifier->vertcoos != m_sbCoords) ||
                           coordsChanged;
  if (needsUpload) {
    m_sbModifier->vertcoos = m_sbCoords;
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
    m_hasMeshUpload = true;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Cleanup
 * \{ */

void JoltSoftBody::CleanupModifier(blender::Object *ob)
{
  if (m_sbModifier && ob) {
    /* Null the coord pointer first so any concurrent deform_verts evaluation
     * sees nullptr and returns early instead of reading freed memory. */
    m_sbModifier->vertcoos = nullptr;
    BLI_remlink(&ob->modifiers, m_sbModifier);
    BKE_modifier_free((ModifierData *)m_sbModifier);
    m_sbModifier = nullptr;

    /* Invalidate the depsgraph so the embedded Blender session does not keep
     * a cached evaluated mesh that still contains the soft-body deformation.
     * Without this, the second game run reads the old deformed rest-pose and
     * produces stretched/wrong vertex positions from the very first frame. */
    KX_KetsjiEngine *engine = KX_GetActiveEngine();
    if (engine) {
      blender::bContext *C = engine->GetContext();
      if (C) {
        DEG_relations_tag_update(CTX_data_main(C));
      }
    }
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }
  JoltRestoreSoftBodyRenderSubsurfLevels(ob, m_subsurfLevelOverrides);
  /* Free coord buffer only after the modifier is gone. */
  if (m_sbCoords) {
    MEM_delete_void(static_cast<void *>(m_sbCoords));
    m_sbCoords = nullptr;
  }
  m_numBlenderVerts = 0;
  m_numVisualVerts = 0;
  m_useRenderSubsurfControl = false;
  m_hasMeshUpload = false;
}

void JoltSoftBody::PurgeStaleModifiers()
{
  if (!m_gameobj) {
    return;
  }
  blender::Object *ob = m_gameobj->GetBlenderObject();
  if (!ob) {
    return;
  }
  /* Remove any SimpleDeformBGE modifiers left over from a previous game run
   * (e.g. the game crashed or the .blend was saved while the game was active).
   * This runs exactly once per template at game startup, before any clones
   * exist, so it cannot accidentally destroy an active clone's modifier. */
  ModifierData *md = (ModifierData *)ob->modifiers.first;
  while (md) {
    ModifierData *next = md->next;
    if (JoltIsSoftBodyModifier(md)) {
      ((blender::SimpleDeformModifierDataBGE *)md)->vertcoos = nullptr;
      BLI_remlink(&ob->modifiers, md);
      BKE_modifier_free(md);
    }
    md = next;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Replica Cloning
 * \{ */

JoltSoftBody *JoltSoftBody::CloneIntoReplica(JoltPhysicsController *newCtrl,
                                              PHY_IMotionState *motionState,
                                              PHY_IPhysicsController *parentCtrl)
{
  if (!m_sharedSettings || !m_env || !newCtrl || !motionState) {
    return nullptr;
  }

  MT_Vector3 replicaPos = motionState->GetWorldPosition();
  MT_Matrix3x3 replicaOri = motionState->GetWorldOrientation();

  /* Compute the new pin transform from the replica's parent controller. */
  MT_Vector3   newPinPos    = m_pinInitialPos;
  MT_Matrix3x3 newPinOri    = m_pinInitialOri;
  bool         hasPinTransform = false;

  if (parentCtrl) {
    JoltPhysicsController *joltParent =
        static_cast<JoltPhysicsController *>(parentCtrl);
    PHY_IMotionState *pms = joltParent->GetMotionState();
    if (pms) {
      newPinPos       = pms->GetWorldPosition();
      newPinOri       = pms->GetWorldOrientation();
      hasPinTransform = true;
    }
  }

  /* ---- Rotation delta for non-pinned particles ----
   * The template's particle positions have m_initialWorldOri baked into them.
   * When spawning with a rotated parent, we need to rotate non-pinned particles
   * to match the replica's new orientation.
   *
   * For soft bodies pinned to a parent rigid body:
   *   - The pin object IS the parent rigid body
   *   - Rotation delta = newPinOri * m_pinInitialOri.inverse()
   *   - This transforms particles from template's pin frame to replica's pin frame
   *
   * For standalone soft bodies (no pin):
   *   - Use the replica's own world orientation
   *   - Rotation delta = replicaOri * m_initialWorldOri.inverse()
   */
  MT_Matrix3x3 rotationDelta;
  bool needsRotation = false;

  if (hasPinTransform) {
    /* Soft body is pinned to a parent. Use pin orientation for rotation delta. */
    MT_Matrix3x3 invInitialOri = m_pinInitialOri.transposed();
    rotationDelta = newPinOri * invInitialOri;
    needsRotation = !IsNearlyIdentityRotation(rotationDelta);
  }
  else if (m_pinnedData.empty()) {
    /* No pinned vertices - use replica's own orientation. */
    MT_Matrix3x3 invInitialOri = m_initialWorldOri.transposed();
    rotationDelta = replicaOri * invInitialOri;
    needsRotation = !IsNearlyIdentityRotation(rotationDelta);
  }

  /* Dense pinned mask: avoids per-spawn hash allocations/lookups. */
  std::vector<char> pinnedMask;
  if (needsRotation && !m_pinnedData.empty()) {
    const size_t vertCount = m_sharedSettings->mVertices.size();
    pinnedMask.assign(vertCount, 0);
    for (const auto &[joltIdx, pinLocalOffset] : m_pinnedData) {
      (void)pinLocalOffset;
      if (joltIdx >= 0 && joltIdx < (int)vertCount) {
        pinnedMask[(size_t)joltIdx] = 1;
      }
    }
  }

  const bool useRotationBake = needsRotation && m_pinnedData.empty();

  /* Mixed pinned/non-pinned rotations need custom shared settings.
   * For unpinned rotated replicas we use SoftBodyCreationSettings rotation baking
   * and keep sharing the template settings. */
  JPH::Ref<JPH::SoftBodySharedSettings> replicaSettings;
  JPH::Vec3 replicaBoundsCenterCorrection(0.0f, 0.0f, 0.0f);
  if (needsRotation && !pinnedMask.empty()) {
    const uint64_t rotationKey = MakeRotationCacheKey(rotationDelta);
    auto cacheIt = m_rotatedPinnedSettingsCache.find(rotationKey);
    if (cacheIt == m_rotatedPinnedSettingsCache.end()) {
      RotatedPinnedSettingsCacheEntry cacheEntry;
      /* Clone the template settings to preserve Jolt's internal optimization data
       * (e.g. update groups / skinned normals) used by the solver. Rebuilding
       * from public arrays can leave those private caches invalid and crash in
       * rotated mixed pinned/non-pinned spawn paths. */
      cacheEntry.settings = m_sharedSettings->Clone();

      const JPH::Array<JPH::SoftBodySharedSettings::Vertex> &srcVerts =
          m_sharedSettings->mVertices;
      JPH::Array<JPH::SoftBodySharedSettings::Vertex> &dstVerts =
          cacheEntry.settings->mVertices;

      for (size_t i = 0; i < srcVerts.size(); ++i) {
        if (pinnedMask[i] != 0) {
          /* Pinned vertex: keep position/velocity as-is (UpdatePinnedVertices will
           * place it from pin local offsets just before simulation). */
          continue;
        }

        /* Non-pinned vertex: rotate to match replica's orientation.
         * Position/velocity are in Jolt Y-up local space (relative to body COM).
         * Convert to Blender Z-up, apply rotation, convert back to Jolt Y-up. */
        const JPH::Float3 &jp = srcVerts[i].mPosition;
        MT_Vector3 blenderLocal(jp[0], -jp[2], jp[1]);
        MT_Vector3 rotated = rotationDelta * blenderLocal;
        dstVerts[i].mPosition = JPH::Float3(
            static_cast<float>(rotated.x()),
            static_cast<float>(rotated.z()),
            -static_cast<float>(rotated.y()));

        const JPH::Float3 &jv = srcVerts[i].mVelocity;
        MT_Vector3 blenderVelocity(jv[0], -jv[2], jv[1]);
        MT_Vector3 rotatedVelocity = rotationDelta * blenderVelocity;
        dstVerts[i].mVelocity = JPH::Float3(
            static_cast<float>(rotatedVelocity.x()),
            static_cast<float>(rotatedVelocity.z()),
            -static_cast<float>(rotatedVelocity.y()));
      }

      cacheEntry.boundsCenterCorrection = CalculateBoundsCenter(dstVerts);
      RecenterVertices(dstVerts, cacheEntry.boundsCenterCorrection);

      cacheIt = m_rotatedPinnedSettingsCache.emplace(rotationKey, std::move(cacheEntry)).first;
    }

    replicaSettings = cacheIt->second.settings;
    replicaBoundsCenterCorrection = cacheIt->second.boundsCenterCorrection;
  }
  else {
    /* No mixed pinning rotation path required.
     * For unpinned rotated replicas we reuse shared settings and let
     * SoftBodyCreationSettings bake the rotation into local vertices. */
    replicaSettings = m_sharedSettings;
    if (useRotationBake) {
      const uint64_t rotationKey = MakeRotationCacheKey(rotationDelta);
      auto correctionIt = m_rotatedSharedBoundsCenterCache.find(rotationKey);
      if (correctionIt == m_rotatedSharedBoundsCenterCache.end()) {
        replicaBoundsCenterCorrection = CalculateRotatedBoundsCenter(
            m_sharedSettings->mVertices, rotationDelta);
        correctionIt = m_rotatedSharedBoundsCenterCache.emplace(
            rotationKey, replicaBoundsCenterCorrection).first;
      }
      else {
        replicaBoundsCenterCorrection = correctionIt->second;
      }
    }
  }

  if (m_plasticityEnabled) {
    replicaSettings = replicaSettings->Clone();
    if (replicaSettings->mEdgeConstraints.size() == m_sharedSettings->mEdgeConstraints.size()) {
      const size_t edgeCount = replicaSettings->mEdgeConstraints.size();
      for (size_t edgeIndex = 0; edgeIndex < edgeCount; ++edgeIndex) {
        replicaSettings->mEdgeConstraints[edgeIndex].mRestLength =
            m_sharedSettings->mEdgeConstraints[edgeIndex].mRestLength;
      }
    }
  }

  /* Preserve the template's COM-to-origin offset for this replica.
   * For rotated replicas the offset rotates with the spawn orientation delta. */
  MT_Vector3 replicaOriginOffset = m_bodyOriginOffset;
  if (needsRotation) {
    replicaOriginOffset = rotationDelta * replicaOriginOffset;
  }
  JPH::Vec3 replicaSpawnOffset = JoltMath::ToJolt(replicaOriginOffset);
  replicaSpawnOffset += replicaBoundsCenterCorrection;

  /* Create a new Jolt soft body with the (possibly rotated) settings. */
  JPH::Vec3 baseReplicaPos = JoltMath::ToJolt(replicaPos);
  JPH::RVec3 posJolt(baseReplicaPos.GetX() + replicaSpawnOffset.GetX(),
                     baseReplicaPos.GetY() + replicaSpawnOffset.GetY(),
                     baseReplicaPos.GetZ() + replicaSpawnOffset.GetZ());

  JPH::Quat replicaRotation = JPH::Quat::sIdentity();
  if (useRotationBake) {
    MT_Quaternion rotationDeltaQuat = rotationDelta.getRotation();
    const float qLen = (float)rotationDeltaQuat.length();
    if (qLen > 1.0e-12f) {
      rotationDeltaQuat /= qLen;
    }
    else {
      rotationDeltaQuat = MT_Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
    }
    replicaRotation = JoltMath::ToJolt(rotationDeltaQuat);
  }

  JPH::SoftBodyCreationSettings cs(
      replicaSettings.GetPtr(),
      posJolt,
      replicaRotation,
      JoltMakeObjectLayer(
          newCtrl->GetCollisionGroup(), newCtrl->GetCollisionMask(), JOLT_BP_DYNAMIC));

  cs.mFriction         = m_replicaParams.friction;
  cs.mLinearDamping    = m_replicaParams.damping;
  cs.mRestitution      = m_replicaParams.restitution;
  cs.mPressure         = m_replicaParams.pressure;
  cs.mVertexRadius     = m_replicaParams.margin;
  cs.mGravityFactor    = m_replicaParams.gravityFactor;
  cs.mNumIterations    = (JPH::uint32)m_replicaParams.numIterations;
  cs.mFacesDoubleSided = m_replicaParams.facesDoubleSided;
  cs.mUpdatePosition   = true;
  cs.mMakeRotationIdentity = true;

  JPH::BodyInterface &bi = m_env->GetBodyInterfaceNoLock();
  JPH::Body *body = bi.CreateSoftBody(cs);
  if (!body) {
    return nullptr;
  }

  body->SetCollisionGroup(JPH::CollisionGroup(
      m_env->GetConstraintGroupFilter(),
      (JPH::CollisionGroup::GroupID)newCtrl->GetCollisionGroup(),
      body->GetID().GetIndexAndSequenceNumber()));
  if (newCtrl->GetNewClientInfo()) {
    body->SetUserData(reinterpret_cast<JPH::uint64>(newCtrl->GetNewClientInfo()));
  }

  JoltSoftBody *clone        = new JoltSoftBody(m_env, newCtrl);
  clone->m_bodyID            = body->GetID();
  clone->m_sharedSettings    = replicaSettings;
  clone->m_replicaParams     = m_replicaParams;
  clone->m_blenderVertToJoltVert = m_blenderVertToJoltVert;
  clone->m_noPinCollision    = m_noPinCollision;
  clone->m_pinTransformFollow = m_pinTransformFollow;
  clone->m_pinBlenderObject  = m_pinBlenderObject;
  clone->m_meshObject        = m_meshObject;
  clone->m_isActive          = true;
  clone->m_useRenderSubsurfControl = m_useRenderSubsurfControl;
  clone->m_initialWorldOri   = hasPinTransform ? newPinOri : replicaOri;
  clone->m_bodyOriginOffset  = JoltMath::ToMT(replicaSpawnOffset);
  clone->m_plasticityEnabled = m_plasticityEnabled;
  clone->m_plasticThreshold = m_plasticThreshold;
  clone->m_plasticStrength = m_plasticStrength;
  clone->m_plasticMaxDeform = m_plasticMaxDeform;
  clone->m_plasticRepairRate = m_plasticRepairRate;
  clone->m_hasPlasticDeformation = m_hasPlasticDeformation;
  clone->m_originalEdgeRestLengths = m_originalEdgeRestLengths;
  if (clone->m_plasticityEnabled) {
    clone->m_plasticDirtyVertices.assign(replicaSettings->mVertices.size(), 0);
  }

  /* Queue body insertion so runtime spawn bursts can be added in one Jolt batch
   * (AddBodiesPrepare/AddBodiesFinalize) before the next physics step. */
  m_env->QueueSoftBodyBodyAdd(clone->m_bodyID);

  /* Copy pinned local offsets in the pin frame. */
  if (!m_pinnedData.empty()) {
    clone->m_pinnedData = m_pinnedData;
    if (hasPinTransform) {
      /* Wire the replica's pin controller so UpdatePinnedVertices() works. */
      clone->m_pinCtrl = static_cast<JoltPhysicsController *>(parentCtrl);
    }
    clone->m_pinInitialPos = newPinPos;
    clone->m_pinInitialOri = newPinOri;

    /* Do not lock/update the freshly created body here: runtime replicas are
     * queued and only added to the world in FlushPendingSoftBodyBodyAdds().
     * The first pinned update is performed in ProceedDeltaTime() after add,
     * before simulation, which keeps spawn safe and deterministic. */
  }

  return clone;
}

/** \} */

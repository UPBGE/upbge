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
#include <unordered_set>
#include "MT_Transform.h"
#include "RAS_IDisplayArray.h"
#include "SG_Node.h"
#include "PHY_IMotionState.h"
#include "PHY_IPhysicsController.h"
#include "RAS_MeshObject.h"

#include "MEM_guardedalloc.h"

#include <cfloat>

using namespace blender;

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Construction / Destruction
 * \{ */

JoltSoftBody::JoltSoftBody(JoltPhysicsEnvironment *env, JoltPhysicsController *ctrl)
    : m_env(env), m_ctrl(ctrl)
{
}

JoltSoftBody::~JoltSoftBody()
{
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
    /* Do NOT touch m_ctrl here: ~JoltPhysicsController() (which runs before us
     * from ~KX_GameObject) already called RemoveController(this), so the
     * environment destructor's controller-cleanup loop will never see this
     * controller.  The m_ctrl pointer may be dangling at this point. */
  }
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

  /* Store vertex remap for mesh deformation. */
  m_blenderVertToJoltVert = vertRemap;

  /* ---- Build SoftBodySharedSettings ---- */
  JPH::Ref<JPH::SoftBodySharedSettings> sbSettings = new JPH::SoftBodySharedSettings();

  /* Per-particle inverse mass: distribute total mass evenly.
   * Per Jolt docs, mInvMass=0 pins the vertex (infinite mass). */
  float invMass = (settings.mass > 0.0f) ? (1.0f / settings.mass) * (float)numVerts : 1.0f;
  sbSettings->mVertices.resize(numVerts);
  std::vector<MT_Vector3> worldPositions(numVerts);

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
    worldPositions[i] = vr;
  }

  /* ---- Vertex pinning ----
   * Vertices whose weight in the pin vertex group >= pinWeightThreshold get
   * mInvMass = 0 (kinematic).  They stay at their initial world position unless
   * UpdatePinnedVertices() moves them to follow a pin object. */
  if (!settings.pinVertexWeights.empty()) {
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
        /* Store (joltIdx, initial world-space position in Blender Z-up). */
        MT_Vector3 initWorldPos = position + worldPositions[joltIdx];
        m_pinnedData.push_back({joltIdx, initWorldPos});
      }
    }
    /* Store pin object initial transform for per-frame offset calculation. */
    if (settings.hasPinObject) {
      m_pinInitialPos = settings.pinInitialPos;
      m_pinInitialOri = settings.pinInitialOri;
    }
    m_noPinCollision = settings.noPinCollision;
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
  bi.AddBody(m_bodyID, JPH::EActivation::Activate);

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

  /* Re-express pinned caches in the new spawned frame. */
  for (auto &entry : m_pinnedData) {
    MT_Vector3 rel = entry.second - oldOrigin;
    entry.second = newOrigin + rotationDelta * rel;
  }
  m_pinInitialPos = newOrigin + rotationDelta * (m_pinInitialPos - oldOrigin);
  m_pinInitialOri = rotationDelta * m_pinInitialOri;

  /* This orientation is now baked into local particle coordinates. */
  m_initialWorldOri = newWorldOri;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Update Mesh and Pinned Vertices
 * \{ */

void JoltSoftBody::UpdatePinnedVertices(const MT_Vector3 &pinPos, const MT_Matrix3x3 &pinOri)
{
  if (m_pinnedData.empty() || m_bodyID.IsInvalid() || !m_env) {
    return;
  }

  /* mPosition in SoftBodyMotionProperties is body-local (relative to body's
   * current world position, i.e. GetPosition()).  To pin a vertex at a target
   * Blender world position we must convert:
   *   joltLocal = joltWorld(target) - body.GetPosition() */
  MT_Matrix3x3 invInitOri = m_pinInitialOri.transposed();

  /* Match the read pattern in UpdateMesh(): NoLock interface is correct here
   * because we run before PhysicsSystem::Update() (no parallel jobs active). */
  const JPH::BodyLockInterface &lockIf =
      m_env->GetPhysicsSystem()->GetBodyLockInterfaceNoLock();
  JPH::BodyLockWrite lock(lockIf, m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }

  JPH::Body &body = lock.GetBody();
  if (!body.IsSoftBody()) {
    return;
  }

  /* Body world position in Jolt Y-up space (used to convert world→local). */
  JPH::RVec3 bodyWorldPos = body.GetPosition();

  JPH::SoftBodyMotionProperties *mp =
      static_cast<JPH::SoftBodyMotionProperties *>(body.GetMotionProperties());
  auto &joltVerts = mp->GetVertices();

  for (const auto &[joltIdx, initWorldPos] : m_pinnedData) {
    if (joltIdx < 0 || joltIdx >= (int)joltVerts.size()) {
      continue;
    }

    /* Compute target Blender world position following the pin object. */
    MT_Vector3 localOffset = invInitOri * (initWorldPos - m_pinInitialPos);
    MT_Vector3 newWorldPos = pinPos + pinOri * localOffset;

    /* Convert Blender Z-up world → Jolt Y-up world → body local. */
    JPH::Vec3 joltWorld(static_cast<float>(newWorldPos.x()),
                        static_cast<float>(newWorldPos.z()),
                        -static_cast<float>(newWorldPos.y()));
    joltVerts[joltIdx].mPosition = joltWorld - JPH::Vec3(bodyWorldPos);
    /* Keep body-local velocity at zero for pinned vertices.  Their world-space
     * motion comes from the soft body's own body velocity (which tracks the pin
     * object).  Any stale per-particle velocity would create artificial forces
     * on neighbouring unpinned vertices through edge/volume constraints and
     * cause visible jiggling, especially with CCD-enabled pin bodies. */
    joltVerts[joltIdx].mVelocity = JPH::Vec3::sZero();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Update Mesh
 * \{ */

void JoltSoftBody::UpdateMesh()
{
  /* Hidden-layer template objects must NOT write to the shared Blender modifier;
   * doing so would corrupt active replicas that share the same blender::Object*. */
  if (!m_isActive) {
    return;
  }
  if (!m_gameobj || !m_meshObject || m_bodyID.IsInvalid() || !m_env) {
    return;
  }

  /* 1. Get the evaluated Blender object for modifier and mesh access. */
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  blender::Object *ob = m_gameobj->GetBlenderObject();
  blender::Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
  blender::Mesh *me = (blender::Mesh *)ob_eval->data;

  /* Sanity: skip if a modifier changed vertex count at runtime. */
  if (m_numBlenderVerts > 0 && me->verts_num != m_numBlenderVerts) {
    return;
  }

  /* 2. First time: create the deform modifier and allocate the coord buffer. */
  if (!m_sbModifier) {
    m_numBlenderVerts = me->verts_num;
    if (m_numBlenderVerts <= 0) {
      return;
    }

    m_sbModifier = (blender::SimpleDeformModifierDataBGE *)BKE_modifier_new(
        eModifierType_SimpleDeformBGE);
    STRNCPY(m_sbModifier->modifier.name, "joltSbModifier");
    BLI_addtail(&ob->modifiers, m_sbModifier);
    BKE_modifier_unique_name(&ob->modifiers, (ModifierData *)m_sbModifier);
    BKE_modifiers_persistent_uid_init(*ob, m_sbModifier->modifier);
    DEG_relations_tag_update(CTX_data_main(C));

    m_sbCoords = (float(*)[3])MEM_new_zeroed(
        sizeof(float[3]) * (size_t)m_numBlenderVerts, "JoltSoftBody.sbCoords");

    /* Pre-fill from the rest-pose vertex positions so that vertices not covered
     * by the physics mesh (e.g. isolated verts) keep their original location. */
    const blender::Span<blender::float3> restPos = me->vert_positions();
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
  const JPH::BodyLockInterface &lockInterface = m_env->GetPhysicsSystem()->GetBodyLockInterfaceNoLock();
  JPH::BodyLockRead lock(lockInterface, m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }

  const JPH::Body &body = lock.GetBody();
  if (!body.IsSoftBody()) {
    return;
  }

  const JPH::SoftBodyMotionProperties *mp =
      static_cast<const JPH::SoftBodyMotionProperties *>(body.GetMotionProperties());
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
   * For unparented soft bodies this reduces to the object's current world
   * transform and remains equivalent to the previous direct path. */
  SG_Node *sgNode = m_gameobj->GetSGNode();
  const SG_Node *sgParent = sgNode->GetSGParent();

  MT_Vector3   renderPos;
  MT_Matrix3x3 renderInvOri;
  float invSx, invSy, invSz;

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
    renderPos    = combined.getOrigin();
    /* Pure rotation = basis with scale removed; inverse rotation = transpose. */
    renderInvOri = combined.getBasis().scaled(invSx, invSy, invSz).transposed();
  }
  else {
    renderPos = m_gameobj->NodeGetWorldPosition();
    MT_Vector3 scale = m_gameobj->NodeGetWorldScaling();
    invSx = (std::abs(scale.x()) > 1e-6f) ? 1.0f / scale.x() : 1.0f;
    invSy = (std::abs(scale.y()) > 1e-6f) ? 1.0f / scale.y() : 1.0f;
    invSz = (std::abs(scale.z()) > 1e-6f) ? 1.0f / scale.z() : 1.0f;
    renderInvOri = m_gameobj->NodeGetWorldOrientation().transposed();
  }

  for (int blVert = 0; blVert < m_numBlenderVerts; ++blVert) {
    auto it = m_blenderVertToJoltVert.find(blVert);
    if (it == m_blenderVertToJoltVert.end()) {
      continue;
    }
    int joltIdx = it->second;
    if (joltIdx < 0 || joltIdx >= (int)joltVerts.size()) {
      continue;
    }

    const JPH::Vec3 &mp = joltVerts[joltIdx].mPosition;

    /* Particle world position in Blender Z-up: body COM + body-local offset. */
    float wx = bodyBx + mp.GetX();
    float wy = bodyBy + (-mp.GetZ());
    float wz = bodyBz +   mp.GetY();

    /* World → A's local space: inv(R) * (world − origin) / scale. */
    MT_Vector3 offset((double)(wx - (float)renderPos.x()),
                      (double)(wy - (float)renderPos.y()),
                      (double)(wz - (float)renderPos.z()));
    MT_Vector3 local = renderInvOri * offset;
    m_sbCoords[blVert][0] = (float)(local.x() * invSx);
    m_sbCoords[blVert][1] = (float)(local.y() * invSy);
    m_sbCoords[blVert][2] = (float)(local.z() * invSz);
  }

  /* 4. Hand the coord buffer to the modifier and tag for re-evaluation. */
  m_sbModifier->vertcoos = m_sbCoords;
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
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
  /* Free coord buffer only after the modifier is gone. */
  if (m_sbCoords) {
    MEM_delete_void(static_cast<void *>(m_sbCoords));
    m_sbCoords = nullptr;
  }
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
    if (md->type == eModifierType_SimpleDeformBGE) {
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
    needsRotation = true;
  }
  else if (m_pinnedData.empty()) {
    /* No pinned vertices - use replica's own orientation. */
    MT_Matrix3x3 invInitialOri = m_initialWorldOri.transposed();
    rotationDelta = replicaOri * invInitialOri;
    needsRotation = true;
  }

  /* Build a set of pinned vertex indices for fast lookup. */
  std::unordered_set<int> pinnedIndices;
  for (const auto &[joltIdx, initWorldPos] : m_pinnedData) {
    pinnedIndices.insert(joltIdx);
  }

  /* Create a new SoftBodySharedSettings with rotated particle positions.
   * We must create a new settings object because the template's shared settings
   * have the template's orientation baked in, and multiple replicas may share
   * different orientations. */
  JPH::Ref<JPH::SoftBodySharedSettings> replicaSettings;
  if (needsRotation && !pinnedIndices.empty()) {
    /* Mixed case: some pinned, some non-pinned. Need custom settings. */
    replicaSettings = new JPH::SoftBodySharedSettings();
    const JPH::Array<JPH::SoftBodySharedSettings::Vertex> &srcVerts =
        m_sharedSettings->mVertices;
    replicaSettings->mVertices.resize(srcVerts.size());

    for (size_t i = 0; i < srcVerts.size(); ++i) {
      replicaSettings->mVertices[i].mInvMass = srcVerts[i].mInvMass;

      if (pinnedIndices.count((int)i) > 0) {
        /* Pinned vertex: keep position as-is (UpdatePinnedVertices will set it). */
        replicaSettings->mVertices[i].mPosition = srcVerts[i].mPosition;
      }
      else {
        /* Non-pinned vertex: rotate to match replica's orientation.
         * Position is in Jolt Y-up local space (relative to body COM).
         * Convert to Blender Z-up, apply rotation, convert back to Jolt Y-up. */
        const JPH::Float3 &jp = srcVerts[i].mPosition;
        /* Jolt Y-up (X,Y,Z) -> Blender Z-up (X,-Z,Y) */
        MT_Vector3 blenderLocal(jp[0], -jp[2], jp[1]);
        /* Apply rotation delta */
        MT_Vector3 rotated = rotationDelta * blenderLocal;
        /* Blender Z-up -> Jolt Y-up */
        replicaSettings->mVertices[i].mPosition = JPH::Float3(
            static_cast<float>(rotated.x()),
            static_cast<float>(rotated.z()),
            -static_cast<float>(rotated.y()));
      }
    }

    /* Copy all other settings (constraints, faces, etc.) */
    replicaSettings->mFaces = m_sharedSettings->mFaces;
    replicaSettings->mEdgeConstraints = m_sharedSettings->mEdgeConstraints;
    replicaSettings->mVolumeConstraints = m_sharedSettings->mVolumeConstraints;
    replicaSettings->mDihedralBendConstraints = m_sharedSettings->mDihedralBendConstraints;
    replicaSettings->mLRAConstraints = m_sharedSettings->mLRAConstraints;
    replicaSettings->mSkinnedConstraints = m_sharedSettings->mSkinnedConstraints;
    replicaSettings->mRodStretchShearConstraints = m_sharedSettings->mRodStretchShearConstraints;
    replicaSettings->mRodBendTwistConstraints = m_sharedSettings->mRodBendTwistConstraints;
    replicaSettings->mMaterials = m_sharedSettings->mMaterials;
    replicaSettings->mInvBindMatrices = m_sharedSettings->mInvBindMatrices;

    /* Optimize the new settings for simulation. */
    replicaSettings->Optimize();
  }
  else if (needsRotation) {
    /* All particles need rotation (no pinned vertices). */
    replicaSettings = new JPH::SoftBodySharedSettings();
    const JPH::Array<JPH::SoftBodySharedSettings::Vertex> &srcVerts =
        m_sharedSettings->mVertices;
    replicaSettings->mVertices.resize(srcVerts.size());

    for (size_t i = 0; i < srcVerts.size(); ++i) {
      replicaSettings->mVertices[i].mInvMass = srcVerts[i].mInvMass;

      /* Rotate all vertices. */
      const JPH::Float3 &jp = srcVerts[i].mPosition;
      MT_Vector3 blenderLocal(jp[0], -jp[2], jp[1]);
      MT_Vector3 rotated = rotationDelta * blenderLocal;
      replicaSettings->mVertices[i].mPosition = JPH::Float3(
          static_cast<float>(rotated.x()),
          static_cast<float>(rotated.z()),
          -static_cast<float>(rotated.y()));
    }

    /* Copy all other settings. */
    replicaSettings->mFaces = m_sharedSettings->mFaces;
    replicaSettings->mEdgeConstraints = m_sharedSettings->mEdgeConstraints;
    replicaSettings->mVolumeConstraints = m_sharedSettings->mVolumeConstraints;
    replicaSettings->mDihedralBendConstraints = m_sharedSettings->mDihedralBendConstraints;
    replicaSettings->mLRAConstraints = m_sharedSettings->mLRAConstraints;
    replicaSettings->mSkinnedConstraints = m_sharedSettings->mSkinnedConstraints;
    replicaSettings->mRodStretchShearConstraints = m_sharedSettings->mRodStretchShearConstraints;
    replicaSettings->mRodBendTwistConstraints = m_sharedSettings->mRodBendTwistConstraints;
    replicaSettings->mMaterials = m_sharedSettings->mMaterials;
    replicaSettings->mInvBindMatrices = m_sharedSettings->mInvBindMatrices;

    replicaSettings->Optimize();
  }
  else {
    /* No rotation needed - reuse template's shared settings. */
    replicaSettings = m_sharedSettings;
  }

  /* Preserve the template's COM-to-origin offset for this replica.
   * For rotated replicas the offset rotates with the spawn orientation delta. */
  MT_Vector3 replicaOriginOffset = m_bodyOriginOffset;
  if (needsRotation) {
    replicaOriginOffset = rotationDelta * replicaOriginOffset;
  }
  JPH::Vec3 replicaSpawnOffset = JoltMath::ToJolt(replicaOriginOffset);

  /* Additional recenter pass: if the rotated/customized local vertices are not
   * centered around local AABB center, compensate exactly like in Create(). */
  JPH::Vec3 replicaBoundsCenterCorrection(0.0f, 0.0f, 0.0f);
  if (needsRotation && !replicaSettings->mVertices.empty()) {
    const JPH::Float3 &p0 = replicaSettings->mVertices[0].mPosition;
    float minX = p0[0], minY = p0[1], minZ = p0[2];
    float maxX = p0[0], maxY = p0[1], maxZ = p0[2];

    for (size_t i = 1; i < replicaSettings->mVertices.size(); ++i) {
      const JPH::Float3 &p = replicaSettings->mVertices[i].mPosition;
      minX = std::min(minX, p[0]);
      minY = std::min(minY, p[1]);
      minZ = std::min(minZ, p[2]);
      maxX = std::max(maxX, p[0]);
      maxY = std::max(maxY, p[1]);
      maxZ = std::max(maxZ, p[2]);
    }

    replicaBoundsCenterCorrection = JPH::Vec3(0.5f * (minX + maxX),
                                              0.5f * (minY + maxY),
                                              0.5f * (minZ + maxZ));

    for (JPH::SoftBodySharedSettings::Vertex &v : replicaSettings->mVertices) {
      const JPH::Float3 &p = v.mPosition;
      v.mPosition = JPH::Float3(p[0] - replicaBoundsCenterCorrection.GetX(),
                                p[1] - replicaBoundsCenterCorrection.GetY(),
                                p[2] - replicaBoundsCenterCorrection.GetZ());
    }

    replicaSpawnOffset += replicaBoundsCenterCorrection;
  }

  /* Create a new Jolt soft body with the (possibly rotated) settings. */
  JPH::Vec3 baseReplicaPos = JoltMath::ToJolt(replicaPos);
  JPH::RVec3 posJolt(baseReplicaPos.GetX() + replicaSpawnOffset.GetX(),
                     baseReplicaPos.GetY() + replicaSpawnOffset.GetY(),
                     baseReplicaPos.GetZ() + replicaSpawnOffset.GetZ());

  JPH::SoftBodyCreationSettings cs(
      replicaSettings.GetPtr(),
      posJolt,
      JPH::Quat::sIdentity(),
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

  JPH::BodyInterface &bi = m_env->GetBodyInterfaceNoLock();
  JPH::Body *body = bi.CreateSoftBody(cs);
  if (!body) {
    return nullptr;
  }

  JoltSoftBody *clone        = new JoltSoftBody(m_env, newCtrl);
  clone->m_bodyID            = body->GetID();
  clone->m_sharedSettings    = replicaSettings;
  clone->m_replicaParams     = m_replicaParams;
  clone->m_blenderVertToJoltVert = m_blenderVertToJoltVert;
  clone->m_noPinCollision    = m_noPinCollision;
  clone->m_pinBlenderObject  = m_pinBlenderObject;
  clone->m_meshObject        = m_meshObject;
  clone->m_isActive          = true;
  clone->m_initialWorldOri   = hasPinTransform ? newPinOri : replicaOri;
  clone->m_bodyOriginOffset  = JoltMath::ToMT(replicaSpawnOffset);

  bi.AddBody(clone->m_bodyID, JPH::EActivation::Activate);

  /* Copy pinned vertex world positions, re-expressed in the replica's pin frame. */
  if (!m_pinnedData.empty()) {
    if (hasPinTransform) {
      MT_Matrix3x3 invOrigOri = m_pinInitialOri.transposed();
      for (const auto &[joltIdx, initWorldPos] : m_pinnedData) {
        MT_Vector3 localOffset = invOrigOri * (initWorldPos - m_pinInitialPos);
        MT_Vector3 newWorldPos = newPinPos + newPinOri * localOffset;
        clone->m_pinnedData.push_back({joltIdx, newWorldPos});
      }
      /* Wire the replica's pin controller so UpdatePinnedVertices() works. */
      clone->m_pinCtrl = static_cast<JoltPhysicsController *>(parentCtrl);
    }
    else {
      clone->m_pinnedData = m_pinnedData;
    }
    clone->m_pinInitialPos = newPinPos;
    clone->m_pinInitialOri = newPinOri;

    /* Pre-position pinned particles at their correct world locations immediately.
     * Without this, they start at (replicaPos + template_local_offsets) and only
     * reach the correct world positions on the first UpdatePinnedVertices call,
     * causing a visible one-frame teleport on the spawn frame. */
    if (hasPinTransform) {
      clone->UpdatePinnedVertices(newPinPos, newPinOri);
    }
  }

  return clone;
}

/** \} */

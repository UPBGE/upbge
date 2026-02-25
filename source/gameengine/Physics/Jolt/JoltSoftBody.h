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

/** \file JoltSoftBody.h
 *  \ingroup physjolt
 *  \brief Jolt Physics soft body wrapper for UPBGE.
 */

#pragma once

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"

#include <unordered_map>
#include <utility>
#include <vector>

class JoltPhysicsController;
class JoltPhysicsEnvironment;
class KX_GameObject;
class PHY_IMotionState;
class PHY_IPhysicsController;
class RAS_MeshObject;

namespace blender {
struct Object;
struct SimpleDeformModifierDataBGE;
}  // namespace blender

/**
 * Settings used when creating a Jolt soft body, mapped from BulletSoftBody DNA struct.
 */
struct JoltSoftBodySettings {
  float mass = 1.0f;
  float linStiff = 0.5f;     /* Linear stiffness 0..1 -> edge compliance */
  float shearStiff = 0.5f;   /* Shear stiffness 0..1 -> shear compliance */
  float angStiff = 1.0f;     /* Angular stiffness 0..1 -> bend compliance */
  float friction = 0.2f;     /* Dynamic friction (kDF) */
  float restitution = 0.0f;  /* Restitution/Elasticity */
  float damping = 0.0f;      /* Damping coefficient (kDP) -> mLinearDamping */
  float pressure = 0.0f;     /* Pressure coefficient (kPR) -> mPressure */
  float margin = 0.1f;       /* Vertex radius (mVertexRadius) */
  float gravityFactor = 1.0f;
  int numIterations = 5;     /* Position solver iterations (piterations) */
  bool bendingConstraints = true;
  bool lraConstraints = false; /* Long Range Attachment: prevent excessive cloth stretch */
  int lraType = 0;             /* 0=Euclidean distance, 1=Geodesic distance */
  bool facesDoubleSided = false; /* Enable collision from both sides of each face */
  /* Vertex pinning: pre-computed from BulletSoftBody.pin_vgroup at ConvertObject time.
   * Each pair is (joltParticleIndex, weight). Particles with weight >= pinWeightThreshold
   * get mInvMass = 0 (kinematic). */
  std::vector<std::pair<int, float>> pinVertexWeights;
  float pinWeightThreshold = 0.5f;
  /* If true, pinned vertices follow m_pinCtrl's world transform each frame. */
  bool hasPinObject = false;
  MT_Vector3 pinInitialPos;   /* pin object world position at creation time */
  MT_Matrix3x3 pinInitialOri; /* pin object world orientation at creation time */
  /* If true, this soft body cannot apply collision forces to its pin/parent object. */
  bool noPinCollision = false;
};

/**
 * JoltSoftBody manages a Jolt soft body and maps its simulated particle
 * positions back to the UPBGE rendering mesh each frame.
 */
class JoltSoftBody {
 public:
  JoltSoftBody(JoltPhysicsEnvironment *env,
               JoltPhysicsController *ctrl);
  ~JoltSoftBody();

  /** Build soft body from mesh data and add to physics system.
   *  @param vertices      Vertex positions (Blender Z-up local space, 3 floats each).
   *  @param numVerts      Number of vertices (= Jolt particle count).
   *  @param triangles     Triangle indices (3 per face).
   *  @param numTriangles  Number of triangles.
   *  @param position      World-space spawn position.
   *  @param worldOri      World-space orientation (baked into particle positions).
   *  @param worldScale    World-space scale (baked into particle positions).
   *  @param settings      All soft body simulation settings.
   *  @param vertRemap     Maps Blender original vertex index -> Jolt particle index.
   *  @return true on success. */
  bool Create(const float *vertices,
              int numVerts,
              const int *triangles,
              int numTriangles,
              const MT_Vector3 &position,
              const MT_Matrix3x3 &worldOri,
              const MT_Vector3 &worldScale,
              const JoltSoftBodySettings &settings,
              const std::unordered_map<int, int> &vertRemap);

  /** Set the KX_GameObject owning this soft body (needed for mesh deformation). */
  void SetGameObject(KX_GameObject *gameobj) { m_gameobj = gameobj; }

  /** Set the RAS mesh object used for visual deformation output. */
  void SetMeshObject(RAS_MeshObject *meshobj) { m_meshObject = meshobj; }

  /** Set the controller of the object that pinned vertices should follow. */
  void SetPinController(JoltPhysicsController *ctrl) { m_pinCtrl = ctrl; }

  /** Rebase this soft body after full-copy spawning.
   *  Applies the new world orientation to particle local data (and related
   *  cached offsets) so AddFullCopyObject spawns match the reference object's
   *  rotation instead of keeping the template's authored rotation.
   *
   *  @param posDelta   Spawn translation delta in world space.
   *  @param newWorldOri Target world orientation for the spawned object. */
  void ApplySpawnTransform(const MT_Vector3 &posDelta, const MT_Matrix3x3 &newWorldOri);

  /** Update kinematic (pinned) vertex positions to follow the pin object's current transform.
   *  Must be called BEFORE PhysicsSystem::Update() each frame. */
  void UpdatePinnedVertices(const MT_Vector3 &pinPos, const MT_Matrix3x3 &pinOri);

  /** Returns true if this soft body has at least one kinematic (pinned) vertex. */
  bool HasPinnedVertices() const { return !m_pinnedData.empty(); }

  /** Returns the pin controller (may be null if not set or no pin object). */
  JoltPhysicsController *GetPinController() const { return m_pinCtrl; }

  /** Initial world orientation of the pin object captured at creation time (Blender Z-up).
   *  Used by the environment to pass the correct rotation when the pin object is a soft body,
   *  since Jolt soft bodies always report identity rotation from GetPositionAndRotation(). */
  const MT_Matrix3x3 &GetPinInitialOri() const { return m_pinInitialOri; }

  /** Returns true if this soft body should not apply collision forces to its pin/parent object. */
  bool GetNoPinCollision() const { return m_noPinCollision; }

  /** Store the Blender Object* of the pin target (resolved lazily to a controller). */
  void SetPinBlenderObject(blender::Object *obj) { m_pinBlenderObject = obj; }
  blender::Object *GetPinBlenderObject() const { return m_pinBlenderObject; }

  /** Push deformed particle positions to the Blender render mesh each frame.
   *  No-op if this soft body is marked inactive (hidden-layer template). */
  void UpdateMesh();

  /** Remove the runtime deform modifier from the Blender object on shutdown. */
  void CleanupModifier(blender::Object *ob);

  /** Purge any leftover SimpleDeformBGE modifiers from a previous game run.
   *  Called once on the template object at game startup (ConvertObject), before
   *  any clones exist.  Must NOT be called from UpdateMesh() because multiple
   *  simultaneous clones share the same Blender object — a later clone's first
   *  UpdateMesh() would silently destroy an earlier clone's active modifier. */
  void PurgeStaleModifiers();

  /** Get the Jolt BodyID for this soft body. */
  JPH::BodyID GetBodyID() const { return m_bodyID; }

  /** Get the associated controller. */
  JoltPhysicsController *GetController() { return m_ctrl; }

  /** Body COM offset relative to the game-object origin in Blender world space.
   *  WriteDynamicsToMotionState subtracts this so the SG origin stays stable at startup. */
  const MT_Vector3 &GetBodyOriginOffset() const { return m_bodyOriginOffset; }

  /** Get the RAS mesh object used for rendering (needed by replica creation). */
  RAS_MeshObject *GetMeshObject() const { return m_meshObject; }

  /** Mark this soft body active (updates mesh modifier) or inactive (hidden template). */
  void SetActive(bool active) { m_isActive = active; }
  bool IsActive() const { return m_isActive; }

  /** Create a new independent JoltSoftBody for a replica game object spawned via
   *  'Add Object' logic brick.  Re-uses the shared Jolt settings but creates a
   *  fresh simulation body at the replica's world position.
   *  @param newCtrl    The replica's new (empty) physics controller.
   *  @param motionState The replica's motion state (provides spawn position).
   *  @param parentCtrl  The replica's parent controller, used as pin controller when
   *                     the original's pin object matches the parent (may be null).
   *  @return New JoltSoftBody on success, nullptr on failure (caller must delete). */
  JoltSoftBody *CloneIntoReplica(JoltPhysicsController *newCtrl,
                                  PHY_IMotionState *motionState,
                                  PHY_IPhysicsController *parentCtrl);

 private:
  /** Scalar creation parameters cached for replica cloning. */
  struct SoftBodyReplicaParams {
    float friction = 0.2f;
    float damping = 0.0f;
    float restitution = 0.0f;
    float pressure = 0.0f;
    float margin = 0.05f;
    float gravityFactor = 1.0f;
    int   numIterations = 5;
    bool  facesDoubleSided = false;
  };

  JoltPhysicsEnvironment *m_env;
  JoltPhysicsController *m_ctrl;
  JPH::BodyID m_bodyID;

  /** Shared Jolt settings — can be reused across multiple bodies (Ref-counted). */
  JPH::Ref<JPH::SoftBodySharedSettings> m_sharedSettings;

  /** Scalar creation params stored during Create() for use in CloneIntoReplica(). */
  SoftBodyReplicaParams m_replicaParams;

  KX_GameObject *m_gameobj = nullptr;
  RAS_MeshObject *m_meshObject = nullptr;

  /** Maps Blender original vertex index -> Jolt particle index. */
  std::unordered_map<int, int> m_blenderVertToJoltVert;

  /** Per-vertex coordinate array fed to SimpleDeformModifierDataBGE.
   *  Indexed by Blender vertex index; stores LOCAL-space positions. */
  float (*m_sbCoords)[3] = nullptr;

  /** Runtime deform modifier attached to the Blender object. */
  blender::SimpleDeformModifierDataBGE *m_sbModifier = nullptr;

  /** Number of Blender mesh vertices (sanity check for runtime geometry changes). */
  int m_numBlenderVerts = 0;

  /** Pinned (kinematic) vertex data: (joltParticleIndex, localOffset relative to pin object). */
  std::vector<std::pair<int, MT_Vector3>> m_pinnedData;

  /** Optional controller of the object that pinned vertices follow each frame. */
  JoltPhysicsController *m_pinCtrl = nullptr;

  /** Blender Object* of the pin target, stored for deferred FinalizeSoftBodyPins lookup. */
  blender::Object *m_pinBlenderObject = nullptr;

  /** Initial world transform of the pin object at creation time (Blender Z-up). */
  MT_Vector3   m_pinInitialPos;
  MT_Matrix3x3 m_pinInitialOri;

  /** The soft body's own initial world orientation (baked into particle positions).
   *  Used by CloneIntoReplica to compute rotation delta for replicas spawned with
   *  different orientations (e.g., when the logic brick owner is rotated). */
  MT_Matrix3x3 m_initialWorldOri;

  /** If true, this soft body does not apply collision forces to its pin/parent object. */
  bool m_noPinCollision = false;

  /** Body COM offset relative to game-object origin (Blender world space). */
  MT_Vector3 m_bodyOriginOffset = MT_Vector3(0.0f, 0.0f, 0.0f);

  /** False for hidden-layer template objects: stops UpdateMesh() writing to the
   *  shared Blender modifier so active replicas are not corrupted. */
  bool m_isActive = true;
};

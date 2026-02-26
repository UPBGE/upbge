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

/** \file JoltPhysicsEnvironment.h
 *  \ingroup physjolt
 *  \brief Jolt Physics backend implementing PHY_IPhysicsEnvironment.
 */

#pragma once

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>

#include "PHY_IPhysicsEnvironment.h"
#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class JoltCharacter;
class JoltConstraint;
class JoltPhysicsController;
class JoltGraphicController;
class JoltSoftBody;
class JoltVehicle;
class KX_GameObject;

namespace blender {
struct Depsgraph;
struct EffectorWeights;
struct Object;
struct Scene;
}

/* -------------------------------------------------------------------- */
/** \name Broadphase Layers & Object Layer Encoding
 *
 * ObjectLayer (32 bits) encoding:
 *   bits  0-13 : user col_group (14 bits, from Blender's 16-bit col_group)
 *   bits 14-15 : motion category (0=static, 1=dynamic, 2=sensor)
 *   bits 16-31 : user col_mask  (16 bits, full)
 *
 * Three broadphase layers allow Stage 1 rejection:
 *   - Static  vs Static  = skip (static objects never move into each other)
 *   - Sensor  vs Sensor  = skip (sensors only detect non-sensors)
 *   - All other combinations are checked.
 *
 * Stage 2 pair filtering uses only the user group/mask bits (motion bits stripped).
 * \{ */

enum JoltBroadPhaseLayer : JPH::BroadPhaseLayer::Type {
  JOLT_BP_STATIC  = 0,
  JOLT_BP_DYNAMIC = 1,
  JOLT_BP_SENSOR  = 2,
  JOLT_BP_NUM_LAYERS = 3
};

/** Build a Jolt ObjectLayer from UPBGE collision settings + motion category. */
inline JPH::ObjectLayer JoltMakeObjectLayer(unsigned short col_group,
                                            unsigned short col_mask,
                                            JoltBroadPhaseLayer category = JOLT_BP_STATIC)
{
  JPH::uint32 layer = ((JPH::uint32)(col_group & 0x3FFF))
                     | ((JPH::uint32)(category & 0x3) << 14)
                     | ((JPH::uint32)col_mask << 16);
  return (JPH::ObjectLayer)layer;
}

/** Extract the user col_group (14-bit) from an ObjectLayer. */
inline unsigned short JoltGetGroup(JPH::ObjectLayer layer)
{
  return (unsigned short)(layer & 0x3FFF);
}

/** Extract the user col_mask (16-bit) from an ObjectLayer. */
inline unsigned short JoltGetMask(JPH::ObjectLayer layer)
{
  return (unsigned short)((layer >> 16) & 0xFFFF);
}

/** Extract the motion category from an ObjectLayer. */
inline JoltBroadPhaseLayer JoltGetCategory(JPH::ObjectLayer layer)
{
  return (JoltBroadPhaseLayer)((layer >> 14) & 0x3);
}

/* -------------------------------------------------------------------- */
/** \name Deferred Physics Operations
 *
 * Operations that modify body state (motion type, object layer, body removal)
 * cannot be safely performed while PhysicsSystem::Update() is running.
 * These operations are deferred until after the physics step completes.
 * \{ */

/** Types of operations that can be deferred. */
enum class JoltDeferredOpType {
  SuspendDynamics,
  RestoreDynamics,
  RemoveBody,
  AddBody,
  DestroyBody,
  SetObjectLayer,
  SetMotionType
};

/** Data for a deferred operation. Union stores operation-specific parameters. */
struct JoltDeferredOp {
  JoltDeferredOpType type;
  JPH::BodyID bodyID;

  /** Parameters for different operation types. */
  union {
    /** SuspendDynamics / RestoreDynamics: ghost mode flag. */
    bool ghost;

    /** SetObjectLayer: new layer value. */
    JPH::ObjectLayer objectLayer;

    /** SetMotionType: new motion type. */
    JPH::EMotionType motionType;
  };

  /** Controller that requested this operation (for tracking). */
  JoltPhysicsController *controller = nullptr;

  /** Collision group/mask for layer operations. */
  unsigned short collisionGroup = 0;
  unsigned short collisionMask = 0;

  /** Broadphase category for layer operations. */
  JoltBroadPhaseLayer bpCategory = JOLT_BP_STATIC;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Custom Broadphase Layer Interface
 *
 * Maps ObjectLayer → BroadPhaseLayer using the 2-bit motion category.
 * \{ */

class JoltBroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override
  {
    return JOLT_BP_NUM_LAYERS;
  }

  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
  {
    return JPH::BroadPhaseLayer(JoltGetCategory(inLayer));
  }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
  {
    switch ((JPH::BroadPhaseLayer::Type)inLayer) {
      case JOLT_BP_STATIC:  return "Static";
      case JOLT_BP_DYNAMIC: return "Dynamic";
      case JOLT_BP_SENSOR:  return "Sensor";
      default:              return "Unknown";
    }
  }
#endif
};

/* -------------------------------------------------------------------- */
/** \name Custom Object vs BroadPhase Layer Filter
 *
 * Determines which broadphase trees an object needs to check.
 * Static-vs-static and sensor-vs-sensor are skipped.
 * \{ */

class JoltObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer inLayer,
                     JPH::BroadPhaseLayer inBroadPhaseLayer) const override
  {
    JoltBroadPhaseLayer myCat = JoltGetCategory(inLayer);
    JPH::BroadPhaseLayer::Type targetCat = (JPH::BroadPhaseLayer::Type)inBroadPhaseLayer;

    if (myCat == JOLT_BP_STATIC && targetCat == JOLT_BP_STATIC)
      return false;
    if (myCat == JOLT_BP_SENSOR && targetCat == JOLT_BP_SENSOR)
      return false;

    return true;
  }
};

/* -------------------------------------------------------------------- */
/** \name Custom Object Layer Pair Filter
 *
 * Stage 2 pair filtering using user col_group/col_mask.
 * Motion category bits are stripped before comparison.
 * \{ */

class JoltObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer inLayer1,
                     JPH::ObjectLayer inLayer2) const override
  {
    unsigned short group1 = JoltGetGroup(inLayer1);
    unsigned short mask1  = JoltGetMask(inLayer1);
    unsigned short group2 = JoltGetGroup(inLayer2);
    unsigned short mask2  = JoltGetMask(inLayer2);
    return (group1 & mask2) != 0 && (group2 & mask1) != 0;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Constraint Collision Group Filter
 *
 * Custom GroupFilter that disables collisions between body pairs connected
 * by a constraint with "Disable Collisions" enabled.  Checked before the
 * narrow phase (GJK/EPA), so rejected pairs skip expensive contact work.
 *
 * Bodies store their BodyID (GetIndexAndSequenceNumber) in SubGroupID.
 * \{ */

class JoltConstraintGroupFilter : public JPH::GroupFilter {
 public:
  virtual bool CanCollide(const JPH::CollisionGroup &inGroup1,
                          const JPH::CollisionGroup &inGroup2) const override
  {
    uint32_t id1 = inGroup1.GetSubGroupID();
    uint32_t id2 = inGroup2.GetSubGroupID();
    if (id1 > id2)
      std::swap(id1, id2);
    uint64_t key = (uint64_t(id1) << 32) | uint64_t(id2);
    return m_noCollidePairs.find(key) == m_noCollidePairs.end();
  }

  void DisableCollision(uint32_t id1, uint32_t id2)
  {
    if (id1 > id2)
      std::swap(id1, id2);
    uint64_t key = (uint64_t(id1) << 32) | uint64_t(id2);
    m_noCollidePairs[key]++;
  }

  void EnableCollision(uint32_t id1, uint32_t id2)
  {
    if (id1 > id2)
      std::swap(id1, id2);
    uint64_t key = (uint64_t(id1) << 32) | uint64_t(id2);
    auto it = m_noCollidePairs.find(key);
    if (it != m_noCollidePairs.end()) {
      if (--it->second <= 0) {
        m_noCollidePairs.erase(it);
      }
    }
  }

 private:
  std::unordered_map<uint64_t, int> m_noCollidePairs;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Contact Listener
 * \{ */

class JoltPhysicsEnvironment;  /* Forward declaration for back-pointer. */

/** Contact pair data stored during physics step for processing in CallbackTriggers(). */
struct JoltContactPair {
  JPH::BodyID bodyID1;
  JPH::BodyID bodyID2;
  JPH::RVec3 contactPosition;
  JPH::Vec3 contactNormal;
  float penetrationDepth;
  float combinedFriction;
  float combinedRestitution;
  bool isNew;  /**< true = OnContactAdded, false = OnContactPersisted */
};

class JoltContactListener : public JPH::ContactListener {
 public:
  virtual JPH::ValidateResult OnContactValidate(
      const JPH::Body &inBody1,
      const JPH::Body &inBody2,
      JPH::RVec3Arg inBaseOffset,
      const JPH::CollideShapeResult &inCollisionResult) override;

  virtual void OnContactAdded(const JPH::Body &inBody1,
                               const JPH::Body &inBody2,
                               const JPH::ContactManifold &inManifold,
                               JPH::ContactSettings &ioSettings) override;

  virtual void OnContactPersisted(const JPH::Body &inBody1,
                                   const JPH::Body &inBody2,
                                   const JPH::ContactManifold &inManifold,
                                   JPH::ContactSettings &ioSettings) override;

  virtual void OnContactRemoved(const JPH::SubShapeIDPair &inSubShapePair) override;

  /** Swap the accumulated contacts to the provided vector and clear internal storage.
   *  Called from the main thread between Update() and CallbackTriggers(). */
  void SwapContacts(std::vector<JoltContactPair> &outContacts);

  /** Set back-pointer to the physics environment (needed for broadphase callback). */
  void SetEnvironment(JoltPhysicsEnvironment *env) { m_env = env; }

 private:
  void StoreContact(const JPH::Body &inBody1,
                    const JPH::Body &inBody2,
                    const JPH::ContactManifold &inManifold,
                    const JPH::ContactSettings &ioSettings,
                    bool isNew);

  std::mutex m_mutex;
  std::vector<JoltContactPair> m_contacts;
  JoltPhysicsEnvironment *m_env = nullptr;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Body Activation Listener
 * \{ */

class JoltBodyActivationListener : public JPH::BodyActivationListener {
 public:
  virtual void OnBodyActivated(const JPH::BodyID &inBodyID, JPH::uint64 inBodyUserData) override;
  virtual void OnBodyDeactivated(const JPH::BodyID &inBodyID,
                                  JPH::uint64 inBodyUserData) override;
  int GetActiveCount() const { return m_activeCount.load(std::memory_order_relaxed); }

 private:
  std::atomic<int> m_activeCount{0};
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Soft Body Contact Listener
 * \{ */

/**
 * Filters soft body ↔ rigid body contacts so that a soft body with the
 * "No Force on Pin Object" flag set cannot push its designated pin/parent body.
 * Setting mInvMassScale2 = 0 gives the other body infinite effective mass for
 * that contact, so no impulse is transferred to it.
 */
class JoltSoftBodyContactListener : public JPH::SoftBodyContactListener {
 public:
  virtual JPH::SoftBodyValidateResult OnSoftBodyContactValidate(
      const JPH::Body &inSoftBody,
      const JPH::Body &inOtherBody,
      JPH::SoftBodyContactSettings &ioSettings) override;

  /** Register a soft body / pin body pair that should not exchange forces. */
  void Register(JPH::BodyID softBodyID, JPH::BodyID pinBodyID);

  /** Remove the entry when the soft body is destroyed. */
  void Unregister(JPH::BodyID softBodyID);

 private:
  std::atomic<bool> m_hasNoPinCollisionPairs{false};
  mutable std::shared_mutex m_mutex;
  /** Maps soft body IndexAndSequenceNumber → pin body IndexAndSequenceNumber. */
  std::unordered_map<JPH::uint32, JPH::uint32> m_noPinCollisionMap;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltPhysicsEnvironment
 *
 * Main physics environment implementing PHY_IPhysicsEnvironment for Jolt Physics.
 * \{ */

class JoltPhysicsEnvironment : public PHY_IPhysicsEnvironment {
 public:
  JoltPhysicsEnvironment(blender::Scene *blenderscene,
                         int numThreads,
                         int maxBodies,
                         int maxBodyPairs,
                         int maxContactConstraints,
                         int tempAllocatorMB,
                         bool visualizePhysics);
  virtual ~JoltPhysicsEnvironment();

  /** Factory method matching CcdPhysicsEnvironment::Create() pattern. */
  static JoltPhysicsEnvironment *Create(blender::Scene *blenderscene, bool visualizePhysics);

  /* ---- PHY_IPhysicsEnvironment interface ---- */

  virtual bool ProceedDeltaTime(double curTime, float timeStep, float interval) override;
  virtual void UpdateSoftBodies() override;
  virtual void DebugDrawWorld() override;

  virtual void SetFixedTimeStep(bool useFixedTimeStep, float fixedTimeStep) override;
  virtual float GetFixedTimeStep() override;

  virtual int GetDebugMode() const override;
  virtual void SetDebugMode(int debugMode) override;

  virtual void SetNumIterations(int numIter) override;
  virtual void SetNumTimeSubSteps(int numTimeSubSteps) override;
  virtual int GetNumTimeSubSteps() override;

  virtual void SetDeactivationTime(float dTime) override;
  virtual void SetDeactivationLinearTreshold(float linTresh) override;
  virtual void SetDeactivationAngularTreshold(float angTresh) override;

  virtual void SetERPNonContact(float erp) override;
  virtual void SetERPContact(float erp2) override;
  virtual void SetCFM(float cfm) override;
  virtual void SetContactBreakingTreshold(float contactBreakingTreshold) override;
  virtual void SetSolverSorConstant(float sor) override;
  virtual void SetSolverType(PHY_SolverType solverType) override;
  virtual void SetSolverTau(float tau) override;
  virtual void SetSolverDamping(float damping) override;

  virtual void SetGravity(float x, float y, float z) override;
  virtual void GetGravity(MT_Vector3 &grav) override;

  virtual PHY_IConstraint *CreateConstraint(class PHY_IPhysicsController *ctrl,
                                            class PHY_IPhysicsController *ctrl2,
                                            PHY_ConstraintType type,
                                            float pivotX,
                                            float pivotY,
                                            float pivotZ,
                                            float axisX,
                                            float axisY,
                                            float axisZ,
                                            float axis1X = 0,
                                            float axis1Y = 0,
                                            float axis1Z = 0,
                                            float axis2X = 0,
                                            float axis2Y = 0,
                                            float axis2Z = 0,
                                            int flag = 0,
                                            bool replicate_dupli = false) override;
  virtual PHY_IVehicle *CreateVehicle(PHY_IPhysicsController *ctrl) override;

  virtual void RemoveConstraintById(int constraintid, bool free) override;
  virtual bool IsRigidBodyConstraintEnabled(int constraintid) override;
  virtual float GetAppliedImpulse(int constraintid) override;

  virtual PHY_IVehicle *GetVehicleConstraint(int constraintId) override;
  virtual PHY_ICharacter *GetCharacterController(class KX_GameObject *ob) override;

  virtual PHY_IPhysicsController *RayTest(PHY_IRayCastFilterCallback &filterCallback,
                                          float fromX,
                                          float fromY,
                                          float fromZ,
                                          float toX,
                                          float toY,
                                          float toZ) override;

  virtual bool CullingTest(PHY_CullingCallback callback,
                           void *userData,
                           const std::array<MT_Vector4, 6> &planes,
                           int occlusionRes,
                           const int *viewport,
                           const MT_Matrix4x4 &matrix) override;

  virtual void AddSensor(PHY_IPhysicsController *ctrl) override;
  virtual void RemoveSensor(PHY_IPhysicsController *ctrl) override;
  virtual void AddCollisionCallback(int response_class,
                                    PHY_ResponseCallback callback,
                                    void *user) override;
  virtual bool RequestCollisionCallback(PHY_IPhysicsController *ctrl) override;
  virtual bool RemoveCollisionCallback(PHY_IPhysicsController *ctrl) override;
  virtual PHY_CollisionTestResult CheckCollision(PHY_IPhysicsController *ctrl0,
                                                 PHY_IPhysicsController *ctrl1) override;

  virtual PHY_IPhysicsController *CreateSphereController(float radius,
                                                         const MT_Vector3 &position) override;
  virtual PHY_IPhysicsController *CreateConeController(float coneradius,
                                                       float coneheight) override;

  virtual void MergeEnvironment(PHY_IPhysicsEnvironment *other_env) override;

  virtual void ConvertObject(BL_SceneConverter *converter,
                             KX_GameObject *gameobj,
                             RAS_MeshObject *meshobj,
                             KX_Scene *kxscene,
                             PHY_IMotionState *motionstate,
                             int activeLayerBitInfo,
                             bool isCompoundChild,
                             bool hasCompoundChildren) override;

  virtual void SetupObjectConstraints(KX_GameObject *obj_src,
                                      KX_GameObject *obj_dest,
                                      blender::bRigidBodyJointConstraint *dat,
                                      bool replicate_dupli) override;

  virtual int CreateRigidBodyConstraint(KX_GameObject *gameobj1,
                                        KX_GameObject *gameobj2,
                                        const MT_Vector3 &pivotLocal,
                                        const MT_Matrix3x3 &basisLocal,
                                        blender::RigidBodyCon *rbc) override;
  virtual void SetRigidBodyConstraintEnabled(int constraintid, bool enabled) override;

  virtual void ExportFile(const std::string &filename) override;

  virtual bool SavePhysicsState(std::vector<uint8_t> &outBuffer) override;
  virtual bool RestorePhysicsState(const std::vector<uint8_t> &inBuffer) override;

  /* ---- JoltPhysicsEnvironment-specific methods ---- */

  JPH::PhysicsSystem *GetPhysicsSystem()
  {
    return m_physicsSystem.get();
  }
  JPH::BodyInterface &GetBodyInterface();
  JPH::BodyInterface &GetBodyInterfaceNoLock();
  JPH::TempAllocator *GetTempAllocator() { return m_tempAllocator.get(); }
  JoltConstraintGroupFilter *GetConstraintGroupFilter() const { return m_constraintGroupFilter.GetPtr(); }

  void AddController(JoltPhysicsController *ctrl);
  bool RemoveController(JoltPhysicsController *ctrl);

  /** Register a blender Object → controller mapping (used for soft body pin lookup). */
  void RegisterControllerForObject(blender::Object *obj, JoltPhysicsController *ctrl);

  /** Find the controller registered for a given Blender Object (may return null). */
  JoltPhysicsController *FindControllerByBlenderObject(blender::Object *obj);

  /** Link pinned-vertex controllers for all soft bodies that have a pin_object set.
   *  Call once after all objects have been converted (post-scene-init). */
  void FinalizeSoftBodyPins();

  /** Register a soft body clone produced by PostProcessReplica (Add Object spawning).
   *  Adds it to m_softBodies and the no-pin-collision map if needed. */
  void AddSoftBodyReplica(JoltSoftBody *sb, JoltPhysicsController *pinCtrl);

  /** Queue a newly created rigid-body body ID for batched AddBodiesPrepare/Finalize.
   *  Used by runtime spawn paths to reduce per-body broadphase insertion cost. */
  void QueueRigidBodyBodyAdd(JPH::BodyID bodyID,
                             JPH::EActivation activation = JPH::EActivation::Activate);

  /** Remove one queued rigid-body add entry if present.
   *  Used when a body is suspended/removed before the queued add is flushed. */
  void RemovePendingRigidBodyBodyAdd(JPH::BodyID bodyID);

  /** Queue a newly created soft-body body ID for batched AddBodiesPrepare/Finalize.
   *  Used by runtime replica spawning to reduce per-body broadphase insertion cost. */
  void QueueSoftBodyBodyAdd(JPH::BodyID bodyID);

  /** Record a soft-body AddBody call so runtime broadphase maintenance can be
   *  triggered after heavy spawn bursts. */
  void NotifySoftBodyBodyAdded();

  /** Record a rigid-body AddBody call so runtime broadphase maintenance can be
   *  triggered after heavy spawn bursts. */
  void NotifyRigidBodyBodyAdded();

  /** Request one global depsgraph relations refresh for soft-body modifier edits.
   *  UpdateSoftBodies() will perform the actual tag once per frame. */
  void RequestSoftBodyRelationsTagUpdate();

  /** Mark the breakable-constraint cache dirty after threshold changes. */
  void NotifyConstraintBreakingThresholdChanged();

  /** Remove a soft body from update/contact bookkeeping and delete it.
   *  Called on runtime object destroy and environment teardown. */
  void RemoveSoftBody(JoltSoftBody *sb);

  void AddGraphicController(JoltGraphicController *ctrl);
  void RemoveGraphicController(JoltGraphicController *ctrl);

  void CallbackTriggers();
  void ApplyEffectorForces();
  blender::EffectorWeights *GetEffectorWeights();
  blender::Depsgraph *GetDepsgraph();
  void ProcessFhSprings(float timeStep);

  /** Check if physics update is currently in progress. */
  bool IsPhysicsUpdating() const { return m_isPhysicsUpdating; }

  /** Queue a deferred operation to be processed after physics update completes.
   * Returns true if queued (physics updating), false if should execute immediately. */
  bool QueueDeferredOperation(const JoltDeferredOp &op);

  /** Process all queued deferred operations. Called after physics update completes. */
  void ProcessDeferredOperations();

  /** Finalize queued rigid-body body insertions through Jolt's batched add API.
   *  Called from ProceedDeltaTime() before simulation. */
  void FlushPendingRigidBodyBodyAdds();

  /** Finalize queued soft-body body insertions through Jolt's batched add API.
   *  Called from ProceedDeltaTime() before simulation. */
  void FlushPendingSoftBodyBodyAdds();

  /** Finalize queued soft-body removals/destructions in batches.
   *  Called before each simulation step and during environment teardown. */
  void FlushPendingSoftBodyBodyRemoves();

  /** Rebuild and return dense controller iteration storage preserving set order. */
  const std::vector<JoltPhysicsController *> &GetControllersForIteration();

  /** Queue one compound child sub-shape to be assembled into the parent in one pass. */
  void QueuePendingCompoundChildShape(JoltPhysicsController *parentCtrl,
                                      const JPH::Vec3 &relativePos,
                                      const JPH::Quat &relativeRot,
                                      JPH::RefConst<JPH::Shape> childShape);

  /** Finalize queued parent compound-shape rebuilds. */
  void FinalizePendingCompoundShapeBuilds();

  /** Rebuild cache of constraints that have finite breaking thresholds. */
  void RebuildBreakableConstraintCache();

  friend class JoltContactListener;
  friend class JoltPhysicsController;

 protected:
  std::unique_ptr<JPH::TempAllocatorImpl> m_tempAllocator;
  std::unique_ptr<JPH::JobSystemThreadPool> m_jobSystem;

  /* Filter objects must be declared before m_physicsSystem because
   * PhysicsSystem::Init() stores references to them. C++ destroys members
   * in reverse declaration order, so this ensures the filters outlive
   * m_physicsSystem. */
  JoltBroadPhaseLayerInterface m_broadPhaseLayerInterface;
  JoltObjectVsBroadPhaseLayerFilter m_objectVsBroadPhaseLayerFilter;
  JoltObjectLayerPairFilter m_objectLayerPairFilter;

  std::unique_ptr<JPH::PhysicsSystem> m_physicsSystem;
  JPH::Ref<JoltConstraintGroupFilter> m_constraintGroupFilter;
  JoltContactListener m_contactListener;
  JoltSoftBodyContactListener m_softBodyContactListener;
  JoltBodyActivationListener m_bodyActivationListener;

  std::set<JoltPhysicsController *> m_controllers;
  std::set<JoltPhysicsController *> m_collisionCallbackControllers;
  std::atomic<bool> m_collectContactsForCallbacks{false};
  std::unordered_map<int, JoltConstraint *> m_constraintById;
  std::vector<JoltConstraint *> m_breakableConstraintsCache;
  std::vector<int> m_brokenConstraintIDsScratch;
  std::unordered_map<KX_GameObject *, JoltCharacter *> m_characterByObject;
  std::vector<JoltVehicle *> m_vehicles;
  std::set<JoltGraphicController *> m_graphicControllers;
  std::vector<JoltSoftBody *> m_softBodies;
  std::vector<JoltSoftBody *> m_pinnedSoftBodies;

  struct PinnedSoftBodyUpdateEntry {
    JoltSoftBody *softBody = nullptr;
    MT_Vector3 pinPos;
    MT_Matrix3x3 pinOri;
  };

  struct PendingBodyAddEntry {
    JPH::BodyID bodyID;
    JPH::EActivation activation = JPH::EActivation::Activate;
  };

  struct PendingCompoundSubShapeEntry {
    JPH::Vec3 position;
    JPH::Quat rotation;
    JPH::RefConst<JPH::Shape> shape;
  };

  /** Runtime-created rigid-body bodies waiting for batched AddBodiesFinalize. */
  std::vector<PendingBodyAddEntry> m_pendingRigidBodyBodyAdds;

  /** Per-parent queued compound sub-shapes for one-pass rebuild. */
  std::unordered_map<JoltPhysicsController *, std::vector<PendingCompoundSubShapeEntry>>
      m_pendingCompoundSubShapesByController;

  /** Runtime-created soft-body bodies waiting for batched AddBodiesFinalize. */
  std::vector<JPH::BodyID> m_pendingSoftBodyBodyAdds;

  /** Soft-body bodies queued for batched RemoveBodies/DestroyBodies. */
  std::vector<JPH::BodyID> m_pendingSoftBodyBodyRemoves;

  /** Scratch buffers reused each frame to avoid repeated allocations. */
  std::vector<PinnedSoftBodyUpdateEntry> m_pinnedSoftBodyUpdatesScratch;
  std::vector<JPH::BodyID> m_pinnedSoftBodyBodyIDsScratch;
  std::vector<JoltSoftBody *> m_softBodiesToMeshUpdateScratch;
  std::vector<JPH::BodyID> m_softBodyMeshUpdateIDsScratch;
  std::vector<JoltContactPair> m_contactPairsScratch;
  std::vector<JoltPhysicsController *> m_controllersIterationCache;

  /** True when m_controllersIterationCache must be rebuilt from m_controllers. */
  bool m_controllersIterationCacheDirty = true;

  /** Number of soft bodies added one-by-one since the last broadphase optimize. */
  int m_pendingSoftBodyAddsForOptimize = 0;

  /** Number of rigid bodies added one-by-one since the last broadphase optimize. */
  int m_pendingRigidBodyAddsForOptimize = 0;

  /** Number of soft bodies added through AddBodiesFinalize since the last optimize.
   *  Batched insertion is already broadphase-friendly, so this uses a higher
   *  optimize threshold than one-by-one AddBody insertions. */
  int m_pendingSoftBodyBatchAddsForOptimize = 0;

  /** Number of soft-body adds observed since the previous ProceedDeltaTime() tick.
   *  Includes both one-by-one and batched insertions. */
  int m_softBodyAddsSinceLastStep = 0;

  /** Number of rigid-body one-by-one adds observed since the previous step. */
  int m_rigidBodyAddsSinceLastStep = 0;

  /** Number of consecutive physics ticks without new soft-body adds while a
   *  runtime spawn burst is pending broadphase optimization. */
  int m_softBodyAddIdleFrames = 0;

  /** Number of consecutive physics ticks without new rigid-body one-by-one adds
   *  while a runtime spawn burst is pending broadphase optimization. */
  int m_rigidBodyAddIdleFrames = 0;

  /** True if at least one soft body requested DEG_relations_tag_update this frame. */
  bool m_softBodyRelationsTagDirty = false;

  /** Maps Blender Object* → JoltPhysicsController* for soft-body pin-object lookup. */
  std::unordered_map<blender::Object*, JoltPhysicsController*> m_controllerByBlenderObject;

  PHY_ResponseCallback m_triggerCallbacks[PHY_NUM_RESPONSE];
  void *m_triggerCallbacksUserPtrs[PHY_NUM_RESPONSE];

  blender::Scene *m_blenderScene;
  blender::EffectorWeights *m_fallbackEffectorWeights = nullptr;

  int m_numTimeSubSteps;
  int m_debugMode;
  bool m_debugErrors = false;

  float m_fixedTimeStep = 0.0f;
  float m_deactivationTime;
  float m_linearDeactivationThreshold;
  float m_angularDeactivationThreshold;

  int m_activeBodyCount = 0;

  bool m_needsBroadPhaseOptimize = true;
  int m_broadPhaseOptimizeCooldownFrames = 0;
  bool m_breakableConstraintsCacheDirty = true;

  /* Temporary runtime probe state for identifying constraint-heavy regressions. */
  bool m_perfProbeEnabled = false;
  int m_perfProbeWindowFrames = 120;
  int m_perfProbeFramesAccum = 0;
  double m_perfProbeStepUSAccum = 0.0;
  double m_perfProbeUpdateUSAccum = 0.0;
  double m_perfProbeBroadphaseUSAccum = 0.0;
  double m_perfProbeBreakCheckUSAccum = 0.0;
  double m_perfProbePrepUSAccum = 0.0;
  double m_perfProbeWriteKinematicUSAccum = 0.0;
  double m_perfProbePinnedUpdateUSAccum = 0.0;
  double m_perfProbeDeferredUSAccum = 0.0;
  double m_perfProbeFhSpringUSAccum = 0.0;
  double m_perfProbeSimulationTickUSAccum = 0.0;
  double m_perfProbeSyncMotionUSAccum = 0.0;
  double m_perfProbeCharacterUSAccum = 0.0;
  double m_perfProbeVehicleUSAccum = 0.0;
  double m_perfProbeCallbackUSAccum = 0.0;
  double m_perfProbeUpdateSoftBodiesUSAccum = 0.0;
  double m_perfProbeUpdateSoftBodiesDepsgraphUSAccum = 0.0;
  double m_perfProbeUpdateSoftBodiesFilterUSAccum = 0.0;
  double m_perfProbeUpdateSoftBodiesMeshUSAccum = 0.0;
  double m_perfProbeUpdateSoftBodiesRelTagUSAccum = 0.0;
  size_t m_perfProbeBroadphaseCallsAccum = 0;
  size_t m_perfProbeBroadphaseStartupCallsAccum = 0;
  size_t m_perfProbeBroadphaseSoftSingleCallsAccum = 0;
  size_t m_perfProbeBroadphaseSoftBatchCallsAccum = 0;
  size_t m_perfProbeBroadphaseRigidSingleCallsAccum = 0;
  size_t m_perfProbeBroadphaseOtherCallsAccum = 0;
  size_t m_perfProbeBreakableCheckedAccum = 0;
  size_t m_perfProbeBrokenAccum = 0;
  size_t m_perfProbeDeferredOpsQueuedAccum = 0;
  size_t m_perfProbeControllersAccum = 0;
  size_t m_perfProbeContactPairsAccum = 0;
  size_t m_perfProbePendingSoftSingleAddsAccum = 0;
  size_t m_perfProbePendingSoftBatchAddsAccum = 0;
  size_t m_perfProbePendingRigidSingleAddsAccum = 0;
  size_t m_perfProbeUpdateSoftBodiesCallsAccum = 0;
  size_t m_perfProbeSoftBodiesTotalAccum = 0;
  size_t m_perfProbeSoftBodiesCandidatesAccum = 0;
  size_t m_perfProbeSoftBodiesUpdatedAccum = 0;

  /** Flag set during PhysicsSystem::Update() to prevent unsafe body modifications. */
  bool m_isPhysicsUpdating = false;

  /** Queue of deferred operations to process after physics update completes. */
  std::vector<JoltDeferredOp> m_deferredOps;

  /** Set after the first successful physics step. Used to detect startup conversion. */
  bool m_hasSteppedSimulation = false;
};

/** \} */

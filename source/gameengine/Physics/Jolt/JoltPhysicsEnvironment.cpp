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

/** \file JoltPhysicsEnvironment.cpp
 *  \ingroup physjolt
 */

#include "JoltPhysicsEnvironment.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>

#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/StateRecorderImpl.h>

#include "JoltCharacter.h"
#include "JoltCollData.h"
#include "JoltConstraint.h"
#include "JoltDebugDraw.h"
#include "JoltDefaultMotionState.h"
#include "JoltGraphicController.h"
#include "JoltSoftBody.h"
#include "JoltVehicle.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltShapeBuilder.h"

#include "BKE_context.hh"
#include "BKE_effect.h"
#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_modifier_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"
#include "MEM_guardedalloc.h"

#include "BL_SceneConverter.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "KX_Scene.h"
#include "PHY_IMotionState.h"
#include "RAS_MeshObject.h"
#include "SG_Node.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <unordered_set>

static int g_joltConstraintUid = 1;

#define CCD_CONSTRAINT_DISABLE_LINKED_COLLISION 0x80
#include <cstdio>
#include <thread>

/* -------------------------------------------------------------------- */
/** \name Bullet-style combine functions for friction and restitution
 * \{
 *
 * Bullet Physics uses multiply mode for both friction and restitution:
 * - Combined friction = friction1 * friction2
 * - Combined restitution = restitution1 * restitution2
 *
 * This means if either object has 0 restitution, the collision is inelastic.
 * Jolt's default is max() for restitution and geometric mean for friction.
 */

static float BulletCombineFriction(const JPH::Body &inBody1,
                                    const JPH::SubShapeID &inSubShapeID1,
                                    const JPH::Body &inBody2,
                                    const JPH::SubShapeID &inSubShapeID2)
{
  (void)inSubShapeID1;
  (void)inSubShapeID2;
  return inBody1.GetFriction() * inBody2.GetFriction();
}

static float BulletCombineRestitution(const JPH::Body &inBody1,
                                       const JPH::SubShapeID &inSubShapeID1,
                                       const JPH::Body &inBody2,
                                       const JPH::SubShapeID &inSubShapeID2)
{
  (void)inSubShapeID1;
  (void)inSubShapeID2;
  return inBody1.GetRestitution() * inBody2.GetRestitution();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Jolt global initialization (once per process)
 * \{ */

static bool s_joltInitialized = false;

static void JoltTraceImpl(const char *inFMT, ...)
{
  va_list list;
  va_start(list, inFMT);
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), inFMT, list);
  va_end(list);
  printf("Jolt: %s\n", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool JoltAssertFailedImpl(const char *inExpression,
                                 const char *inMessage,
                                 const char *inFile,
                                 JPH::uint inLine)
{
  printf("Jolt Assert: %s:%u: (%s) %s\n",
         inFile,
         inLine,
         inExpression,
         inMessage ? inMessage : "");
  return true;  /* trigger breakpoint */
}
#endif

static void EnsureJoltInitialized()
{
  if (!s_joltInitialized) {
    JPH::RegisterDefaultAllocator();

    JPH::Trace = JoltTraceImpl;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailedImpl;)

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    s_joltInitialized = true;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltContactListener
 * \{ */

JPH::ValidateResult JoltContactListener::OnContactValidate(
    const JPH::Body &inBody1,
    const JPH::Body &inBody2,
    JPH::RVec3Arg inBaseOffset,
    const JPH::CollideShapeResult &inCollisionResult)
{
  /* NOTE: This is called from physics threads during PhysicsSystem::Update().
   * Only reading from bodies is safe here.
   *
   * Collision group/mask filtering is handled at stage 2 (JoltObjectLayerPairFilter)
   * so no additional filtering is needed here. */

  return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
}

void JoltContactListener::OnContactAdded(const JPH::Body &inBody1,
                                          const JPH::Body &inBody2,
                                          const JPH::ContactManifold &inManifold,
                                          JPH::ContactSettings &ioSettings)
{
  StoreContact(inBody1, inBody2, inManifold, ioSettings, true);
}

void JoltContactListener::OnContactPersisted(const JPH::Body &inBody1,
                                              const JPH::Body &inBody2,
                                              const JPH::ContactManifold &inManifold,
                                              JPH::ContactSettings &ioSettings)
{
  StoreContact(inBody1, inBody2, inManifold, ioSettings, false);
}

void JoltContactListener::OnContactRemoved(const JPH::SubShapeIDPair &inSubShapePair)
{
  /* Contact removal events are handled implicitly: if a pair that was colliding
   * last frame no longer appears in the current frame's contact list, the
   * collision event manager detects the end of collision. */
}

void JoltContactListener::StoreContact(const JPH::Body &inBody1,
                                        const JPH::Body &inBody2,
                                        const JPH::ContactManifold &inManifold,
                                        const JPH::ContactSettings &ioSettings,
                                        bool isNew)
{
  /* Called from physics threads — must be thread-safe.
   * Skip contact capture entirely when no controller requested callbacks. */
  if (!m_env || !m_env->m_collectContactsForCallbacks.load(std::memory_order_relaxed)) {
    return;
  }

  JoltContactPair pair;
  pair.bodyID1 = inBody1.GetID();
  pair.bodyID2 = inBody2.GetID();
  pair.contactNormal = inManifold.mWorldSpaceNormal;
  pair.penetrationDepth = inManifold.mPenetrationDepth;
  pair.combinedFriction = ioSettings.mCombinedFriction;
  pair.combinedRestitution = ioSettings.mCombinedRestitution;
  pair.isNew = isNew;

  /* Use first contact point position if available. */
  if (inManifold.mRelativeContactPointsOn1.size() > 0) {
    pair.contactPosition = inManifold.mBaseOffset +
                           inManifold.mRelativeContactPointsOn1[0];
  }
  else {
    pair.contactPosition = inManifold.mBaseOffset;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  m_contacts.push_back(pair);
}

void JoltContactListener::SwapContacts(std::vector<JoltContactPair> &outContacts)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  outContacts.swap(m_contacts);
  m_contacts.clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBodyContactListener
 * \{ */

JPH::SoftBodyValidateResult JoltSoftBodyContactListener::OnSoftBodyContactValidate(
    const JPH::Body &inSoftBody,
    const JPH::Body &inOtherBody,
    JPH::SoftBodyContactSettings &ioSettings)
{
  if (!m_hasNoPinCollisionPairs.load(std::memory_order_acquire)) {
    return JPH::SoftBodyValidateResult::AcceptContact;
  }

  /* Called from physics threads during soft body collision detection.
   * All bodies are locked; do NOT call any body-locking functions here. */
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_noPinCollisionMap.find(inSoftBody.GetID().GetIndexAndSequenceNumber());
  if (it != m_noPinCollisionMap.end() &&
      it->second == inOtherBody.GetID().GetIndexAndSequenceNumber()) {
    /* Keep the contact so the soft body still collides/deforms against the
     * pin/parent body, but disable impulse transfer to the pin/parent body.
     * This matches the "No Force on Pin Object" meaning. */
    ioSettings.mInvMassScale2 = 0.0f;
    ioSettings.mInvInertiaScale2 = 0.0f;
    ioSettings.mIsSensor = false;
  }
  return JPH::SoftBodyValidateResult::AcceptContact;
}

void JoltSoftBodyContactListener::Register(JPH::BodyID softBodyID, JPH::BodyID pinBodyID)
{
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_noPinCollisionMap[softBodyID.GetIndexAndSequenceNumber()] =
      pinBodyID.GetIndexAndSequenceNumber();
  m_hasNoPinCollisionPairs.store(true, std::memory_order_release);
}

void JoltSoftBodyContactListener::Unregister(JPH::BodyID softBodyID)
{
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_noPinCollisionMap.erase(softBodyID.GetIndexAndSequenceNumber());
  m_hasNoPinCollisionPairs.store(!m_noPinCollisionMap.empty(), std::memory_order_release);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltBodyActivationListener
 * \{ */

void JoltBodyActivationListener::OnBodyActivated(const JPH::BodyID &inBodyID,
                                                   JPH::uint64 inBodyUserData)
{
  m_activeCount.fetch_add(1, std::memory_order_relaxed);
}

void JoltBodyActivationListener::OnBodyDeactivated(const JPH::BodyID &inBodyID,
                                                    JPH::uint64 inBodyUserData)
{
  m_activeCount.fetch_sub(1, std::memory_order_relaxed);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltPhysicsEnvironment — Construction / Destruction
 * \{ */

JoltPhysicsEnvironment::JoltPhysicsEnvironment(blender::Scene *blenderscene,
                                               int numThreads,
                                               int maxBodies,
                                               int maxBodyPairs,
                                               int maxContactConstraints,
                                               int tempAllocatorMB,
                                               bool visualizePhysics)
    : m_blenderScene(blenderscene),
      m_numTimeSubSteps(1),
      m_debugMode(0),
      m_deactivationTime(2.0f),
      m_linearDeactivationThreshold(0.8f),
      m_angularDeactivationThreshold(1.0f)
{
  for (int i = 0; i < PHY_NUM_RESPONSE; i++) {
    m_triggerCallbacks[i] = nullptr;
    m_triggerCallbacksUserPtrs[i] = nullptr;
  }

  EnsureJoltInitialized();

  /* Reset per-session state so IDs don't grow unboundedly across sessions. */
  g_joltConstraintUid = 1;

  /* Create temporary allocator for per-frame physics allocations. */
  m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(
      tempAllocatorMB * 1024 * 1024);

  /* Create job system with configured thread count. */
  m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
      JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, numThreads);

  /* Create the physics system with 3 broadphase layers (static/dynamic/sensor).
   * Custom filter classes handle Stage 1 broadphase rejection and Stage 2
   * pair filtering using UPBGE's col_group/col_mask bitmasks. */
  m_physicsSystem = std::make_unique<JPH::PhysicsSystem>();
  m_physicsSystem->Init(
      (JPH::uint)maxBodies,
      0,  /* numBodyMutexes: 0 = auto-detect */
      (JPH::uint)maxBodyPairs,
      (JPH::uint)maxContactConstraints,
      m_broadPhaseLayerInterface,
      m_objectVsBroadPhaseLayerFilter,
      m_objectLayerPairFilter);

  /* Create the constraint group filter used for "Disable Collisions" on
   * rigid body constraints.  This filter is shared by all bodies and checked
   * before narrow-phase GJK/EPA, so rejected pairs skip expensive work. */
  m_constraintGroupFilter = new JoltConstraintGroupFilter();

  /* Register listeners. */
  m_contactListener.SetEnvironment(this);
  m_physicsSystem->SetBodyActivationListener(&m_bodyActivationListener);
  m_physicsSystem->SetContactListener(&m_contactListener);
  m_physicsSystem->SetSoftBodyContactListener(&m_softBodyContactListener);

  /* Set Bullet-style combine functions for friction and restitution.
   * This makes the behavior consistent with Bullet Physics where
   * restitution = r1 * r2 (if either is 0, result is 0). */
  m_physicsSystem->SetCombineFriction(BulletCombineFriction);
  m_physicsSystem->SetCombineRestitution(BulletCombineRestitution);

  /* Set default gravity (Blender Z-up: -9.81 on Z → Jolt Y-up: -9.81 on Y). */
  m_physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

  if (visualizePhysics) {
    m_debugMode = 1;
  }
  m_debugErrors = blenderscene->gm.jolt_debug_errors != 0;

  const char *perfProbeEnv = std::getenv("UPBGE_JOLT_PERF_PROBE");
  const char *perfProbeWindowEnv = std::getenv("UPBGE_JOLT_PERF_PROBE_WINDOW");

  /* Probe defaults to debug-errors mode and can be forced through env vars.
   * This keeps regular gameplay builds quiet unless explicit investigation is enabled. */
  m_perfProbeEnabled = m_debugErrors;
  if (perfProbeEnv && perfProbeEnv[0] != '\0' && perfProbeEnv[0] != '0') {
    m_perfProbeEnabled = true;
  }

  if (perfProbeWindowEnv) {
    const int window = std::atoi(perfProbeWindowEnv);
    if (window > 0) {
      m_perfProbeWindowFrames = window;
    }
  }

  if (m_perfProbeEnabled) {
    printf("Jolt: Perf probe enabled (window=%d frames)\n", m_perfProbeWindowFrames);
  }
}

JoltPhysicsEnvironment::~JoltPhysicsEnvironment()
{
  /* Remove all constraints from the physics system first (they reference bodies). */
  for (auto &pair : m_constraintById) {
    JoltConstraint *con = pair.second;
    JPH::Constraint *joltCon = con->GetConstraint();
    if (joltCon) {
      m_physicsSystem->RemoveConstraint(joltCon);
    }
    delete con;
  }
  m_constraintById.clear();

  /* Delete characters (CharacterVirtual instances). */
  for (auto &pair : m_characterByObject) {
    delete pair.second;
  }
  m_characterByObject.clear();

  /* Delete vehicles. */
  for (JoltVehicle *veh : m_vehicles) {
    delete veh;
  }
  m_vehicles.clear();

  /* Delete soft bodies. */
  while (!m_softBodies.empty()) {
    RemoveSoftBody(m_softBodies.back());
  }
  FlushPendingSoftBodyBodyRemoves();

  /* Controllers are normally destroyed by KX_GameObject before we get here.
   * Each controller destructor removes its body and erases itself from m_controllers.
   * Handle any stragglers that weren't cleaned up by game objects. */
  {
    JPH::BodyInterface &bodyInterface = m_physicsSystem->GetBodyInterfaceNoLock();
    /* Copy the set since controller destructors modify m_controllers. */
    std::vector<JoltPhysicsController *> remaining(m_controllers.begin(), m_controllers.end());

    std::vector<JPH::BodyID> addedBodyIDs;
    std::vector<JPH::BodyID> destroyBodyIDs;
    addedBodyIDs.reserve(remaining.size());
    destroyBodyIDs.reserve(remaining.size());

    for (JoltPhysicsController *ctrl : remaining) {
      JPH::BodyID bodyID = ctrl->GetBodyID();
      if (!bodyID.IsInvalid()) {
        if (std::find(destroyBodyIDs.begin(), destroyBodyIDs.end(), bodyID) == destroyBodyIDs.end()) {
          destroyBodyIDs.push_back(bodyID);
        }
        if (bodyInterface.IsAdded(bodyID) &&
            std::find(addedBodyIDs.begin(), addedBodyIDs.end(), bodyID) == addedBodyIDs.end()) {
          addedBodyIDs.push_back(bodyID);
        }
      }
    }

    if (!addedBodyIDs.empty()) {
      bodyInterface.RemoveBodies(addedBodyIDs.data(), (int)addedBodyIDs.size());
    }
    if (!destroyBodyIDs.empty()) {
      bodyInterface.DestroyBodies(destroyBodyIDs.data(), (int)destroyBodyIDs.size());
    }

    for (JoltPhysicsController *ctrl : remaining) {
      ctrl->SetEnvironment(nullptr);  /* Prevent controller destructor from double-freeing. */
      delete ctrl;  /* Free the controller and its motion state. */
    }

    m_pendingRigidBodyBodyAdds.clear();
    m_pendingCompoundSubShapesByController.clear();
    m_controllers.clear();
    m_controllersIterationCache.clear();
    m_controllersIterationCacheDirty = true;
  }

  /* Graphic controllers are owned by KX_GameObject, but clear stale pointers. */
  m_graphicControllers.clear();

  if (m_fallbackEffectorWeights) {
    MEM_delete(m_fallbackEffectorWeights);
    m_fallbackEffectorWeights = nullptr;
  }

  /* Destroy physics system before job system and allocator. */
  m_physicsSystem.reset();
  m_jobSystem.reset();
  m_tempAllocator.reset();

  /* Jolt global state (allocator, Factory, registered types) persists for the
   * entire process lifetime.  Repeated init/shutdown of these globals caused
   * memory growth from thread-stack caching and heap fragmentation. */
}

JoltPhysicsEnvironment *JoltPhysicsEnvironment::Create(blender::Scene *blenderscene,
                                                       bool visualizePhysics)
{
  /* Read configuration from scene's GameData. */
  int numThreads = blenderscene->gm.jolt_physics_threads;
  if (numThreads <= 0) {
    numThreads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
  }
  numThreads = std::clamp(numThreads, 1, (int)std::thread::hardware_concurrency());

  int maxBodies = blenderscene->gm.jolt_max_bodies;
  if (maxBodies <= 0) {
    maxBodies = 65536;
  }

  int maxBodyPairs = blenderscene->gm.jolt_max_body_pairs;
  if (maxBodyPairs <= 0) {
    maxBodyPairs = 10240;
  }

  int maxContactConstraints = blenderscene->gm.jolt_max_contact_constraints;
  if (maxContactConstraints <= 0) {
    maxContactConstraints = 10240;
  }

  /* Temp allocator needs memory for:
   * - Body pairs (maxBodyPairs * ~200 bytes)
   * - Contact constraints (maxContactConstraints * ~64 bytes)
   * - Per-body temp data (maxBodies * ~900 bytes)
   * Use a safe multiplier to avoid OOM during collision detection. */
  int tempAllocatorMB = std::max(64, (int)((int64_t)maxBodies * 2048 / (1024 * 1024)) + 32);
  JoltPhysicsEnvironment *env = new JoltPhysicsEnvironment(blenderscene,
                                                           numThreads,
                                                           maxBodies,
                                                           maxBodyPairs,
                                                           maxContactConstraints,
                                                           tempAllocatorMB,
                                                           visualizePhysics);

  /* Apply scene-level physics settings. */
  env->SetGravity(0.0f, 0.0f, -(blenderscene->gm.gravity));
  env->SetDeactivationTime(blenderscene->gm.deactivationtime);
  env->SetDeactivationLinearTreshold(blenderscene->gm.lineardeactthreshold);
  env->SetDeactivationAngularTreshold(blenderscene->gm.angulardeactthreshold);
  env->SetERPNonContact(blenderscene->gm.erp);
  env->SetERPContact(blenderscene->gm.erp2);

  return env;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltPhysicsEnvironment — PHY_IPhysicsEnvironment interface
 * \{ */

bool JoltPhysicsEnvironment::ProceedDeltaTime(double curTime, float timeStep, float interval)
{
  if (timeStep <= 0.0f) {
    return false;
  }

  using PerfClock = std::chrono::steady_clock;
  const bool perfProbeEnabled = m_perfProbeEnabled;
  const PerfClock::time_point perfStepStart =
      perfProbeEnabled ? PerfClock::now() : PerfClock::time_point();
  double perfBroadphaseUS = 0.0;
  double perfUpdateUS = 0.0;
  double perfBreakCheckUS = 0.0;
  double perfPrepUS = 0.0;
  double perfWriteKinematicUS = 0.0;
  double perfPinnedUpdateUS = 0.0;
  double perfDeferredUS = 0.0;
  double perfFhSpringUS = 0.0;
  double perfSimulationTickUS = 0.0;
  double perfSyncMotionUS = 0.0;
  double perfCharacterUS = 0.0;
  double perfVehicleUS = 0.0;
  double perfCallbackUS = 0.0;
  size_t perfBreakableChecked = 0;
  size_t perfBroken = 0;
  size_t perfDeferredOpsQueued = 0;
  size_t perfControllers = 0;
  size_t perfContactPairs = 0;
  size_t perfPendingSoftSingleAdds = 0;
  size_t perfPendingSoftBatchAdds = 0;
  size_t perfPendingRigidSingleAdds = 0;

  int collisionSteps = m_numTimeSubSteps;
  if (collisionSteps < 1) {
    collisionSteps = 1;
  }

  /* Full simulation step sequence (matching Bullet's order for physics correctness):
   *   1. Sync motion states BEFORE physics (kinematic bodies read from game objects)
   *   1.75 Apply Blender effectors (force fields)
   *   2. PhysicsSystem::Update()
   *   3. Process FH springs
   *   4. Sync motion states AFTER physics (dynamic bodies write to game objects)
   *   5. CallbackTriggers()
   */

  PerfClock::time_point prepStart;
  if (perfProbeEnabled) {
    prepStart = PerfClock::now();
    perfPendingSoftSingleAdds = (size_t)m_pendingSoftBodyAddsForOptimize;
    perfPendingSoftBatchAdds = (size_t)m_pendingSoftBodyBatchAddsForOptimize;
    perfPendingRigidSingleAdds = (size_t)m_pendingRigidBodyAddsForOptimize;
  }

  /* Finalize any queued compound-shape rebuilds before body adds so newly added
   * bodies enter the broadphase with their final collision shape. */
  FinalizePendingCompoundShapeBuilds();

  /* Finalize queued runtime body additions/removals before per-step work.
   * Rigid bodies and soft bodies both use Jolt's batched add APIs here. */
  FlushPendingRigidBodyBodyAdds();
  FlushPendingSoftBodyBodyRemoves();
  FlushPendingSoftBodyBodyAdds();

  if (perfProbeEnabled) {
    perfPrepUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                     PerfClock::now() - prepStart)
                     .count();
  }

  /* Broadphase maintenance:
   *  - one-time optimize after scene load / environment merge;
   *  - runtime re-optimize for many one-by-one AddBody insertions;
   *  - optional safety optimize for very large batched spawn bursts.
   *
   * Jolt docs note batched insertion is already broadphase-friendly and that
   * OptimizeBroadPhase should not be called every frame. Keep a short cooldown
   * for runtime-triggered optimizes to avoid repeated spikes. */
  constexpr int kRuntimeSoftBodySingleAddOptimizeThreshold = 8;
  constexpr int kRuntimeSoftBodyBatchAddOptimizeThreshold = 128;
  constexpr int kRuntimeSoftBodySingleAddIdleFramesBeforeOptimize = 2;
  constexpr int kRuntimeSoftBodyBatchAddIdleFramesBeforeOptimize = 4;
  constexpr int kRuntimeRigidBodySingleAddOptimizeThreshold = 16;
  constexpr int kRuntimeRigidBodySingleAddIdleFramesBeforeOptimize = 2;
  constexpr int kRuntimeBroadPhaseOptimizeCooldownFrames = 30;

  if (m_broadPhaseOptimizeCooldownFrames > 0) {
    --m_broadPhaseOptimizeCooldownFrames;
  }

  if (m_pendingSoftBodyAddsForOptimize > 0 || m_pendingSoftBodyBatchAddsForOptimize > 0) {
    if (m_softBodyAddsSinceLastStep > 0) {
      m_softBodyAddIdleFrames = 0;
    }
    else {
      ++m_softBodyAddIdleFrames;
    }
  }

  if (m_pendingRigidBodyAddsForOptimize > 0) {
    if (m_rigidBodyAddsSinceLastStep > 0) {
      m_rigidBodyAddIdleFrames = 0;
    }
    else {
      ++m_rigidBodyAddIdleFrames;
    }
  }

  const bool shouldOptimizeForSingleAdds =
      m_pendingSoftBodyAddsForOptimize >= kRuntimeSoftBodySingleAddOptimizeThreshold &&
      m_softBodyAddIdleFrames >= kRuntimeSoftBodySingleAddIdleFramesBeforeOptimize;
  const bool shouldOptimizeForBatchAdds =
      m_pendingSoftBodyBatchAddsForOptimize >= kRuntimeSoftBodyBatchAddOptimizeThreshold &&
      m_softBodyAddIdleFrames >= kRuntimeSoftBodyBatchAddIdleFramesBeforeOptimize;
  const bool shouldOptimizeForRigidSingleAdds =
      m_pendingRigidBodyAddsForOptimize >= kRuntimeRigidBodySingleAddOptimizeThreshold &&
      m_rigidBodyAddIdleFrames >= kRuntimeRigidBodySingleAddIdleFramesBeforeOptimize;

  const bool shouldOptimizeForRuntimeAdds =
      shouldOptimizeForSingleAdds || shouldOptimizeForBatchAdds || shouldOptimizeForRigidSingleAdds;

  /* Keep startup optimize immediate. Runtime optimize requests are throttled to
   * avoid expensive repeated broadphase rebuilds in consecutive frames. */
  const bool shouldForceOptimizeNow = m_needsBroadPhaseOptimize && !m_hasSteppedSimulation;
  const bool shouldOptimizeNow =
      shouldForceOptimizeNow ||
      ((m_needsBroadPhaseOptimize || shouldOptimizeForRuntimeAdds) &&
       m_broadPhaseOptimizeCooldownFrames == 0);

  if (shouldOptimizeNow) {
    PerfClock::time_point broadPhaseStart;
    if (perfProbeEnabled) {
      broadPhaseStart = PerfClock::now();
    }

    m_physicsSystem->OptimizeBroadPhase();
    m_needsBroadPhaseOptimize = false;
    m_pendingSoftBodyAddsForOptimize = 0;
    m_pendingRigidBodyAddsForOptimize = 0;
    m_pendingSoftBodyBatchAddsForOptimize = 0;
    m_softBodyAddIdleFrames = 0;
    m_rigidBodyAddIdleFrames = 0;
    m_broadPhaseOptimizeCooldownFrames = kRuntimeBroadPhaseOptimizeCooldownFrames;

    if (perfProbeEnabled) {
      perfBroadphaseUS += (double)std::chrono::duration_cast<std::chrono::microseconds>(
                              PerfClock::now() - broadPhaseStart)
                              .count();
      ++m_perfProbeBroadphaseCallsAccum;
      if (shouldForceOptimizeNow) {
        ++m_perfProbeBroadphaseStartupCallsAccum;
      }
      if (shouldOptimizeForSingleAdds) {
        ++m_perfProbeBroadphaseSoftSingleCallsAccum;
      }
      if (shouldOptimizeForBatchAdds) {
        ++m_perfProbeBroadphaseSoftBatchCallsAccum;
      }
      if (shouldOptimizeForRigidSingleAdds) {
        ++m_perfProbeBroadphaseRigidSingleCallsAccum;
      }
      if (!shouldForceOptimizeNow && !shouldOptimizeForSingleAdds &&
          !shouldOptimizeForBatchAdds && !shouldOptimizeForRigidSingleAdds) {
        ++m_perfProbeBroadphaseOtherCallsAccum;
      }
    }
  }
  m_softBodyAddsSinceLastStep = 0;
  m_rigidBodyAddsSinceLastStep = 0;

  /* Step 1: Write game object transforms to kinematic bodies. */
  PerfClock::time_point writeKinematicStart;
  if (perfProbeEnabled) {
    writeKinematicStart = PerfClock::now();
  }

  for (JoltPhysicsController *ctrl : GetControllersForIteration()) {
    ctrl->WriteMotionStateToDynamics(true);  /* nondynaonly = true → only kinematic/static */
  }

  if (perfProbeEnabled) {
    perfWriteKinematicUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                             PerfClock::now() - writeKinematicStart)
                             .count();
    perfControllers = GetControllersForIteration().size();
  }

  /* Step 1.5: Update kinematic (pinned) vertices on soft bodies so they follow their
   * pin objects this frame. Must happen after kinematic rigid bodies are written
   * (Step 1) but before the physics solver runs (Step 2). */
  PerfClock::time_point pinnedUpdateStart;
  if (perfProbeEnabled) {
    pinnedUpdateStart = PerfClock::now();
  }

  m_pinnedSoftBodyUpdatesScratch.clear();
  m_pinnedSoftBodyBodyIDsScratch.clear();
  m_pinnedSoftBodyUpdatesScratch.reserve(m_pinnedSoftBodies.size());
  m_pinnedSoftBodyBodyIDsScratch.reserve(m_pinnedSoftBodies.size());

  for (JoltSoftBody *sb : m_pinnedSoftBodies) {
    if (!sb || !sb->HasPinnedVertices()) {
      continue;
    }

    JoltPhysicsController *pinCtrl = sb->GetPinController();
    if (!pinCtrl || !pinCtrl->GetMotionState()) {
      continue;
    }

    const JPH::BodyID sbBodyID = sb->GetBodyID();
    if (sbBodyID.IsInvalid()) {
      continue;
    }

    MT_Vector3 pinPos = pinCtrl->GetMotionState()->GetWorldPosition();
    /* Jolt soft bodies always report identity rotation (orientation is baked into
     * particle positions at creation, never updated as a body rotation).
     * For soft body pin objects we must use the stored initial Blender orientation,
     * which is their permanent effective rotation. For rigid body pin objects,
     * read the actual current orientation from the motion state. */
    MT_Matrix3x3 pinOri = pinCtrl->GetSoftBody() ?
                              sb->GetPinInitialOri() :
                              pinCtrl->GetMotionState()->GetWorldOrientation();

    PinnedSoftBodyUpdateEntry entry;
    entry.softBody = sb;
    entry.pinPos = pinPos;
    entry.pinOri = pinOri;
    m_pinnedSoftBodyUpdatesScratch.push_back(entry);
    m_pinnedSoftBodyBodyIDsScratch.push_back(sbBodyID);
  }

  if (!m_pinnedSoftBodyBodyIDsScratch.empty()) {
    const JPH::BodyLockInterface &lockIf =
        m_physicsSystem->GetBodyLockInterfaceNoLock();
    JPH::BodyLockMultiWrite multiLock(lockIf,
                                      m_pinnedSoftBodyBodyIDsScratch.data(),
                                      (int)m_pinnedSoftBodyBodyIDsScratch.size());

    for (int i = 0; i < (int)m_pinnedSoftBodyBodyIDsScratch.size(); ++i) {
      JPH::Body *body = multiLock.GetBody(i);
      if (!body) {
        continue;
      }

      const PinnedSoftBodyUpdateEntry &entry = m_pinnedSoftBodyUpdatesScratch[(size_t)i];
      entry.softBody->UpdatePinnedVerticesLocked(entry.pinPos, entry.pinOri, *body);
    }
  }

  if (perfProbeEnabled) {
    perfPinnedUpdateUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                           PerfClock::now() - pinnedUpdateStart)
                           .count();
  }

  /* Step 1.75: Apply Blender effectors (force fields) to dynamic rigid bodies. */
  ApplyEffectorForces();

  /* Step 2: Run the physics simulation.
   * Set the updating flag to defer unsafe body modifications.
   * This prevents crashes when logic tries to modify bodies during physics. */
  PerfClock::time_point updateStart;
  if (perfProbeEnabled) {
    updateStart = PerfClock::now();
  }

  m_isPhysicsUpdating = true;
  JPH::EPhysicsUpdateError updateErr = m_physicsSystem->Update(
      timeStep, collisionSteps, m_tempAllocator.get(), m_jobSystem.get());
  m_isPhysicsUpdating = false;

  if (perfProbeEnabled) {
    perfUpdateUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                       PerfClock::now() - updateStart)
                       .count();
  }

  if (m_debugErrors && updateErr != JPH::EPhysicsUpdateError::None) {
    printf("Jolt: Update returned error %d\n", (int)updateErr);
  }

  /* Process any deferred operations that were queued during the physics update. */
  PerfClock::time_point deferredStart;
  if (perfProbeEnabled) {
    deferredStart = PerfClock::now();
    perfDeferredOpsQueued = m_deferredOps.size();
  }

  ProcessDeferredOperations();
  FlushPendingSoftBodyBodyRemoves();

  if (perfProbeEnabled) {
    perfDeferredUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                         PerfClock::now() - deferredStart)
                         .count();
  }

  /* Step 3: Process FH (Floating Height) springs. */
  PerfClock::time_point fhSpringStart;
  if (perfProbeEnabled) {
    fhSpringStart = PerfClock::now();
  }

  ProcessFhSprings(timeStep);

  if (perfProbeEnabled) {
    perfFhSpringUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                        PerfClock::now() - fhSpringStart)
                        .count();
  }

  /* Step 3b: SimulationTick — clamp velocities (min/max linear/angular). */
  PerfClock::time_point simulationTickStart;
  if (perfProbeEnabled) {
    simulationTickStart = PerfClock::now();
  }

  for (JoltPhysicsController *ctrl : GetControllersForIteration()) {
    ctrl->SimulationTick(timeStep);
  }

  if (perfProbeEnabled) {
    perfSimulationTickUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                               PerfClock::now() - simulationTickStart)
                               .count();
  }

  /* Step 4: Read Jolt body transforms back to game objects. */
  PerfClock::time_point syncMotionStart;
  if (perfProbeEnabled) {
    syncMotionStart = PerfClock::now();
  }

  for (JoltPhysicsController *ctrl : GetControllersForIteration()) {
    ctrl->SynchronizeMotionStates(timeStep);
  }

  if (perfProbeEnabled) {
    perfSyncMotionUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                          PerfClock::now() - syncMotionStart)
                          .count();
  }

  /* Step 5: Update characters. */
  PerfClock::time_point characterStart;
  if (perfProbeEnabled) {
    characterStart = PerfClock::now();
  }

  for (auto &pair : m_characterByObject) {
    pair.second->Update(timeStep);
  }

  if (perfProbeEnabled) {
    perfCharacterUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                        PerfClock::now() - characterStart)
                        .count();
  }

  /* Step 6: Sync vehicle wheels. */
  PerfClock::time_point vehicleStart;
  if (perfProbeEnabled) {
    vehicleStart = PerfClock::now();
  }

  for (JoltVehicle *veh : m_vehicles) {
    veh->SyncWheels();
  }

  if (perfProbeEnabled) {
    perfVehicleUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                      PerfClock::now() - vehicleStart)
                      .count();
  }

  /* Step 7: Check breaking thresholds on constraints. */
  PerfClock::time_point breakCheckStart;
  if (perfProbeEnabled) {
    breakCheckStart = PerfClock::now();
  }

  if (m_breakableConstraintsCacheDirty) {
    RebuildBreakableConstraintCache();
  }

  m_brokenConstraintIDsScratch.clear();
  for (JoltConstraint *con : m_breakableConstraintsCache) {
    if (con && con->CheckBreaking()) {
      m_brokenConstraintIDsScratch.push_back(con->GetIdentifier());
    }
  }

  for (int constraintID : m_brokenConstraintIDsScratch) {
    RemoveConstraintById(constraintID, true);
  }

  if (perfProbeEnabled) {
    perfBreakableChecked = m_breakableConstraintsCache.size();
    perfBroken = m_brokenConstraintIDsScratch.size();
    perfBreakCheckUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                           PerfClock::now() - breakCheckStart)
                           .count();
  }

  /* Step 8: Fire collision callbacks. */
  PerfClock::time_point callbackStart;
  if (perfProbeEnabled) {
    callbackStart = PerfClock::now();
  }

  CallbackTriggers();

  if (perfProbeEnabled) {
    perfCallbackUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                       PerfClock::now() - callbackStart)
                       .count();
    if (m_triggerCallbacks[PHY_OBJECT_RESPONSE]) {
      perfContactPairs = m_contactPairsScratch.size();
    }
  }

  /* Track active body count. */
  m_activeBodyCount = m_bodyActivationListener.GetActiveCount();
  m_hasSteppedSimulation = true;

  if (perfProbeEnabled) {
    const double perfStepUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                                  PerfClock::now() - perfStepStart)
                                  .count();

    m_perfProbeStepUSAccum += perfStepUS;
    m_perfProbeUpdateUSAccum += perfUpdateUS;
    m_perfProbeBroadphaseUSAccum += perfBroadphaseUS;
    m_perfProbeBreakCheckUSAccum += perfBreakCheckUS;
    m_perfProbePrepUSAccum += perfPrepUS;
    m_perfProbeWriteKinematicUSAccum += perfWriteKinematicUS;
    m_perfProbePinnedUpdateUSAccum += perfPinnedUpdateUS;
    m_perfProbeDeferredUSAccum += perfDeferredUS;
    m_perfProbeFhSpringUSAccum += perfFhSpringUS;
    m_perfProbeSimulationTickUSAccum += perfSimulationTickUS;
    m_perfProbeSyncMotionUSAccum += perfSyncMotionUS;
    m_perfProbeCharacterUSAccum += perfCharacterUS;
    m_perfProbeVehicleUSAccum += perfVehicleUS;
    m_perfProbeCallbackUSAccum += perfCallbackUS;
    m_perfProbeBreakableCheckedAccum += perfBreakableChecked;
    m_perfProbeBrokenAccum += perfBroken;
    m_perfProbeDeferredOpsQueuedAccum += perfDeferredOpsQueued;
    m_perfProbeControllersAccum += perfControllers;
    m_perfProbeContactPairsAccum += perfContactPairs;
    m_perfProbePendingSoftSingleAddsAccum += perfPendingSoftSingleAdds;
    m_perfProbePendingSoftBatchAddsAccum += perfPendingSoftBatchAdds;
    m_perfProbePendingRigidSingleAddsAccum += perfPendingRigidSingleAdds;
    ++m_perfProbeFramesAccum;

    const int probeWindow = std::max(1, m_perfProbeWindowFrames);
    if (m_perfProbeFramesAccum >= probeWindow) {
      const double invFrames = 1.0 / (double)m_perfProbeFramesAccum;
      const double stepAvgMS = (m_perfProbeStepUSAccum * invFrames) / 1000.0;
      const double updateAvgMS = (m_perfProbeUpdateUSAccum * invFrames) / 1000.0;
      const double broadphaseFrameAvgMS = (m_perfProbeBroadphaseUSAccum * invFrames) / 1000.0;
      const double breakCheckAvgMS = (m_perfProbeBreakCheckUSAccum * invFrames) / 1000.0;
      const double prepAvgMS = (m_perfProbePrepUSAccum * invFrames) / 1000.0;
      const double writeKinematicAvgMS = (m_perfProbeWriteKinematicUSAccum * invFrames) / 1000.0;
      const double pinnedUpdateAvgMS = (m_perfProbePinnedUpdateUSAccum * invFrames) / 1000.0;
      const double deferredAvgMS = (m_perfProbeDeferredUSAccum * invFrames) / 1000.0;
      const double fhSpringAvgMS = (m_perfProbeFhSpringUSAccum * invFrames) / 1000.0;
      const double simulationTickAvgMS = (m_perfProbeSimulationTickUSAccum * invFrames) / 1000.0;
      const double syncMotionAvgMS = (m_perfProbeSyncMotionUSAccum * invFrames) / 1000.0;
      const double characterAvgMS = (m_perfProbeCharacterUSAccum * invFrames) / 1000.0;
      const double vehicleAvgMS = (m_perfProbeVehicleUSAccum * invFrames) / 1000.0;
      const double callbackAvgMS = (m_perfProbeCallbackUSAccum * invFrames) / 1000.0;
      const double broadphaseCallAvgMS =
          m_perfProbeBroadphaseCallsAccum > 0 ?
              (m_perfProbeBroadphaseUSAccum / (double)m_perfProbeBroadphaseCallsAccum) / 1000.0 :
              0.0;
      const double breakableCheckedAvg = (double)m_perfProbeBreakableCheckedAccum * invFrames;
      const double deferredQueuedAvg = (double)m_perfProbeDeferredOpsQueuedAccum * invFrames;
      const double controllersAvg = (double)m_perfProbeControllersAccum * invFrames;
      const double contactPairsAvg = (double)m_perfProbeContactPairsAccum * invFrames;
      const double pendingSoftSingleAddsAvg = (double)m_perfProbePendingSoftSingleAddsAccum * invFrames;
      const double pendingSoftBatchAddsAvg = (double)m_perfProbePendingSoftBatchAddsAccum * invFrames;
      const double pendingRigidSingleAddsAvg =
          (double)m_perfProbePendingRigidSingleAddsAccum * invFrames;
      const double updateSoftBodiesFrameAvgMS =
          (m_perfProbeUpdateSoftBodiesUSAccum * invFrames) / 1000.0;
      const double updateSoftBodiesCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double updateSoftBodiesDepsgraphCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesDepsgraphUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double updateSoftBodiesFilterCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesFilterUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double updateSoftBodiesMeshCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesMeshUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double updateSoftBodiesRelTagCallAvgMS =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (m_perfProbeUpdateSoftBodiesRelTagUSAccum /
               (double)m_perfProbeUpdateSoftBodiesCallsAccum) /
                  1000.0 :
              0.0;
      const double softBodiesTotalAvg =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (double)m_perfProbeSoftBodiesTotalAccum /
                  (double)m_perfProbeUpdateSoftBodiesCallsAccum :
              0.0;
      const double softBodiesCandidatesAvg =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (double)m_perfProbeSoftBodiesCandidatesAccum /
                  (double)m_perfProbeUpdateSoftBodiesCallsAccum :
              0.0;
      const double softBodiesUpdatedAvg =
          m_perfProbeUpdateSoftBodiesCallsAccum > 0 ?
              (double)m_perfProbeSoftBodiesUpdatedAccum /
                  (double)m_perfProbeUpdateSoftBodiesCallsAccum :
              0.0;

      printf(
          "JoltPerfProbe: window=%d step_avg=%.3fms update_avg=%.3fms "
          "broadphase_frame_avg=%.3fms broadphase_calls=%zu broadphase_call_avg=%.3fms "
          "triggers[startup=%zu soft_single=%zu soft_batch=%zu rigid_single=%zu other=%zu] "
          "breakcheck_avg=%.3fms breakable_checked_avg=%.1f broken_total=%zu\n",
          m_perfProbeFramesAccum,
          stepAvgMS,
          updateAvgMS,
          broadphaseFrameAvgMS,
          m_perfProbeBroadphaseCallsAccum,
          broadphaseCallAvgMS,
          m_perfProbeBroadphaseStartupCallsAccum,
          m_perfProbeBroadphaseSoftSingleCallsAccum,
          m_perfProbeBroadphaseSoftBatchCallsAccum,
          m_perfProbeBroadphaseRigidSingleCallsAccum,
          m_perfProbeBroadphaseOtherCallsAccum,
          breakCheckAvgMS,
          breakableCheckedAvg,
          m_perfProbeBrokenAccum);

      printf(
          "JoltPerfProbeDetail: prep=%.3fms write_kinematic=%.3fms pinned_update=%.3fms "
          "deferred=%.3fms fh=%.3fms sim_tick=%.3fms sync=%.3fms chars=%.3fms "
          "vehicles=%.3fms callbacks=%.3fms controllers_avg=%.1f deferred_ops_avg=%.1f "
          "contacts_avg=%.1f pending_adds_avg[soft_single=%.1f soft_batch=%.1f rigid_single=%.1f]\n",
          prepAvgMS,
          writeKinematicAvgMS,
          pinnedUpdateAvgMS,
          deferredAvgMS,
          fhSpringAvgMS,
          simulationTickAvgMS,
          syncMotionAvgMS,
          characterAvgMS,
          vehicleAvgMS,
          callbackAvgMS,
          controllersAvg,
          deferredQueuedAvg,
          contactPairsAvg,
          pendingSoftSingleAddsAvg,
          pendingSoftBatchAddsAvg,
          pendingRigidSingleAddsAvg);

      printf(
          "JoltPerfProbeSoftBody: calls=%zu frame_avg=%.3fms call_avg=%.3fms "
          "call_breakdown[depsgraph=%.3fms filter=%.3fms mesh=%.3fms rel_tag=%.3fms] "
          "counts_per_call[total=%.1f candidates=%.1f updated=%.1f]\n",
          m_perfProbeUpdateSoftBodiesCallsAccum,
          updateSoftBodiesFrameAvgMS,
          updateSoftBodiesCallAvgMS,
          updateSoftBodiesDepsgraphCallAvgMS,
          updateSoftBodiesFilterCallAvgMS,
          updateSoftBodiesMeshCallAvgMS,
          updateSoftBodiesRelTagCallAvgMS,
          softBodiesTotalAvg,
          softBodiesCandidatesAvg,
          softBodiesUpdatedAvg);

      m_perfProbeFramesAccum = 0;
      m_perfProbeStepUSAccum = 0.0;
      m_perfProbeUpdateUSAccum = 0.0;
      m_perfProbeBroadphaseUSAccum = 0.0;
      m_perfProbeBreakCheckUSAccum = 0.0;
      m_perfProbePrepUSAccum = 0.0;
      m_perfProbeWriteKinematicUSAccum = 0.0;
      m_perfProbePinnedUpdateUSAccum = 0.0;
      m_perfProbeDeferredUSAccum = 0.0;
      m_perfProbeFhSpringUSAccum = 0.0;
      m_perfProbeSimulationTickUSAccum = 0.0;
      m_perfProbeSyncMotionUSAccum = 0.0;
      m_perfProbeCharacterUSAccum = 0.0;
      m_perfProbeVehicleUSAccum = 0.0;
      m_perfProbeCallbackUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesDepsgraphUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesFilterUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesMeshUSAccum = 0.0;
      m_perfProbeUpdateSoftBodiesRelTagUSAccum = 0.0;
      m_perfProbeBroadphaseCallsAccum = 0;
      m_perfProbeBroadphaseStartupCallsAccum = 0;
      m_perfProbeBroadphaseSoftSingleCallsAccum = 0;
      m_perfProbeBroadphaseSoftBatchCallsAccum = 0;
      m_perfProbeBroadphaseRigidSingleCallsAccum = 0;
      m_perfProbeBroadphaseOtherCallsAccum = 0;
      m_perfProbeBreakableCheckedAccum = 0;
      m_perfProbeBrokenAccum = 0;
      m_perfProbeDeferredOpsQueuedAccum = 0;
      m_perfProbeControllersAccum = 0;
      m_perfProbeContactPairsAccum = 0;
      m_perfProbePendingSoftSingleAddsAccum = 0;
      m_perfProbePendingSoftBatchAddsAccum = 0;
      m_perfProbePendingRigidSingleAddsAccum = 0;
      m_perfProbeUpdateSoftBodiesCallsAccum = 0;
      m_perfProbeSoftBodiesTotalAccum = 0;
      m_perfProbeSoftBodiesCandidatesAccum = 0;
      m_perfProbeSoftBodiesUpdatedAccum = 0;
    }
  }

  return true;
}

blender::Depsgraph *JoltPhysicsEnvironment::GetDepsgraph()
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  if (engine == nullptr) {
    return nullptr;
  }

  blender::bContext *C = engine->GetContext();
  if (C == nullptr) {
    return nullptr;
  }

  blender::Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  }

  return depsgraph;
}

blender::EffectorWeights *JoltPhysicsEnvironment::GetEffectorWeights()
{
  if (m_blenderScene == nullptr) {
    return nullptr;
  }

  blender::RigidBodyWorld *rbw = m_blenderScene->rigidbody_world;
  blender::EffectorWeights *weights = rbw ? rbw->effector_weights : nullptr;

  if (weights == nullptr) {
    if (m_fallbackEffectorWeights == nullptr) {
      m_fallbackEffectorWeights = blender::BKE_effector_add_weights(nullptr);
    }
    weights = m_fallbackEffectorWeights;
  }

  return weights;
}

void JoltPhysicsEnvironment::ApplyEffectorForces()
{
  if (m_blenderScene == nullptr) {
    return;
  }

  blender::Depsgraph *depsgraph = GetDepsgraph();
  if (depsgraph == nullptr) {
    return;
  }

  blender::EffectorWeights *effectorWeights = GetEffectorWeights();
  if (effectorWeights == nullptr) {
    return;
  }

  blender::ListBaseT<blender::EffectorCache> *effectors =
      blender::BKE_effectors_create(depsgraph, nullptr, nullptr, effectorWeights, false);
  if (effectors == nullptr) {
    return;
  }

  JPH::BodyInterface &bi = GetBodyInterface();

  for (JoltPhysicsController *ctrl : GetControllersForIteration()) {
    if (ctrl->GetSoftBody()) {
      continue;
    }

    const JPH::BodyID bodyID = ctrl->GetBodyID();
    if (bodyID.IsInvalid() || !bi.IsAdded(bodyID)) {
      continue;
    }

    JPH::RVec3 bodyPos;
    JPH::Vec3 bodyLinVel;
    {
      JPH::BodyLockRead bodyLock(m_physicsSystem->GetBodyLockInterface(), bodyID);
      if (!bodyLock.Succeeded()) {
        continue;
      }

      const JPH::Body &body = bodyLock.GetBody();
      if (!body.IsDynamic() || body.GetMotionProperties() == nullptr) {
        continue;
      }

      if (body.GetMotionProperties()->GetInverseMass() <= 0.0f) {
        continue;
      }

      bodyPos = body.GetCenterOfMassPosition();
      bodyLinVel = body.GetLinearVelocity();
    }

    KX_ClientObjectInfo *info = static_cast<KX_ClientObjectInfo *>(ctrl->GetNewClientInfo());
    if (info == nullptr) {
      continue;
    }

    KX_GameObject *gameobj = KX_GameObject::GetClientObject(info);
    if (gameobj == nullptr) {
      continue;
    }

    blender::Object *blenderobj = gameobj->GetBlenderObject();
    if (blenderobj == nullptr) {
      continue;
    }

    if (blenderobj->pd && blenderobj->pd->forcefield != blender::PFIELD_NULL) {
      continue;
    }

    /* Force-field sampling must track the game-object origin used by gameplay
     * transforms/spawn logic. For hidden template replicas with non-unit scale,
     * Jolt body COM can diverge from that origin after shape scaling, which can
     * move effector sampling outside field falloff and yield zero force. */
    PHY_IMotionState *motionState = ctrl->GetMotionState();
    const MT_Vector3 worldPos = motionState ? motionState->GetWorldPosition() :
                                              JoltMath::ToMT(bodyPos);
    const MT_Vector3 worldVel = JoltMath::ToMT(bodyLinVel);

    float effLoc[3] = {float(worldPos.x()), float(worldPos.y()), float(worldPos.z())};
    float effVel[3] = {float(worldVel.x()), float(worldVel.y()), float(worldVel.z())};

    blender::EffectedPoint effectedPoint;
    blender::pd_point_from_loc(m_blenderScene, effLoc, effVel, 0, &effectedPoint);

    float effForce[3] = {0.0f, 0.0f, 0.0f};
    blender::BKE_effectors_apply(
        effectors, nullptr, effectorWeights, &effectedPoint, effForce, nullptr, nullptr);

    if (!blender::is_zero_v3(effForce)) {
      bi.AddForce(bodyID,
                  JoltMath::ToJolt(effForce[0], effForce[1], effForce[2]),
                  JPH::EActivation::Activate);
    }
  }

  blender::BKE_effectors_free(effectors);
}

void JoltPhysicsEnvironment::RegisterControllerForObject(blender::Object *obj,
                                                         JoltPhysicsController *ctrl)
{
  if (obj) {
    m_controllerByBlenderObject[obj] = ctrl;
  }
}

JoltPhysicsController *JoltPhysicsEnvironment::FindControllerByBlenderObject(blender::Object *obj)
{
  if (!obj) {
    return nullptr;
  }
  auto it = m_controllerByBlenderObject.find(obj);
  return (it != m_controllerByBlenderObject.end()) ? it->second : nullptr;
}

void JoltPhysicsEnvironment::AddSoftBodyReplica(JoltSoftBody *sb,
                                                JoltPhysicsController *pinCtrl)
{
  if (!sb) {
    return;
  }

  /* Track in the soft body update list. */
  m_softBodies.push_back(sb);
  if (sb->HasPinnedVertices()) {
    m_pinnedSoftBodies.push_back(sb);
  }

  /* Register no-pin-collision if the flag is set and pin body is valid. */
  if (pinCtrl && sb->GetNoPinCollision() && !pinCtrl->GetBodyID().IsInvalid()) {
    m_softBodyContactListener.Register(sb->GetBodyID(), pinCtrl->GetBodyID());
  }
}

void JoltPhysicsEnvironment::QueueRigidBodyBodyAdd(JPH::BodyID bodyID,
                                                   JPH::EActivation activation)
{
  if (bodyID.IsInvalid()) {
    return;
  }

  PendingBodyAddEntry entry;
  entry.bodyID = bodyID;
  entry.activation = activation;
  m_pendingRigidBodyBodyAdds.push_back(entry);
}

void JoltPhysicsEnvironment::RemovePendingRigidBodyBodyAdd(JPH::BodyID bodyID)
{
  if (bodyID.IsInvalid() || m_pendingRigidBodyBodyAdds.empty()) {
    return;
  }

  auto newEnd = std::remove_if(
      m_pendingRigidBodyBodyAdds.begin(),
      m_pendingRigidBodyBodyAdds.end(),
      [bodyID](const PendingBodyAddEntry &entry) {
        return entry.bodyID == bodyID;
      });
  if (newEnd != m_pendingRigidBodyBodyAdds.end()) {
    m_pendingRigidBodyBodyAdds.erase(newEnd, m_pendingRigidBodyBodyAdds.end());
  }
}

void JoltPhysicsEnvironment::QueuePendingCompoundChildShape(JoltPhysicsController *parentCtrl,
                                                            const JPH::Vec3 &relativePos,
                                                            const JPH::Quat &relativeRot,
                                                            JPH::RefConst<JPH::Shape> childShape)
{
  if (!parentCtrl || !childShape) {
    return;
  }

  std::vector<PendingCompoundSubShapeEntry> &pendingSubShapes =
      m_pendingCompoundSubShapesByController[parentCtrl];

  if (pendingSubShapes.empty()) {
    JPH::RefConst<JPH::Shape> parentShape = parentCtrl->GetShape();
    if (!parentShape) {
      m_pendingCompoundSubShapesByController.erase(parentCtrl);
      return;
    }

    const JPH::Shape *rawParentShape = parentShape.GetPtr();
    if (rawParentShape->GetSubType() == JPH::EShapeSubType::StaticCompound) {
      const JPH::StaticCompoundShape *existingCompound =
          static_cast<const JPH::StaticCompoundShape *>(rawParentShape);
      pendingSubShapes.reserve(existingCompound->GetNumSubShapes() + 1);
      for (JPH::uint i = 0; i < existingCompound->GetNumSubShapes(); ++i) {
        const JPH::CompoundShape::SubShape &sub = existingCompound->GetSubShape(i);
        PendingCompoundSubShapeEntry entry;
        entry.position = sub.GetPositionCOM();
        entry.rotation = sub.GetRotation();
        entry.shape = sub.mShape;
        pendingSubShapes.push_back(entry);
      }
    }
    else {
      PendingCompoundSubShapeEntry rootEntry;
      rootEntry.position = JPH::Vec3::sZero();
      rootEntry.rotation = JPH::Quat::sIdentity();
      rootEntry.shape = parentShape;
      pendingSubShapes.push_back(rootEntry);
    }
  }

  PendingCompoundSubShapeEntry childEntry;
  childEntry.position = relativePos;
  childEntry.rotation = relativeRot;
  childEntry.shape = childShape;
  pendingSubShapes.push_back(childEntry);
}

void JoltPhysicsEnvironment::FinalizePendingCompoundShapeBuilds()
{
  if (m_pendingCompoundSubShapesByController.empty()) {
    return;
  }

  JPH::BodyInterface &bi = GetBodyInterface();

  for (auto &pair : m_pendingCompoundSubShapesByController) {
    JoltPhysicsController *parentCtrl = pair.first;
    std::vector<PendingCompoundSubShapeEntry> &subShapes = pair.second;
    if (!parentCtrl || subShapes.empty()) {
      continue;
    }

    const JPH::BodyID parentBodyID = parentCtrl->GetBodyID();
    if (parentBodyID.IsInvalid()) {
      continue;
    }

    JPH::RefConst<JPH::Shape> parentShapeAtFinalize = parentCtrl->GetShape();
    if (!parentShapeAtFinalize) {
      continue;
    }

    size_t baseShapeCount = 1;
    const JPH::Shape *parentRawShape = parentShapeAtFinalize.GetPtr();
    if (parentRawShape->GetSubType() == JPH::EShapeSubType::StaticCompound) {
      const JPH::StaticCompoundShape *existingCompound =
          static_cast<const JPH::StaticCompoundShape *>(parentRawShape);
      baseShapeCount = (size_t)existingCompound->GetNumSubShapes();
    }
    if (baseShapeCount > subShapes.size()) {
      baseShapeCount = subShapes.size();
    }

    JPH::StaticCompoundShapeSettings compoundSettings;
    for (const PendingCompoundSubShapeEntry &entry : subShapes) {
      if (!entry.shape) {
        continue;
      }
      compoundSettings.AddShape(entry.position, entry.rotation, entry.shape);
    }

    JPH::Shape::ShapeResult result = compoundSettings.Create();
    if (result.HasError()) {
      /* Fallback: preserve previous behavior by trying incremental child appends,
       * so an invalid late child does not discard all previously valid children. */
      JPH::RefConst<JPH::Shape> fallbackShape = parentShapeAtFinalize;
      for (size_t childIndex = baseShapeCount; childIndex < subShapes.size(); ++childIndex) {
        const PendingCompoundSubShapeEntry &childEntry = subShapes[childIndex];
        if (!childEntry.shape) {
          continue;
        }

        JPH::StaticCompoundShapeSettings incrementalSettings;
        const JPH::Shape *fallbackRaw = fallbackShape.GetPtr();
        if (fallbackRaw->GetSubType() == JPH::EShapeSubType::StaticCompound) {
          const JPH::StaticCompoundShape *existingCompound =
              static_cast<const JPH::StaticCompoundShape *>(fallbackRaw);
          for (JPH::uint i = 0; i < existingCompound->GetNumSubShapes(); ++i) {
            const JPH::CompoundShape::SubShape &sub = existingCompound->GetSubShape(i);
            incrementalSettings.AddShape(sub.GetPositionCOM(), sub.GetRotation(), sub.mShape);
          }
        }
        else {
          incrementalSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), fallbackShape);
        }

        incrementalSettings.AddShape(childEntry.position, childEntry.rotation, childEntry.shape);
        JPH::Shape::ShapeResult incrementalResult = incrementalSettings.Create();
        if (incrementalResult.HasError()) {
          continue;
        }

        fallbackShape = incrementalResult.Get();
        parentCtrl->SetShape(fallbackShape);
        bi.SetShape(parentBodyID, fallbackShape, true, JPH::EActivation::Activate);
      }
      continue;
    }

    JPH::RefConst<JPH::Shape> newCompound = result.Get();
    parentCtrl->SetShape(newCompound);
    bi.SetShape(parentBodyID, newCompound, true, JPH::EActivation::Activate);
  }

  m_pendingCompoundSubShapesByController.clear();
}

void JoltPhysicsEnvironment::FlushPendingRigidBodyBodyAdds()
{
  if (m_pendingRigidBodyBodyAdds.empty()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();

  auto flushRun = [&](std::vector<JPH::BodyID> &runIDs,
                      std::unordered_set<JPH::uint32> &runIDKeys,
                      JPH::EActivation activation) {
    if (runIDs.empty()) {
      return;
    }

    JPH::BodyID *bodyIDs = runIDs.data();
    const int bodyCount = (int)runIDs.size();
    JPH::BodyInterface::AddState addState = bi.AddBodiesPrepare(bodyIDs, bodyCount);
    if (addState) {
      bi.AddBodiesFinalize(bodyIDs, bodyCount, addState, activation);
    }
    else {
      /* Fallback path: preserve behavior if batched prepare fails. */
      for (int i = 0; i < bodyCount; ++i) {
        bi.AddBody(bodyIDs[i], activation);
      }

      /* Fallback used one-by-one insertion. Track this as a runtime add burst
       * so broadphase optimize can happen on the normal threshold/idle policy. */
      m_pendingRigidBodyAddsForOptimize += bodyCount;
      m_rigidBodyAddsSinceLastStep += bodyCount;
    }

    runIDs.clear();
    runIDKeys.clear();
  };

  std::vector<JPH::BodyID> runIDs;
  std::unordered_set<JPH::uint32> runIDKeys;
  runIDs.reserve(m_pendingRigidBodyBodyAdds.size());
  runIDKeys.reserve(m_pendingRigidBodyBodyAdds.size());
  JPH::EActivation runActivation = JPH::EActivation::DontActivate;
  bool hasRun = false;

  for (const PendingBodyAddEntry &entry : m_pendingRigidBodyBodyAdds) {
    if (entry.bodyID.IsInvalid() || bi.IsAdded(entry.bodyID)) {
      continue;
    }

    if (!hasRun) {
      runActivation = entry.activation;
      hasRun = true;
    }
    else if (entry.activation != runActivation) {
      flushRun(runIDs, runIDKeys, runActivation);
      runActivation = entry.activation;
    }

    if (runIDKeys.insert(entry.bodyID.GetIndexAndSequenceNumber()).second) {
      runIDs.push_back(entry.bodyID);
    }
  }

  if (hasRun) {
    flushRun(runIDs, runIDKeys, runActivation);
  }

  m_pendingRigidBodyBodyAdds.clear();
}

void JoltPhysicsEnvironment::QueueSoftBodyBodyAdd(JPH::BodyID bodyID)
{
  if (!bodyID.IsInvalid()) {
    m_pendingSoftBodyBodyAdds.push_back(bodyID);
  }
}

void JoltPhysicsEnvironment::NotifySoftBodyBodyAdded()
{
  ++m_pendingSoftBodyAddsForOptimize;
  ++m_softBodyAddsSinceLastStep;
}

void JoltPhysicsEnvironment::NotifyRigidBodyBodyAdded()
{
  ++m_pendingRigidBodyAddsForOptimize;
  ++m_rigidBodyAddsSinceLastStep;
}

void JoltPhysicsEnvironment::RequestSoftBodyRelationsTagUpdate()
{
  m_softBodyRelationsTagDirty = true;
}

void JoltPhysicsEnvironment::NotifyConstraintBreakingThresholdChanged()
{
  m_breakableConstraintsCacheDirty = true;
}

void JoltPhysicsEnvironment::RebuildBreakableConstraintCache()
{
  m_breakableConstraintsCache.clear();
  m_breakableConstraintsCache.reserve(m_constraintById.size());

  for (const auto &pair : m_constraintById) {
    JoltConstraint *constraint = pair.second;
    if (constraint && constraint->GetBreakingThreshold() < FLT_MAX) {
      m_breakableConstraintsCache.push_back(constraint);
    }
  }

  m_breakableConstraintsCacheDirty = false;
}

void JoltPhysicsEnvironment::FlushPendingSoftBodyBodyAdds()
{
  if (m_pendingSoftBodyBodyAdds.empty()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();
  JPH::BodyID *bodyIDs = m_pendingSoftBodyBodyAdds.data();
  const int bodyCount = (int)m_pendingSoftBodyBodyAdds.size();

  JPH::BodyInterface::AddState addState = bi.AddBodiesPrepare(bodyIDs, bodyCount);
  if (addState) {
    bi.AddBodiesFinalize(bodyIDs, bodyCount, addState, JPH::EActivation::Activate);
    m_pendingSoftBodyBatchAddsForOptimize += bodyCount;
  }
  else {
    /* Fallback path (defensive): preserve behavior even if AddBodiesPrepare fails. */
    for (int i = 0; i < bodyCount; ++i) {
      bi.AddBody(bodyIDs[i], JPH::EActivation::Activate);
    }
    m_pendingSoftBodyAddsForOptimize += bodyCount;
  }

  m_softBodyAddsSinceLastStep += bodyCount;
  m_pendingSoftBodyBodyAdds.clear();
}

void JoltPhysicsEnvironment::FlushPendingSoftBodyBodyRemoves()
{
  if (m_pendingSoftBodyBodyRemoves.empty()) {
    return;
  }

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();
  std::vector<JPH::BodyID> addedBodyIDs;
  std::vector<JPH::BodyID> destroyBodyIDs;
  addedBodyIDs.reserve(m_pendingSoftBodyBodyRemoves.size());
  destroyBodyIDs.reserve(m_pendingSoftBodyBodyRemoves.size());

  for (const JPH::BodyID &bodyID : m_pendingSoftBodyBodyRemoves) {
    if (bodyID.IsInvalid()) {
      continue;
    }

    destroyBodyIDs.push_back(bodyID);
    if (bi.IsAdded(bodyID)) {
      addedBodyIDs.push_back(bodyID);
    }
  }

  if (!addedBodyIDs.empty()) {
    bi.RemoveBodies(addedBodyIDs.data(), (int)addedBodyIDs.size());
  }
  if (!destroyBodyIDs.empty()) {
    bi.DestroyBodies(destroyBodyIDs.data(), (int)destroyBodyIDs.size());
  }

  m_pendingSoftBodyBodyRemoves.clear();
}

void JoltPhysicsEnvironment::RemoveSoftBody(JoltSoftBody *sb)
{
  if (!sb) {
    return;
  }

  const JPH::BodyID sbBodyID = sb->GetBodyID();
  if (!sbBodyID.IsInvalid()) {
    m_softBodyContactListener.Unregister(sbBodyID);
  }

  JoltPhysicsController *ctrl = sb->GetController();
  if (ctrl) {
    auto ctrlIt = m_controllers.find(ctrl);
    if (ctrlIt != m_controllers.end() && ctrl->GetSoftBody() == sb) {
      ctrl->SetSoftBody(nullptr);
      if (ctrl->GetBodyID() == sbBodyID) {
        ctrl->SetBodyID(JPH::BodyID());
      }
    }
  }

  auto it = std::find(m_softBodies.begin(), m_softBodies.end(), sb);
  if (it != m_softBodies.end()) {
    m_softBodies.erase(it);
  }

  auto pinnedIt = std::find(m_pinnedSoftBodies.begin(), m_pinnedSoftBodies.end(), sb);
  if (pinnedIt != m_pinnedSoftBodies.end()) {
    m_pinnedSoftBodies.erase(pinnedIt);
  }

  if (!sbBodyID.IsInvalid()) {
    auto pendingBegin = m_pendingSoftBodyBodyAdds.begin();
    auto pendingEnd = std::remove(pendingBegin, m_pendingSoftBodyBodyAdds.end(), sbBodyID);
    if (pendingEnd != m_pendingSoftBodyBodyAdds.end()) {
      m_pendingSoftBodyBodyAdds.erase(pendingEnd, m_pendingSoftBodyBodyAdds.end());
    }

    auto removeBegin = m_pendingSoftBodyBodyRemoves.begin();
    auto removeEnd = std::remove(removeBegin, m_pendingSoftBodyBodyRemoves.end(), sbBodyID);
    if (removeEnd != m_pendingSoftBodyBodyRemoves.end()) {
      m_pendingSoftBodyBodyRemoves.erase(removeEnd, m_pendingSoftBodyBodyRemoves.end());
    }
    m_pendingSoftBodyBodyRemoves.push_back(sbBodyID);

    if (!m_isPhysicsUpdating) {
      JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();
      if (bi.IsAdded(sbBodyID)) {
        bi.RemoveBody(sbBodyID);
      }
    }
  }

  sb->SetBodyDestructionHandledByEnvironment(true);
  delete sb;
}

void JoltPhysicsEnvironment::FinalizeSoftBodyPins()
{
  /* Startup conversion can queue rigid adds/compound updates before this method
   * is called from BL_DataConversion. Finalize them now to preserve startup
   * behavior while still batching conversion work. */
  FinalizePendingCompoundShapeBuilds();
  FlushPendingRigidBodyBodyAdds();

  m_pinnedSoftBodies.clear();

  for (JoltSoftBody *sb : m_softBodies) {
    if (!sb->HasPinnedVertices()) {
      continue;
    }
    m_pinnedSoftBodies.push_back(sb);

    /* Retrieve the pin object controller from the Blender object pointer stored during
     * ConvertObject. The lookup is safe even if pin_object has no physics body. */
    blender::Object *pinObj = sb->GetPinBlenderObject();
    if (pinObj) {
      JoltPhysicsController *pinCtrl = FindControllerByBlenderObject(pinObj);
      sb->SetPinController(pinCtrl); /* may be null — fixed-world-space pinning still works */

      /* If the "No Force on Pin Object" flag is set and the pin object has a
       * physics body, register the pair so the soft body contact listener can
       * zero out impulses transferred to the pin body. */
      if (pinCtrl && sb->GetNoPinCollision() && !pinCtrl->GetBodyID().IsInvalid()) {
        m_softBodyContactListener.Register(sb->GetBodyID(), pinCtrl->GetBodyID());
      }
    }
  }
}

void JoltPhysicsEnvironment::UpdateSoftBodies()
{
  using PerfClock = std::chrono::steady_clock;
  const bool perfProbeEnabled = m_perfProbeEnabled;
  const PerfClock::time_point updateSoftBodiesStart =
      perfProbeEnabled ? PerfClock::now() : PerfClock::time_point();
  double perfDepsgraphUS = 0.0;
  double perfFilterUS = 0.0;
  double perfMeshUS = 0.0;
  double perfRelTagUS = 0.0;
  size_t softBodiesTotal = m_softBodies.size();
  size_t softBodiesCandidates = 0;
  size_t softBodiesUpdated = 0;

  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *C = engine ? engine->GetContext() : nullptr;

  /* Fast path: no soft-body work and no pending relations refresh request. */
  if (m_softBodies.empty() && !m_softBodyRelationsTagDirty) {
    if (perfProbeEnabled) {
      const double updateSoftBodiesUS =
          (double)std::chrono::duration_cast<std::chrono::microseconds>(
              PerfClock::now() - updateSoftBodiesStart)
              .count();

      m_perfProbeUpdateSoftBodiesUSAccum += updateSoftBodiesUS;
      ++m_perfProbeUpdateSoftBodiesCallsAccum;
      m_perfProbeSoftBodiesTotalAccum += softBodiesTotal;
    }
    return;
  }

  JPH::BodyInterface &bi = m_physicsSystem->GetBodyInterfaceNoLock();

  /* Build candidate list first. This avoids depsgraph lookup entirely when
   * no soft body needs mesh update this frame. */
  PerfClock::time_point filterStart;
  if (perfProbeEnabled) {
    filterStart = PerfClock::now();
  }

  m_softBodiesToMeshUpdateScratch.clear();
  m_softBodyMeshUpdateIDsScratch.clear();
  m_softBodiesToMeshUpdateScratch.reserve(m_softBodies.size());
  m_softBodyMeshUpdateIDsScratch.reserve(m_softBodies.size());

  for (JoltSoftBody *sb : m_softBodies) {
    if (!sb) {
      continue;
    }
    if (!sb->IsActive()) {
      continue;
    }
    const JPH::BodyID bodyID = sb->GetBodyID();
    if (bodyID.IsInvalid()) {
      continue;
    }
    if (!bi.IsAdded(bodyID)) {
      continue;
    }
    if (!bi.IsActive(bodyID) && !sb->HasPinnedVertices() && sb->HasMeshUpload()) {
      continue;
    }

    m_softBodiesToMeshUpdateScratch.push_back(sb);
    m_softBodyMeshUpdateIDsScratch.push_back(bodyID);
  }

  softBodiesCandidates = m_softBodyMeshUpdateIDsScratch.size();

  if (perfProbeEnabled) {
    perfFilterUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                       PerfClock::now() - filterStart)
                       .count();
  }

  blender::Depsgraph *depsgraph = nullptr;
  if (!m_softBodyMeshUpdateIDsScratch.empty()) {
    PerfClock::time_point depsgraphStart;
    if (perfProbeEnabled) {
      depsgraphStart = PerfClock::now();
    }

    if (C) {
      depsgraph = CTX_data_depsgraph_pointer(C);
      if (!depsgraph) {
        depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
      }
    }

    if (perfProbeEnabled) {
      perfDepsgraphUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                           PerfClock::now() - depsgraphStart)
                           .count();
    }

    if (depsgraph) {
      PerfClock::time_point meshStart;
      if (perfProbeEnabled) {
        meshStart = PerfClock::now();
      }

      const JPH::BodyLockInterface &lockIf =
          m_physicsSystem->GetBodyLockInterfaceNoLock();
      JPH::BodyLockMultiRead multiLock(lockIf,
                                       m_softBodyMeshUpdateIDsScratch.data(),
                                       (int)m_softBodyMeshUpdateIDsScratch.size());

      for (int i = 0; i < (int)m_softBodyMeshUpdateIDsScratch.size(); ++i) {
        const JPH::Body *body = multiLock.GetBody(i);
        if (!body) {
          continue;
        }
        m_softBodiesToMeshUpdateScratch[(size_t)i]->UpdateMeshLocked(depsgraph, *body);
        ++softBodiesUpdated;
      }

      if (perfProbeEnabled) {
        perfMeshUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                        PerfClock::now() - meshStart)
                        .count();
      }
    }
  }

  if (m_softBodyRelationsTagDirty && C) {
    PerfClock::time_point relTagStart;
    if (perfProbeEnabled) {
      relTagStart = PerfClock::now();
    }

    DEG_relations_tag_update(CTX_data_main(C));
    m_softBodyRelationsTagDirty = false;

    if (perfProbeEnabled) {
      perfRelTagUS = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                          PerfClock::now() - relTagStart)
                          .count();
    }
  }

  if (perfProbeEnabled) {
    const double updateSoftBodiesUS =
        (double)std::chrono::duration_cast<std::chrono::microseconds>(
            PerfClock::now() - updateSoftBodiesStart)
            .count();

    m_perfProbeUpdateSoftBodiesUSAccum += updateSoftBodiesUS;
    m_perfProbeUpdateSoftBodiesDepsgraphUSAccum += perfDepsgraphUS;
    m_perfProbeUpdateSoftBodiesFilterUSAccum += perfFilterUS;
    m_perfProbeUpdateSoftBodiesMeshUSAccum += perfMeshUS;
    m_perfProbeUpdateSoftBodiesRelTagUSAccum += perfRelTagUS;
    ++m_perfProbeUpdateSoftBodiesCallsAccum;
    m_perfProbeSoftBodiesTotalAccum += softBodiesTotal;
    m_perfProbeSoftBodiesCandidatesAccum += softBodiesCandidates;
    m_perfProbeSoftBodiesUpdatedAccum += softBodiesUpdated;
  }
}

void JoltPhysicsEnvironment::DebugDrawWorld()
{
  if (m_debugMode <= 0) {
    return;
  }
  JoltDebugDraw::DrawBodies(this);
  JoltDebugDraw::DrawConstraints(this);
}

void JoltPhysicsEnvironment::SetFixedTimeStep(bool useFixedTimeStep, float fixedTimeStep)
{
  if (useFixedTimeStep && fixedTimeStep > 0.0f) {
    m_fixedTimeStep = fixedTimeStep;
    /* Compute substeps: a 60Hz ticRate with fixedTimeStep=1/120 needs 2 substeps. */
    float ticRate = 1.0f / 60.0f;
    int substeps = (int)std::ceil(ticRate / fixedTimeStep);
    if (substeps < 1) substeps = 1;
    m_numTimeSubSteps = substeps;
  }
  else {
    m_fixedTimeStep = 0.0f;
  }
}

float JoltPhysicsEnvironment::GetFixedTimeStep()
{
  return m_fixedTimeStep;
}

int JoltPhysicsEnvironment::GetDebugMode() const
{
  return m_debugMode;
}

void JoltPhysicsEnvironment::SetDebugMode(int debugMode)
{
  m_debugMode = debugMode;
}

void JoltPhysicsEnvironment::SetNumIterations(int numIter)
{
  if (numIter <= 0) {
    return;
  }

  /* Map iterative solver quality to Jolt's solver steps.
   * Keep Bullet-like behavior where one value controls iteration strength,
   * with position steps following Jolt's default 10:2 velocity/position ratio. */
  const int velocitySteps = std::max(1, numIter);
  const int positionSteps = std::max(1, (velocitySteps + 4) / 5);

  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mNumVelocitySteps = (JPH::uint)velocitySteps;
  settings.mNumPositionSteps = (JPH::uint)positionSteps;
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetNumTimeSubSteps(int numTimeSubSteps)
{
  m_numTimeSubSteps = numTimeSubSteps;
}

int JoltPhysicsEnvironment::GetNumTimeSubSteps()
{
  return m_numTimeSubSteps;
}

void JoltPhysicsEnvironment::SetDeactivationTime(float dTime)
{
  m_deactivationTime = dTime;
  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mTimeBeforeSleep = dTime;
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetDeactivationLinearTreshold(float linTresh)
{
  m_linearDeactivationThreshold = linTresh;
  /* Jolt uses a single point velocity threshold. Use the larger of linear/angular. */
  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mPointVelocitySleepThreshold = std::max(m_linearDeactivationThreshold,
                                                    m_angularDeactivationThreshold);
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetDeactivationAngularTreshold(float angTresh)
{
  m_angularDeactivationThreshold = angTresh;
  /* Jolt uses a single point velocity threshold. Use the larger of linear/angular. */
  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mPointVelocitySleepThreshold = std::max(m_linearDeactivationThreshold,
                                                    m_angularDeactivationThreshold);
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetERPNonContact(float erp)
{
  /* Map to Jolt's Baumgarte stabilization factor.
   * ERP in Bullet is typically 0.2-0.8, Jolt's mBaumgarte is 0.0-1.0.
   * Clamp to reasonable range to prevent instability. */
  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mBaumgarte = std::clamp(erp, 0.1f, 0.8f);
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetERPContact(float erp2)
{
  /* Map to Jolt's Baumgarte stabilization factor for contacts.
   * Jolt doesn't separate contact/non-contact ERP like Bullet does.
   * We use the same setting, but prioritize contact ERP if both are set. */
  JPH::PhysicsSettings settings = m_physicsSystem->GetPhysicsSettings();
  settings.mBaumgarte = std::clamp(erp2, 0.1f, 0.8f);
  m_physicsSystem->SetPhysicsSettings(settings);
}

void JoltPhysicsEnvironment::SetCFM(float cfm)
{
  /* No direct Jolt equivalent. Store for reference. */
}

void JoltPhysicsEnvironment::SetContactBreakingTreshold(float contactBreakingTreshold)
{
  /* No direct Jolt mapping. Jolt manages contact cache internally. */
}

void JoltPhysicsEnvironment::SetSolverSorConstant(float sor)
{
  /* No Jolt equivalent. */
}

void JoltPhysicsEnvironment::SetSolverType(PHY_SolverType solverType)
{
  /* Jolt has a single solver type. */
}

void JoltPhysicsEnvironment::SetSolverTau(float tau)
{
  /* No Jolt equivalent. */
}

void JoltPhysicsEnvironment::SetSolverDamping(float damping)
{
  /* No Jolt equivalent. */
}

void JoltPhysicsEnvironment::SetGravity(float x, float y, float z)
{
  /* Convert from Blender Z-up to Jolt Y-up. */
  JPH::Vec3 g = JoltMath::ToJolt(x, y, z);
  m_physicsSystem->SetGravity(g);
}

void JoltPhysicsEnvironment::GetGravity(MT_Vector3 &grav)
{
  JPH::Vec3 g = m_physicsSystem->GetGravity();
  grav = JoltMath::ToMT(g);
}

PHY_IConstraint *JoltPhysicsEnvironment::CreateConstraint(PHY_IPhysicsController *ctrl,
                                                          PHY_IPhysicsController *ctrl2,
                                                          PHY_ConstraintType type,
                                                          float pivotX,
                                                          float pivotY,
                                                          float pivotZ,
                                                          float axisX,
                                                          float axisY,
                                                          float axisZ,
                                                          float axis1X,
                                                          float axis1Y,
                                                          float axis1Z,
                                                          float axis2X,
                                                          float axis2Y,
                                                          float axis2Z,
                                                          int flag,
                                                          bool replicate_dupli)
{
  if (!m_isPhysicsUpdating) {
    FinalizePendingCompoundShapeBuilds();
    FlushPendingRigidBodyBodyAdds();
  }

  bool disableCollision = (0 != (flag & CCD_CONSTRAINT_DISABLE_LINKED_COLLISION));

  JoltPhysicsController *c0 = static_cast<JoltPhysicsController *>(ctrl);
  JoltPhysicsController *c1 = static_cast<JoltPhysicsController *>(ctrl2);

  if (!c0 || c0->GetBodyID().IsInvalid()) {
    return nullptr;
  }

  JPH::BodyInterface &bi = GetBodyInterface();
  JPH::BodyID bodyID0 = c0->GetBodyID();
  JPH::BodyID bodyID1 = (c1 && !c1->GetBodyID().IsInvalid()) ? c1->GetBodyID() : JPH::BodyID();

  /* Pivot in body-local space of body0 (convert from Blender Z-up to Jolt Y-up). */
  JPH::Vec3 pivotLocal = JoltMath::ToJolt(pivotX, pivotY, pivotZ);
  JPH::Vec3 axisIn = JoltMath::ToJolt(axisX, axisY, axisZ);
  if (axisIn.LengthSq() < 1e-6f) {
    axisIn = JPH::Vec3::sAxisY();  /* Default axis = Jolt Y (Blender Z). */
  }
  else {
    axisIn = axisIn.Normalized();
  }

  /* Compute pivot in world space from body0's transform. */
  JPH::RVec3 pos0 = bi.GetCenterOfMassPosition(bodyID0);
  JPH::Quat rot0 = bi.GetRotation(bodyID0);
  JPH::RVec3 pivotWorld = pos0 + rot0 * pivotLocal;

  /* Compute pivot in body1's local space. */
  JPH::Vec3 pivotLocal1;
  if (!bodyID1.IsInvalid()) {
    JPH::RVec3 pos1 = bi.GetCenterOfMassPosition(bodyID1);
    JPH::Quat rot1 = bi.GetRotation(bodyID1);
    pivotLocal1 = JPH::Vec3(rot1.Conjugated() * (pivotWorld - pos1));
  }
  else {
    pivotLocal1 = JPH::Vec3(pivotWorld);
  }

  /* Get body references for constraint creation.
   * Use the no-lock interface to avoid deadlocks: taking two sequential
   * BodyLockWrite can deadlock when both body IDs hash to the same mutex
   * bucket ("Resource deadlock avoided"). Constraint creation happens on
   * the main thread during scene setup, not during physics stepping, so
   * the no-lock interface is safe here. */
  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterfaceNoLock();

  JPH::BodyLockWrite lock0(lockInterface, bodyID0);
  if (!lock0.Succeeded()) {
    return nullptr;
  }
  JPH::Body &body0 = lock0.GetBody();

  /* Optional second body. */
  JPH::Body *body1Ptr = &JPH::Body::sFixedToWorld;
  JPH::BodyLockWrite lock1(lockInterface, !bodyID1.IsInvalid() ? bodyID1 : JPH::BodyID());
  if (!bodyID1.IsInvalid()) {
    if (!lock1.Succeeded()) {
      return nullptr;
    }
    body1Ptr = &lock1.GetBody();
  }
  JPH::Body &body1 = *body1Ptr;

  JPH::Constraint *joltConstraint = nullptr;

  switch (type) {
    case PHY_POINT2POINT_CONSTRAINT: {
      JPH::PointConstraintSettings settings;
      if (!bodyID1.IsInvalid()) {
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = pivotLocal;
        settings.mPoint2 = pivotLocal1;
      }
      else {
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        settings.mPoint1 = JPH::Vec3(pivotWorld);
        settings.mPoint2 = JPH::Vec3(pivotWorld);
      }
      joltConstraint = settings.Create(body0, body1);
      break;
    }

    case PHY_LINEHINGE_CONSTRAINT:
    case PHY_ANGULAR_CONSTRAINT: {
      JPH::HingeConstraintSettings settings;
      if (!bodyID1.IsInvalid()) {
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = pivotLocal;
        settings.mHingeAxis1 = axisIn;
        settings.mNormalAxis1 = axisIn.GetNormalizedPerpendicular();
        JPH::Quat rot1 = bi.GetRotation(bodyID1);
        JPH::Vec3 worldAxis = rot0 * axisIn;
        settings.mPoint2 = pivotLocal1;
        settings.mHingeAxis2 = rot1.Conjugated() * worldAxis;
        settings.mNormalAxis2 = settings.mHingeAxis2.GetNormalizedPerpendicular();
      }
      else {
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        JPH::Vec3 worldAxis = rot0 * axisIn;
        settings.mPoint1 = JPH::Vec3(pivotWorld);
        settings.mHingeAxis1 = worldAxis;
        settings.mNormalAxis1 = worldAxis.GetNormalizedPerpendicular();
        settings.mPoint2 = settings.mPoint1;
        settings.mHingeAxis2 = settings.mHingeAxis1;
        settings.mNormalAxis2 = settings.mNormalAxis1;
      }
      joltConstraint = settings.Create(body0, body1);
      break;
    }

    case PHY_CONE_TWIST_CONSTRAINT: {
      JPH::ConeConstraintSettings settings;
      settings.mHalfConeAngle = JPH::JPH_PI * 0.25f;  /* Default 45 degrees. */
      if (!bodyID1.IsInvalid()) {
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPoint1 = pivotLocal;
        settings.mTwistAxis1 = axisIn;
        JPH::Quat rot1 = bi.GetRotation(bodyID1);
        JPH::Vec3 worldAxis = rot0 * axisIn;
        settings.mPoint2 = pivotLocal1;
        settings.mTwistAxis2 = rot1.Conjugated() * worldAxis;
      }
      else {
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        JPH::Vec3 worldAxis = rot0 * axisIn;
        settings.mPoint1 = JPH::Vec3(pivotWorld);
        settings.mTwistAxis1 = worldAxis;
        settings.mPoint2 = settings.mPoint1;
        settings.mTwistAxis2 = settings.mTwistAxis1;
      }
      joltConstraint = settings.Create(body0, body1);
      break;
    }

    case PHY_GENERIC_6DOF_CONSTRAINT:
    case PHY_GENERIC_6DOF_SPRING2_CONSTRAINT: {
      JPH::SixDOFConstraintSettings settings;

      /* Use Pyramid swing type so Y/Z rotation limits can be asymmetric
       * (e.g. -45° to 30°). The default Cone mode requires symmetric
       * limits in [0, π] which doesn't match Blender's convention. */
      settings.mSwingType = JPH::ESwingType::Pyramid;

      /* Build rotation frame from provided axes. */
      JPH::Vec3 ax1 = JoltMath::ToJolt(axis1X, axis1Y, axis1Z);
      JPH::Vec3 ax2 = JoltMath::ToJolt(axis2X, axis2Y, axis2Z);
      if (ax1.LengthSq() < 1e-6f) {
        ax1 = axisIn.GetNormalizedPerpendicular();
        ax2 = axisIn.Cross(ax1).Normalized();
      }
      else {
        ax1 = ax1.Normalized();
        if (ax2.LengthSq() < 1e-6f) {
          ax2 = axisIn.Cross(ax1).Normalized();
        }
        else {
          ax2 = ax2.Normalized();
        }
      }

      if (!bodyID1.IsInvalid()) {
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPosition1 = pivotLocal;
        settings.mPosition2 = pivotLocal1;
        settings.mAxisX1 = axisIn;
        settings.mAxisY1 = ax1;
        JPH::Quat rot1 = bi.GetRotation(bodyID1);
        JPH::Vec3 worldAxisX = rot0 * axisIn;
        JPH::Vec3 worldAxisY = rot0 * ax1;
        settings.mAxisX2 = rot1.Conjugated() * worldAxisX;
        settings.mAxisY2 = rot1.Conjugated() * worldAxisY;
      }
      else {
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        settings.mPosition1 = JPH::Vec3(pivotWorld);
        settings.mPosition2 = settings.mPosition1;
        JPH::Vec3 worldAxisX = rot0 * axisIn;
        JPH::Vec3 worldAxisY = rot0 * ax1;
        settings.mAxisX1 = worldAxisX;
        settings.mAxisY1 = worldAxisY;
        settings.mAxisX2 = settings.mAxisX1;
        settings.mAxisY2 = settings.mAxisY1;
      }

      /* By default all axes are free. Limits are set via SetParam(). */
      joltConstraint = settings.Create(body0, body1);
      break;
    }

    default:
      return nullptr;
  }

  if (!joltConstraint) {
    return nullptr;
  }

  /* Add constraint to physics system. */
  m_physicsSystem->AddConstraint(joltConstraint);

  /* Register no-collide pair in the group filter so the bodies skip
   * narrow-phase collision detection. */
  if (disableCollision && !bodyID1.IsInvalid()) {
    m_constraintGroupFilter->DisableCollision(
        bodyID0.GetIndexAndSequenceNumber(),
        bodyID1.GetIndexAndSequenceNumber());
  }

  /* Create wrapper. */
  int uid = g_joltConstraintUid++;
  JoltConstraint *wrapper = new JoltConstraint(
      joltConstraint, type, uid, disableCollision, this);

  /* Track constraint for O(1) lookup by ID. */
  m_constraintById[uid] = wrapper;
  m_breakableConstraintsCacheDirty = true;

  return wrapper;
}

PHY_IVehicle *JoltPhysicsEnvironment::CreateVehicle(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl || joltCtrl->GetBodyID().IsInvalid()) {
    return nullptr;
  }

  int uid = g_joltConstraintUid++;
  JoltVehicle *vehicle = new JoltVehicle(joltCtrl, this, uid);
  m_vehicles.push_back(vehicle);
  return vehicle;
}

void JoltPhysicsEnvironment::RemoveConstraintById(int constraintid, bool free)
{
  auto it = m_constraintById.find(constraintid);
  if (it == m_constraintById.end()) {
    return;
  }

  JoltConstraint *wrapper = it->second;
  JPH::Constraint *joltCon = wrapper->GetConstraint();

  /* Remove no-collide pair from group filter before removing the constraint,
   * while the body references are still valid. */
  if (wrapper->GetDisableCollision() && joltCon) {
    JPH::TwoBodyConstraint *tbc = static_cast<JPH::TwoBodyConstraint *>(joltCon);
    m_constraintGroupFilter->EnableCollision(
        tbc->GetBody1()->GetID().GetIndexAndSequenceNumber(),
        tbc->GetBody2()->GetID().GetIndexAndSequenceNumber());
  }

  if (joltCon) {
    m_physicsSystem->RemoveConstraint(joltCon);
  }

  if (free) {
    delete wrapper;
  }

  m_constraintById.erase(it);
  m_breakableConstraintsCacheDirty = true;
}

bool JoltPhysicsEnvironment::IsRigidBodyConstraintEnabled(int constraintid)
{
  auto it = m_constraintById.find(constraintid);
  if (it == m_constraintById.end()) {
    return false;
  }
  return it->second->GetEnabled();
}

float JoltPhysicsEnvironment::GetAppliedImpulse(int constraintid)
{
  /* No direct Jolt equivalent; return 0 or estimate from contacts. */
  return 0.0f;
}

PHY_IVehicle *JoltPhysicsEnvironment::GetVehicleConstraint(int constraintId)
{
  for (JoltVehicle *veh : m_vehicles) {
    if (veh->GetUserConstraintId() == constraintId) {
      return veh;
    }
  }
  return nullptr;
}

PHY_ICharacter *JoltPhysicsEnvironment::GetCharacterController(KX_GameObject *ob)
{
  auto it = m_characterByObject.find(ob);
  if (it != m_characterByObject.end()) {
    return it->second;
  }
  return nullptr;
}

PHY_IPhysicsController *JoltPhysicsEnvironment::RayTest(
    PHY_IRayCastFilterCallback &filterCallback,
    float fromX,
    float fromY,
    float fromZ,
    float toX,
    float toY,
    float toZ)
{
  MT_Vector3 fromMT(fromX, fromY, fromZ);
  MT_Vector3 toMT(toX, toY, toZ);
  MT_Vector3 dir = toMT - fromMT;

  JPH::RVec3 origin = JoltMath::ToJolt(fromMT);
  JPH::Vec3 direction = JoltMath::ToJolt(dir);

  JPH::RRayCast ray(origin, direction);
  JPH::RayCastResult hit;

  const JPH::NarrowPhaseQuery &npq = m_physicsSystem->GetNarrowPhaseQuery();
  if (!npq.CastRay(ray, hit)) {
    return nullptr;
  }

  /* Look up the body and its controller. */
  JPH::BodyLockRead lock(m_physicsSystem->GetBodyLockInterface(), hit.mBodyID);
  if (!lock.Succeeded()) {
    return nullptr;
  }
  const JPH::Body &body = lock.GetBody();

  /* Find the JoltPhysicsController associated with this body via user data. */
  void *userData = reinterpret_cast<void *>(body.GetUserData());
  KX_ClientObjectInfo *clientInfo = static_cast<KX_ClientObjectInfo *>(userData);
  if (!clientInfo) {
    return nullptr;
  }

  PHY_IPhysicsController *ctrl = static_cast<PHY_IPhysicsController *>(
      clientInfo->m_gameobject->GetPhysicsController());

  if (!ctrl) {
    return nullptr;
  }

  /* Apply broadphase filter callback. */
  if (!filterCallback.needBroadphaseRayCast(ctrl)) {
    return nullptr;
  }

  /* Fill result and report hit. */
  JPH::RVec3 hitPoint = ray.GetPointOnRay(hit.mFraction);
  JPH::Vec3 hitNormal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPoint);

  PHY_RayCastResult result;
  result.m_controller = ctrl;
  result.m_hitPoint = JoltMath::ToMT(JPH::Vec3(hitPoint));
  result.m_hitNormal = JoltMath::ToMT(hitNormal);

  filterCallback.reportHit(&result);

  return ctrl;
}

bool JoltPhysicsEnvironment::CullingTest(PHY_CullingCallback callback,
                                          void *userData,
                                          const std::array<MT_Vector4, 6> &planes,
                                          int occlusionRes,
                                          const int *viewport,
                                          const MT_Matrix4x4 &matrix)
{
  /* Iterate all active graphic controllers and test their AABBs against
   * the frustum planes. This is a simple CPU-side frustum cull matching
   * Bullet's CcdGraphicController approach. */
  for (JoltGraphicController *gc : m_graphicControllers) {
    void *clientInfo = gc->GetNewClientInfo();
    if (!clientInfo) {
      continue;
    }

    /* The callback decides whether to render based on AABB vs frustum. */
    callback(static_cast<KX_ClientObjectInfo *>(clientInfo), userData);
  }
  return true;
}

void JoltPhysicsEnvironment::AddSensor(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl || joltCtrl->GetBodyID().IsInvalid()) {
    return;
  }

  if (joltCtrl->Register()) {
    GetBodyInterface().AddBody(joltCtrl->GetBodyID(), JPH::EActivation::Activate);
    NotifyRigidBodyBodyAdded();
  }
}

void JoltPhysicsEnvironment::RemoveSensor(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl || joltCtrl->GetBodyID().IsInvalid()) {
    return;
  }

  if (joltCtrl->Unregister()) {
    /* If physics is currently updating, defer body removal. */
    if (m_isPhysicsUpdating) {
      JoltDeferredOp op;
      op.type = JoltDeferredOpType::RemoveBody;
      op.bodyID = joltCtrl->GetBodyID();
      op.controller = joltCtrl;
      QueueDeferredOperation(op);
    }
    else {
      GetBodyInterface().RemoveBody(joltCtrl->GetBodyID());
    }
  }
}

void JoltPhysicsEnvironment::AddCollisionCallback(int response_class,
                                                   PHY_ResponseCallback callback,
                                                   void *user)
{
  if (response_class >= 0 && response_class < PHY_NUM_RESPONSE) {
    m_triggerCallbacks[response_class] = callback;
    m_triggerCallbacksUserPtrs[response_class] = user;
  }
}

bool JoltPhysicsEnvironment::RequestCollisionCallback(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl) {
    return false;
  }
  /* Register the controller for collision callback notifications.
   * The actual callback invocation happens in CallbackTriggers(). */
  m_collisionCallbackControllers.insert(joltCtrl);
  m_collectContactsForCallbacks.store(true, std::memory_order_relaxed);
  return true;
}

bool JoltPhysicsEnvironment::RemoveCollisionCallback(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl) {
    return false;
  }
  m_collisionCallbackControllers.erase(joltCtrl);
  const bool collectContacts = !m_collisionCallbackControllers.empty();
  m_collectContactsForCallbacks.store(collectContacts, std::memory_order_relaxed);

  if (!collectContacts) {
    /* Drop any pending pairs from the previous frame(s) now that nobody listens. */
    m_contactPairsScratch.clear();
    m_contactListener.SwapContacts(m_contactPairsScratch);
    m_contactPairsScratch.clear();
  }

  return true;
}

PHY_CollisionTestResult JoltPhysicsEnvironment::CheckCollision(PHY_IPhysicsController *ctrl0,
                                                                PHY_IPhysicsController *ctrl1)
{
  PHY_CollisionTestResult result{false, false, nullptr};

  JoltPhysicsController *jc0 = static_cast<JoltPhysicsController *>(ctrl0);
  JoltPhysicsController *jc1 = static_cast<JoltPhysicsController *>(ctrl1);
  if (!jc0 || !jc1 || jc0->GetBodyID().IsInvalid() || jc1->GetBodyID().IsInvalid()) {
    return result;
  }

  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();
  JPH::BodyID ids[2] = {jc0->GetBodyID(), jc1->GetBodyID()};
  JPH::BodyLockMultiRead multiLock(lockInterface, ids, 2);
  const JPH::Body *body0 = multiLock.GetBody(0);
  const JPH::Body *body1 = multiLock.GetBody(1);
  if (!body0 || !body1) {
    return result;
  }

  /* Use CollideShape to check if the two shapes overlap. */
  JPH::CollideShapeSettings collideSettings;
  collideSettings.mMaxSeparationDistance = 0.0f;

  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

  const JPH::NarrowPhaseQuery &npq = m_physicsSystem->GetNarrowPhaseQuery();
  const JPH::Shape *shape0 = body0->GetShape();

  npq.CollideShape(
      shape0,
      JPH::Vec3::sReplicate(1.0f),
      body0->GetCenterOfMassTransform(),
      collideSettings,
      JPH::RVec3::sZero(),
      collector,
      {},  /* broadphase layer filter */
      {},  /* object layer filter */
      {},  /* body filter */
      {}   /* shape filter */
  );

  /* Check if body1 is among the hits. */
  for (const JPH::CollideShapeResult &hit : collector.mHits) {
    if (hit.mBodyID2 == body1->GetID()) {
      result.collide = true;
      result.isFirst = true;

      JoltCollData *collData = new JoltCollData();
      MT_Vector3 contactPt = JoltMath::ToMT(JPH::Vec3(hit.mContactPointOn2));
      MT_Vector3 normal = JoltMath::ToMT(hit.mPenetrationAxis.Normalized());
      collData->AddContactPoint(contactPt, contactPt, contactPt, normal, 0.5f, 0.0f);
      result.collData = collData;
      break;
    }
  }

  return result;
}

PHY_IPhysicsController *JoltPhysicsEnvironment::CreateSphereController(
    float radius, const MT_Vector3 &position)
{
  /* Create a sensor body with SphereShape for Near/Radar sensor. */
  JPH::SphereShapeSettings shapeSettings(std::max(radius, 0.001f));
  JPH::Shape::ShapeResult result = shapeSettings.Create();
  if (result.HasError()) {
    return nullptr;
  }

  JPH::BodyCreationSettings bodySettings(
      result.Get(),
      JoltMath::ToJolt(position),
      JPH::Quat::sIdentity(),
      JPH::EMotionType::Kinematic,
      JoltMakeObjectLayer(0xFFFF, 0xFFFF, JOLT_BP_SENSOR));
  bodySettings.mIsSensor = true;

  JPH::BodyInterface &bi = GetBodyInterface();
  JPH::Body *body = bi.CreateBody(bodySettings);
  if (!body) {
    return nullptr;
  }

  JoltDefaultMotionState *ms = new JoltDefaultMotionState();
  ms->SetWorldPosition(position);

  JoltPhysicsController *ctrl = new JoltPhysicsController();
  ctrl->SetBodyID(body->GetID());
  ctrl->SetMotionState(ms);
  ctrl->SetEnvironment(this);
  ctrl->SetSensorFlag(true);
  ctrl->SetOriginalMotionType(JPH::EMotionType::Kinematic);
  ctrl->SetBroadPhaseCategory(JOLT_BP_SENSOR);
  ctrl->SetShape(result.Get());

  AddController(ctrl);
  /* Don't add body yet — it gets added when AddSensor() is called. */

  return ctrl;
}

PHY_IPhysicsController *JoltPhysicsEnvironment::CreateConeController(float coneradius,
                                                                      float coneheight)
{
  /* Create a sensor body with an approximate cone shape (convex hull). */
  JoltShapeBuilder builder;
  builder.SetShapeType(PHY_SHAPE_CONE);
  builder.SetRadius(coneradius);
  builder.SetHeight(coneheight);

  JPH::RefConst<JPH::Shape> shape = builder.Build();
  if (!shape) {
    return nullptr;
  }

  JPH::BodyCreationSettings bodySettings(
      shape,
      JPH::RVec3::sZero(),
      JPH::Quat::sIdentity(),
      JPH::EMotionType::Kinematic,
      JoltMakeObjectLayer(0xFFFF, 0xFFFF, JOLT_BP_SENSOR));
  bodySettings.mIsSensor = true;

  JPH::BodyInterface &bi = GetBodyInterface();
  JPH::Body *body = bi.CreateBody(bodySettings);
  if (!body) {
    return nullptr;
  }

  JoltDefaultMotionState *ms = new JoltDefaultMotionState();

  JoltPhysicsController *ctrl = new JoltPhysicsController();
  ctrl->SetBodyID(body->GetID());
  ctrl->SetMotionState(ms);
  ctrl->SetEnvironment(this);
  ctrl->SetSensorFlag(true);
  ctrl->SetOriginalMotionType(JPH::EMotionType::Kinematic);
  ctrl->SetBroadPhaseCategory(JOLT_BP_SENSOR);
  ctrl->SetShape(shape);

  AddController(ctrl);

  return ctrl;
}

void JoltPhysicsEnvironment::MergeEnvironment(PHY_IPhysicsEnvironment *other_env)
{
  JoltPhysicsEnvironment *other = static_cast<JoltPhysicsEnvironment *>(other_env);
  if (!other) {
    return;
  }

  FinalizePendingCompoundShapeBuilds();
  other->FinalizePendingCompoundShapeBuilds();

  /* Ensure queued body operations are materialized before transferring state. */
  FlushPendingRigidBodyBodyAdds();
  other->FlushPendingRigidBodyBodyAdds();
  FlushPendingSoftBodyBodyRemoves();
  other->FlushPendingSoftBodyBodyRemoves();

  /* Transfer all controllers from the other environment to this one.
   * In Jolt, bodies cannot be shared between PhysicsSystem instances.
   * We remove from the source system and recreate in the destination using
   * saved body creation settings. */
  JPH::BodyInterface &srcBI = other->GetBodyInterface();
  JPH::BodyInterface &dstBI = this->GetBodyInterface();

  /* Build a mapping from old BodyID → new BodyID for constraint recreation. */
  std::unordered_map<uint32_t, JPH::BodyID> bodyIDMap;

  std::set<JoltPhysicsController *> controllersToMove(other->m_controllers);
  for (JoltPhysicsController *ctrl : controllersToMove) {
    JPH::BodyID oldID = ctrl->GetBodyID();
    if (oldID.IsInvalid()) {
      continue;
    }

    /* Lock source body to get its creation settings. */
    const JPH::BodyLockInterface &srcLock = other->m_physicsSystem->GetBodyLockInterface();
    JPH::BodyLockRead lock(srcLock, oldID);
    if (!lock.Succeeded()) {
      continue;
    }

    const JPH::Body &srcBody = lock.GetBody();
    JPH::BodyCreationSettings bcs = srcBody.GetBodyCreationSettings();
    JPH::uint64 userData = srcBody.GetUserData();
    bool wasActive = srcBody.IsActive();

    /* Release the lock before removing. */
    lock.ReleaseLock();

    /* Remove from source. */
    srcBI.RemoveBody(oldID);
    srcBI.DestroyBody(oldID);
    other->m_controllers.erase(ctrl);
    other->m_controllersIterationCacheDirty = true;

    /* Create in destination. */
    JPH::Body *newBody = dstBI.CreateBody(bcs);
    if (!newBody) {
      continue;
    }

    newBody->SetUserData(userData);
    bodyIDMap[oldID.GetIndexAndSequenceNumber()] = newBody->GetID();
    ctrl->SetBodyID(newBody->GetID());
    ctrl->SetEnvironment(this);

    /* Update CollisionGroup to use this environment's constraint group filter
     * and store the new body identity in SubGroupID. */
    JPH::CollisionGroup cg = newBody->GetCollisionGroup();
    cg.SetGroupFilter(m_constraintGroupFilter);
    cg.SetSubGroupID(newBody->GetID().GetIndexAndSequenceNumber());
    newBody->SetCollisionGroup(cg);

    dstBI.AddBody(newBody->GetID(),
                  wasActive ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    this->AddController(ctrl);
  }

  /* Transfer constraints: recreate them in this system using the body ID map.
   * Jolt constraints reference Body pointers, which are invalidated when bodies
   * are moved between systems. We use GetConstraintSettings() to extract the
   * configuration and TwoBodyConstraintSettings::Create() with new bodies. */
  for (auto &pair : other->m_constraintById) {
    JoltConstraint *srcCon = pair.second;
    JPH::Constraint *joltCon = srcCon->GetConstraint();
    if (!joltCon) {
      continue;
    }

    /* Get the two body references from the constraint. */
    JPH::TwoBodyConstraint *tbc = static_cast<JPH::TwoBodyConstraint *>(joltCon);
    JPH::BodyID body1Old = tbc->GetBody1()->GetID();
    JPH::BodyID body2Old = tbc->GetBody2()->GetID();

    auto it1 = bodyIDMap.find(body1Old.GetIndexAndSequenceNumber());
    auto it2 = bodyIDMap.find(body2Old.GetIndexAndSequenceNumber());
    if (it1 == bodyIDMap.end() || it2 == bodyIDMap.end()) {
      continue;
    }

    /* Get constraint settings and recreate with the new bodies. */
    JPH::Ref<JPH::ConstraintSettings> settings = joltCon->GetConstraintSettings();
    if (!settings) {
      continue;
    }

    /* Remove from source system. */
    other->m_physicsSystem->RemoveConstraint(joltCon);

    /* Use the locking BodyInterface to safely access the new bodies.
     * TwoBodyConstraintSettings::Create() takes Body references. */
    JPH::TwoBodyConstraintSettings *tbcSettings =
        static_cast<JPH::TwoBodyConstraintSettings *>(settings.GetPtr());

    const JPH::BodyLockInterface &dstLock = m_physicsSystem->GetBodyLockInterfaceNoLock();
    JPH::BodyLockWrite lock1(dstLock, it1->second);
    JPH::BodyLockWrite lock2(dstLock, it2->second);
    if (!lock1.Succeeded() || !lock2.Succeeded()) {
      continue;
    }

    JPH::Body &newBody1 = lock1.GetBody();
    JPH::Body &newBody2 = lock2.GetBody();

    JPH::Constraint *newCon = tbcSettings->Create(newBody1, newBody2);
    lock1.ReleaseLock();
    lock2.ReleaseLock();

    m_physicsSystem->AddConstraint(newCon);

    /* Register no-collide pair in this environment's group filter. */
    if (srcCon->GetDisableCollision()) {
      m_constraintGroupFilter->DisableCollision(
          it1->second.GetIndexAndSequenceNumber(),
          it2->second.GetIndexAndSequenceNumber());
    }

    JoltConstraint *newJoltCon = new JoltConstraint(
        newCon, srcCon->GetType(), srcCon->GetIdentifier(),
        srcCon->GetDisableCollision(), this);
    newJoltCon->SetBreakingThreshold(srcCon->GetBreakingThreshold());
    newJoltCon->SetEnabled(srcCon->GetEnabled());
    m_constraintById[srcCon->GetIdentifier()] = newJoltCon;

    delete srcCon;
  }
  other->m_constraintById.clear();
  m_breakableConstraintsCacheDirty = true;

  /* Transfer graphic controllers. */
  for (JoltGraphicController *gc : other->m_graphicControllers) {
    gc->SetPhysicsEnvironment(this);
    m_graphicControllers.insert(gc);
  }
  other->m_graphicControllers.clear();

  /* Transfer soft bodies. */
  for (JoltSoftBody *sb : other->m_softBodies) {
    m_softBodies.push_back(sb);
  }
  other->m_softBodies.clear();

  for (JoltSoftBody *sb : other->m_pinnedSoftBodies) {
    m_pinnedSoftBodies.push_back(sb);
  }
  other->m_pinnedSoftBodies.clear();
  other->m_pendingRigidBodyBodyAdds.clear();
  other->m_pendingCompoundSubShapesByController.clear();
  other->m_pendingSoftBodyBodyAdds.clear();
  other->m_pendingSoftBodyBodyRemoves.clear();
  other->m_pendingRigidBodyAddsForOptimize = 0;
  other->m_pendingSoftBodyAddsForOptimize = 0;
  other->m_pendingSoftBodyBatchAddsForOptimize = 0;
  other->m_rigidBodyAddsSinceLastStep = 0;
  other->m_softBodyAddsSinceLastStep = 0;
  other->m_rigidBodyAddIdleFrames = 0;
  other->m_softBodyAddIdleFrames = 0;

  /* Transfer characters. */
  for (auto &pair : other->m_characterByObject) {
    m_characterByObject[pair.first] = pair.second;
  }
  other->m_characterByObject.clear();

  /* Transfer vehicles. */
  for (JoltVehicle *veh : other->m_vehicles) {
    m_vehicles.push_back(veh);
  }
  other->m_vehicles.clear();

  /* Re-optimize broadphase after bulk insertion. */
  m_needsBroadPhaseOptimize = true;
}

void JoltPhysicsEnvironment::ConvertObject(BL_SceneConverter *converter,
                                            KX_GameObject *gameobj,
                                            RAS_MeshObject *meshobj,
                                            KX_Scene *kxscene,
                                            PHY_IMotionState *motionstate,
                                            int activeLayerBitInfo,
                                            bool isCompoundChild,
                                            bool hasCompoundChildren)
{
  using namespace blender;
  blender::Object *blenderobject = gameobj->GetBlenderObject();

  bool isDyna = (blenderobject->gameflag & OB_DYNAMIC) != 0;
  bool isSensor = (blenderobject->gameflag & OB_SENSOR) != 0;
  bool isCharacter = (blenderobject->gameflag & OB_CHARACTER) != 0;
  bool isSoftBody = (blenderobject->gameflag & OB_SOFT_BODY) != 0;
  bool isRigidBody = (blenderobject->gameflag & OB_RIGID_BODY) != 0;
  bool useGimpact = ((isDyna || isSensor) && !isSoftBody);

  /* Determine bounds type. */
  char boundsType;
  if (!(blenderobject->gameflag & OB_BOUNDS)) {
    /* No explicit bounds configured — use type-based defaults.
     * Also sync collision_boundtype and set OB_BOUNDS so the Jolt UI
     * dropdown (which has no checkbox) shows the correct value. */
    if (blenderobject->gameflag & OB_SOFT_BODY)
      boundsType = OB_BOUND_TRIANGLE_MESH;
    else if (blenderobject->gameflag & OB_CHARACTER)
      boundsType = OB_BOUND_SPHERE;
    else if (isDyna)
      boundsType = OB_BOUND_SPHERE;
    else
      boundsType = OB_BOUND_TRIANGLE_MESH;
    blenderobject->collision_boundtype = boundsType;
    blenderobject->gameflag |= OB_BOUNDS;
  }
  else {
    if (ELEM(blenderobject->collision_boundtype, OB_BOUND_CONVEX_HULL, OB_BOUND_TRIANGLE_MESH) &&
        blenderobject->type != OB_MESH) {
      boundsType = OB_BOUND_SPHERE;
    }
    else {
      boundsType = blenderobject->collision_boundtype;
    }
  }

  /* Get bounds information. */
  float bounds_extends[3] = {1.0f, 1.0f, 1.0f};
  if (const std::optional<Bounds<float3>> bl_bounds = BKE_object_boundbox_eval_cached_get(
          blenderobject)) {
    const std::array<float3, 8> corners = bounds::corners(*bl_bounds);
    bounds_extends[0] = 0.5f * fabsf(corners[0][0] - corners[4][0]);
    bounds_extends[1] = 0.5f * fabsf(corners[0][1] - corners[2][1]);
    bounds_extends[2] = 0.5f * fabsf(corners[0][2] - corners[1][2]);
  }

  /* Build the collision shape. */
  JoltShapeBuilder shapeBuilder;
  float margin = isSoftBody ? 0.1f : blenderobject->margin;
  shapeBuilder.SetMargin(margin);

  /* For soft bodies, strip any stale joltSbModifier that survived from a
   * previous game run BEFORE SetMesh() reads vertex positions.  Without
   * this, the depsgraph-cached evaluated mesh still contains the deformation
   * from the previous run and feeds wrong rest-pose coords into Create(). */
  if (isSoftBody) {
    ModifierData *md = (ModifierData *)blenderobject->modifiers.first;
    while (md) {
      ModifierData *next = md->next;
      if (md->type == eModifierType_SimpleDeformBGE) {
        ((blender::SimpleDeformModifierDataBGE *)md)->vertcoos = nullptr;
        BLI_remlink(&blenderobject->modifiers, md);
        BKE_modifier_free(md);
      }
      md = next;
    }
    DEG_id_tag_update(&blenderobject->id, ID_RECALC_GEOMETRY);
  }

  switch (boundsType) {
    case OB_BOUND_SPHERE:
      shapeBuilder.SetShapeType(PHY_SHAPE_SPHERE);
      shapeBuilder.SetRadius(std::max({bounds_extends[0], bounds_extends[1], bounds_extends[2]}));
      break;
    case OB_BOUND_BOX:
      shapeBuilder.SetShapeType(PHY_SHAPE_BOX);
      shapeBuilder.SetHalfExtents(bounds_extends[0], bounds_extends[1], bounds_extends[2]);
      break;
    case OB_BOUND_CYLINDER: {
      float radius = std::max(bounds_extends[0], bounds_extends[1]);
      shapeBuilder.SetShapeType(PHY_SHAPE_CYLINDER);
      shapeBuilder.SetHalfExtents(radius, radius, bounds_extends[2]);
      break;
    }
    case OB_BOUND_CONE:
      shapeBuilder.SetShapeType(PHY_SHAPE_CONE);
      shapeBuilder.SetRadius(std::max(bounds_extends[0], bounds_extends[1]));
      shapeBuilder.SetHeight(2.0f * bounds_extends[2]);
      break;
    case OB_BOUND_CONVEX_HULL:
      shapeBuilder.SetMesh(kxscene, meshobj, true);
      break;
    case OB_BOUND_CAPSULE: {
      float radius = std::max(bounds_extends[0], bounds_extends[1]);
      float height = 2.0f * (bounds_extends[2] - radius);
      if (height < 0.0f) height = 0.0f;
      shapeBuilder.SetShapeType(PHY_SHAPE_CAPSULE);
      shapeBuilder.SetRadius(radius);
      shapeBuilder.SetHeight(height);
      break;
    }
    case OB_BOUND_TRIANGLE_MESH:
      shapeBuilder.SetMesh(kxscene, meshobj, false);
      break;
    case OB_BOUND_EMPTY:
      shapeBuilder.SetShapeType(PHY_SHAPE_EMPTY);
      break;
    default:
      delete motionstate;
      return;
  }

  MT_Vector3 scaling = gameobj->NodeGetWorldScaling();
  MT_Vector3 pos = motionstate->GetWorldPosition();
  MT_Matrix3x3 ori = motionstate->GetWorldOrientation();
  MT_Quaternion quat = ori.getRotation();
  float mass = isDyna ? blenderobject->mass : 0.0f;

  /* --- Soft body path: create JoltSoftBody instead of a rigid body --- */
  if (isSoftBody && !shapeBuilder.GetVertexArray().empty() &&
      !shapeBuilder.GetTriangleArray().empty())
  {
    unsigned short collGroup = blenderobject->col_group;
    unsigned short collMask = blenderobject->col_mask;

    JoltPhysicsController *ctrl = new JoltPhysicsController();
    ctrl->SetMotionState(motionstate);
    ctrl->SetEnvironment(this);
    ctrl->SetDynamic(true);
    ctrl->SetRigidBodyFlag(false);
    ctrl->SetSensorFlag(false);
    ctrl->SetCompoundFlag(false);
    ctrl->SetOriginalMotionType(JPH::EMotionType::Dynamic);
    ctrl->SetBroadPhaseCategory(JOLT_BP_DYNAMIC);
    ctrl->SetMargin(margin);
    ctrl->SetRadius(blenderobject->inertia);
    ctrl->SetCollisionGroup(collGroup);
    ctrl->SetCollisionMask(collMask);
    ctrl->SetNewClientInfo(gameobj->getClientInfo());

    JoltSoftBody *sb = new JoltSoftBody(this, ctrl);

    /* Map all BulletSoftBody settings that have a Jolt equivalent. */
    JoltSoftBodySettings sbSettings;
    sbSettings.mass = mass;
    sbSettings.friction = blenderobject->friction;  /* fallback */
    sbSettings.restitution = blenderobject->reflect;
    sbSettings.damping  = blenderobject->damping;   /* fallback */
    sbSettings.margin   = margin;
    sbSettings.gravityFactor = blenderobject->gravity_factor;

    if (blenderobject->bsoft) {
      const blender::BulletSoftBody *bsoft = blenderobject->bsoft;
      sbSettings.linStiff         = bsoft->linStiff;  /* edge compliance */
      sbSettings.shearStiff       = bsoft->shearStiff; /* shear compliance */
      sbSettings.angStiff         = bsoft->angStiff;  /* bend compliance */
      sbSettings.friction         = bsoft->kDF;        /* dynamic friction */
      sbSettings.damping          = bsoft->kDP;        /* linear damping */
      sbSettings.pressure         = bsoft->kPR;        /* gas pressure */
      sbSettings.margin           = (bsoft->margin > 0.0f) ? bsoft->margin : margin;
      sbSettings.numIterations    = bsoft->piterations;
      sbSettings.bendingConstraints = (bsoft->flag & OB_BSB_BENDING_CONSTRAINTS) != 0;
      sbSettings.lraConstraints   = (bsoft->flag & OB_BSB_LRA_CONSTRAINTS) != 0;
      sbSettings.lraType          = bsoft->lraType;
      sbSettings.facesDoubleSided = (bsoft->flag & OB_BSB_FACES_DOUBLE_SIDED) != 0;
      sbSettings.noPinCollision   = (bsoft->flag & OB_BSB_NO_PIN_COLLISION) != 0;
      sbSettings.pinWeightThreshold = 1.0f - bsoft->pin_weight_threshold;

      /* Build pin vertex weight list from the named vertex group. */
      if (bsoft->pin_vgroup[0] != '\0') {
        blender::bContext *ctx = KX_GetActiveEngine()->GetContext();
        blender::Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(ctx);
        blender::Object *ob_eval = DEG_get_evaluated(depsgraph, meshobj->GetOriginalObject());
        blender::Mesh *me = (blender::Mesh *)ob_eval->data;
        int vgIdx = blender::BKE_object_defgroup_name_index(ob_eval, bsoft->pin_vgroup);
        if (vgIdx >= 0) {
          const blender::Span<blender::MDeformVert> dverts = me->deform_verts();
          if (!dverts.is_empty()) {
            /* vertRemap is built after this block — grab it from shapeBuilder directly. */
            const std::unordered_map<int, int> &vr = shapeBuilder.GetVertexRemap();
            for (const auto &[blendIdx, joltIdx] : vr) {
              if (blendIdx < (int)dverts.size()) {
                float w = blender::BKE_defvert_find_weight(&dverts[blendIdx], vgIdx);
                if (w > 0.0f) {
                  sbSettings.pinVertexWeights.push_back({joltIdx, w});
                }
              }
            }
          }
        }
      }

      /* Store the pin object's initial world transform for per-frame following. */
      if (bsoft->pin_object) {
        blender::Object *pinObj = bsoft->pin_object;
        float loc[3], rot[3][3], scale[3];
        /* Use BKE_object_to_mat4 instead of pinObj->object_to_world() because
         * inactive (template) objects have OB_HIDE_VIEWPORT forced on them by
         * BL_DataConversion, which prevents the depsgraph from evaluating their
         * world transform.  On the second game run pinObj->runtime->object_to_world
         * is (0,0,0) because the depsgraph skipped its evaluation.
         * BKE_object_to_mat4 computes the matrix directly from loc/rot/size and
         * the parent chain without touching the depsgraph cache — the same
         * approach used for rigid-body constraints in BL_DataConversion. */
        float pinMat[4][4];
        BKE_object_to_mat4(pinObj, pinMat);
        mat4_to_loc_rot_size(loc, rot, scale, (const float (*)[4])pinMat);
        sbSettings.hasPinObject  = true;
        sbSettings.pinInitialPos = MT_Vector3(loc[0], loc[1], loc[2]);
        sbSettings.pinInitialOri = MT_Matrix3x3(
            rot[0][0], rot[1][0], rot[2][0],
            rot[0][1], rot[1][1], rot[2][1],
            rot[0][2], rot[1][2], rot[2][2]);
      }
    }

    const std::vector<float> &verts = shapeBuilder.GetVertexArray();
    const std::vector<int> &tris = shapeBuilder.GetTriangleArray();
    const std::unordered_map<int, int> &vertRemap = shapeBuilder.GetVertexRemap();

    bool ok = sb->Create(verts.data(),
                         (int)verts.size() / 3,
                         tris.data(),
                         (int)tris.size() / 3,
                         pos,
                         ori,
                         scaling,
                         sbSettings,
                         vertRemap);
    if (!ok) {
      delete sb;
      delete ctrl;
      delete motionstate;
      return;
    }

    /* Template objects (disabled in viewport) must NOT be simulated.
     * Two hazards arise if the template soft body is left active:
     *
     * 1. SGNode drift: SynchronizeMotionStates() writes the Jolt COM back to
     *    the game object's SGNode each frame.  CloneIntoReplica() reads that
     *    node's world position as the clone's spawn origin (replicaPos).  If
     *    the template has been simulating, replicaPos drifts away from the
     *    rest-pose and all spawned clones appear at the wrong location.
     *
     * 2. Modifier corruption: UpdateMesh() writes Jolt particle positions into
     *    the shared Blender modifier buffer (m_sbCoords).  That buffer is used
     *    by every live replica.  The m_isActive guard in UpdateMesh() (set by
     *    SetActive(false) below) prevents this write for the template.
     *
     * Removing the body from the active world keeps m_bodyID valid (the body
     * still exists in the pool so PostProcessReplica can lock+inspect it), but
     * prevents any physics steps from running on it. */
    bool sbVisCheck = (blenderobject->base_flag &
                       (BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT |
                        BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT)) != 0;
    if (!sbVisCheck) {
      m_physicsSystem->GetBodyInterfaceNoLock().RemoveBody(sb->GetBodyID());
      sb->SetActive(false);
    }

    sb->SetGameObject(gameobj);
    sb->SetMeshObject(meshobj);
    /* Remove any SimpleDeformBGE modifiers left over from a previous game run.
     * Must be called here (once per template, before any clones exist) rather
     * than inside UpdateMesh() where multiple simultaneous clones would
     * accidentally destroy each other's active modifiers. */
    sb->PurgeStaleModifiers();
    if (blenderobject->bsoft && blenderobject->bsoft->pin_object) {
      sb->SetPinBlenderObject(blenderobject->bsoft->pin_object);
    }

    ctrl->SetSoftBody(sb);
    ctrl->SetBodyID(sb->GetBodyID());
    gameobj->SetPhysicsController(ctrl);
    /* Pre-center in Create() places the Jolt body at COM while preserving a
     * stored COM->origin offset in JoltSoftBody. WriteDynamicsToMotionState()
     * subtracts that offset for soft bodies, so the SG node starts at authored
     * object origin from frame 0 with no startup reposition.
     *
     * Client info must be set before this call so parented soft bodies can use
     * game-object world->local conversion during startup sync. */
    ctrl->WriteDynamicsToMotionState();

    m_softBodies.push_back(sb);
    if (sb->HasPinnedVertices()) {
      m_pinnedSoftBodies.push_back(sb);
    }
    AddController(ctrl);
    /* Register controller → blender object mapping for soft-body pin resolution. */
    RegisterControllerForObject(blenderobject, ctrl);
    return;
  }

  /* Soft body was requested but the mesh data is empty (no vertices or no
   * triangles). This can happen if the object is not a mesh or has no
   * collider polygons. Warn so the user understands why physics falls back. */
  if (isSoftBody) {
    printf("Jolt: Object '%s' is set to Soft Body but has no triangle mesh data — "
           "falling back to rigid body simulation.\n",
           blenderobject->id.name + 2);
  }

  JPH::RefConst<JPH::Shape> shape = shapeBuilder.Build(useGimpact, scaling);
  if (!shape) {
    delete motionstate;
    return;
  }

  /* ---- Compound child: merge shape into the parent's compound shape ---- */
  if (isCompoundChild) {
    /* Walk the parent chain to find the compound root (topmost parent with
     * OB_CHILD flag and a solid physics type — same logic as BL_DataConversion). */
    blender::Object *blenderCompoundRoot = nullptr;
    {
      blender::Object *parentit = blenderobject->parent;
      while (parentit) {
        if ((parentit->gameflag & OB_CHILD) &&
            (parentit->gameflag & (OB_COLLISION | OB_DYNAMIC | OB_RIGID_BODY)) &&
            !(parentit->gameflag & OB_SOFT_BODY)) {
          blenderCompoundRoot = parentit;
        }
        parentit = parentit->parent;
      }
    }

    if (!blenderCompoundRoot || !converter) {
      delete motionstate;
      return;
    }

    KX_GameObject *compoundParent = converter->FindGameObject(blenderCompoundRoot);
    if (!compoundParent || !compoundParent->GetPhysicsController()) {
      delete motionstate;
      return;
    }

    JoltPhysicsController *parentCtrl = static_cast<JoltPhysicsController *>(
        compoundParent->GetPhysicsController());
    if (!parentCtrl || !parentCtrl->GetShape()) {
      delete motionstate;
      return;
    }

    /* Compute relative transform (position + rotation) of the child
     * in the parent's local space, matching Bullet's compound child logic. */
    SG_Node *childNode = gameobj->GetSGNode();
    SG_Node *parentNode = compoundParent->GetSGNode();

    MT_Vector3 parentScale = parentNode->GetWorldScaling();
    parentScale[0] = MT_Scalar(1.0f) / parentScale[0];
    parentScale[1] = MT_Scalar(1.0f) / parentScale[1];
    parentScale[2] = MT_Scalar(1.0f) / parentScale[2];

    MT_Matrix3x3 parentInvRot = parentNode->GetWorldOrientation().transposed();
    MT_Vector3 relativePos = parentInvRot *
        ((childNode->GetWorldPosition() - parentNode->GetWorldPosition()) * parentScale);
    MT_Matrix3x3 relativeRot = parentInvRot * childNode->GetWorldOrientation();
    MT_Quaternion relQuat = relativeRot.getRotation();

    if (!m_hasSteppedSimulation) {
      /* Startup conversion path: queue parent shape rebuild for one-pass
       * assembly once conversion-time compound children are known. */
      QueuePendingCompoundChildShape(
          parentCtrl, JoltMath::ToJolt(relativePos), JoltMath::ToJolt(relQuat), shape);

      delete motionstate;
      return;
    }

    /* Runtime path: preserve prior behavior by rebuilding parent compound
     * immediately when a new child is converted. */
    JPH::StaticCompoundShapeSettings compoundSettings;
    JPH::RefConst<JPH::Shape> parentShape = parentCtrl->GetShape();

    const JPH::Shape *rawParentShape = parentShape.GetPtr();
    if (rawParentShape->GetSubType() == JPH::EShapeSubType::StaticCompound) {
      const JPH::StaticCompoundShape *existingCompound =
          static_cast<const JPH::StaticCompoundShape *>(rawParentShape);
      for (JPH::uint i = 0; i < existingCompound->GetNumSubShapes(); i++) {
        const JPH::CompoundShape::SubShape &sub = existingCompound->GetSubShape(i);
        compoundSettings.AddShape(sub.GetPositionCOM(), sub.GetRotation(), sub.mShape);
      }
    }
    else {
      compoundSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), parentShape);
    }

    compoundSettings.AddShape(JoltMath::ToJolt(relativePos), JoltMath::ToJolt(relQuat), shape);

    JPH::Shape::ShapeResult result = compoundSettings.Create();
    if (result.HasError()) {
      delete motionstate;
      return;
    }

    JPH::RefConst<JPH::Shape> newCompound = result.Get();
    parentCtrl->SetShape(newCompound);

    JPH::BodyInterface &bi = GetBodyInterface();
    bi.SetShape(parentCtrl->GetBodyID(), newCompound, true, JPH::EActivation::Activate);

    delete motionstate;
    return;
  }

  /* Determine Jolt motion type and broadphase category. */
  JPH::EMotionType motionType;
  JoltBroadPhaseLayer bpCategory;
  if (isSensor) {
    motionType = JPH::EMotionType::Static;
    bpCategory = JOLT_BP_SENSOR;
  }
  else if (isCharacter || isDyna) {
    motionType = JPH::EMotionType::Dynamic;
    bpCategory = JOLT_BP_DYNAMIC;
  }
  else {
    motionType = JPH::EMotionType::Static;
    bpCategory = JOLT_BP_STATIC;
  }

  unsigned short col_group = blenderobject->col_group;
  unsigned short col_mask = blenderobject->col_mask;
  JPH::ObjectLayer objectLayer = JoltMakeObjectLayer(col_group, col_mask, bpCategory);

  JPH::BodyCreationSettings bodySettings(
      shape,
      JoltMath::ToJolt(pos),
      JoltMath::ToJolt(quat),
      motionType,
      objectLayer);

  /* Set mass for dynamic bodies. */
  if (mass > 0.0f) {
    bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = mass;
  }

  /* Friction, restitution, damping. */
  bodySettings.mFriction = blenderobject->friction;
  bodySettings.mRestitution = blenderobject->reflect;
  bodySettings.mLinearDamping = blenderobject->damping;
  bodySettings.mAngularDamping = blenderobject->rdamping;

  /* Per-body gravity multiplier. */
  bodySettings.mGravityFactor = blenderobject->gravity_factor;

  /* Inertia multiplier from form factor (Bullet used formfactor / 0.4). */
  if (mass > 0.0f) {
    bodySettings.mInertiaMultiplier = blenderobject->formfactor / 0.4f;
  }

  /* Per-body sleep control: OB_COLLISION_RESPONSE means "No Sleeping" in UI. */
  if (blenderobject->gameflag & OB_COLLISION_RESPONSE) {
    bodySettings.mAllowSleeping = false;
  }

  /* Sensor flag. */
  if (isSensor || (blenderobject->gameflag & OB_GHOST)) {
    bodySettings.mIsSensor = true;
  }

  /* Allow switching dynamic↔static for SuspendDynamics/RestoreDynamics. */
  if (isDyna || isCharacter) {
    bodySettings.mAllowDynamicOrKinematic = true;
  }

  /* CCD for fast-moving dynamic bodies. */
  if ((blenderobject->gameflag2 & OB_CCD_RIGID_BODY) && isDyna) {
    bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
  }

  /* Gyroscopic force (Dzhanibekov / tennis racket effect). */
  if (blenderobject->gameflag2 & OB_GYROSCOPIC_FORCE) {
    bodySettings.mApplyGyroscopicForce = true;
  }

  /* Enhanced internal edge removal for mesh shapes. */
  if (boundsType == OB_BOUND_TRIANGLE_MESH) {
    bodySettings.mEnhancedInternalEdgeRemoval = true;
  }

  /* Axis locking via Jolt AllowedDOFs (matching Bullet linear/angular factor).
   * Coordinate mapping: Blender (X,Y,Z) → Jolt (X,Z,-Y), so:
   *   Blender X → Jolt X
   *   Blender Y → Jolt Z
   *   Blender Z → Jolt Y  */
  if (isDyna) {
    JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_AXIS)
      dofs &= ~JPH::EAllowedDOFs::TranslationX;
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_AXIS)
      dofs &= ~JPH::EAllowedDOFs::TranslationZ;  /* Blender Y → Jolt Z */
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_AXIS)
      dofs &= ~JPH::EAllowedDOFs::TranslationY;  /* Blender Z → Jolt Y */
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_ROT_AXIS)
      dofs &= ~JPH::EAllowedDOFs::RotationX;
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_ROT_AXIS)
      dofs &= ~JPH::EAllowedDOFs::RotationZ;     /* Blender Y → Jolt Z */
    if (blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_ROT_AXIS)
      dofs &= ~JPH::EAllowedDOFs::RotationY;     /* Blender Z → Jolt Y */
    /* Non-rigid dynamic bodies lock all rotation. */
    if (!isRigidBody) {
      dofs &= ~(JPH::EAllowedDOFs::RotationX | JPH::EAllowedDOFs::RotationY |
                JPH::EAllowedDOFs::RotationZ);
    }
    bodySettings.mAllowedDOFs = dofs;
  }

  /* Create the body. */
  JPH::BodyInterface &bi = GetBodyInterface();
  JPH::Body *body = bi.CreateBody(bodySettings);
  if (!body) {
    printf("Jolt: ConvertObject FAILED to create body for '%s'\n",
           gameobj->GetName().c_str());
    delete motionstate;
    return;
  }

  /* Create the controller. */
  JoltPhysicsController *ctrl = new JoltPhysicsController();
  ctrl->SetBodyID(body->GetID());
  ctrl->SetMotionState(motionstate);
  ctrl->SetEnvironment(this);
  ctrl->SetDynamic(isDyna);
  ctrl->SetRigidBodyFlag(isDyna && isRigidBody);
  ctrl->SetSensorFlag(isSensor);
  ctrl->SetCompoundFlag(hasCompoundChildren);
  ctrl->SetOriginalMotionType(motionType);
  ctrl->SetBroadPhaseCategory(bpCategory);
  ctrl->SetShape(shape);
  ctrl->SetMargin(margin);
  ctrl->SetRadius(blenderobject->inertia);
  ctrl->SetFhEnabled((blenderobject->gameflag & OB_DO_FH) != 0);
  ctrl->SetFhRotEnabled((blenderobject->gameflag & OB_ROT_FH) != 0);
  ctrl->SetFhSpring(blenderobject->fh);
  ctrl->SetFhDamping(blenderobject->xyfrict);
  ctrl->SetFhDistance(blenderobject->fhdist);
  ctrl->SetFhNormal((blenderobject->dynamode & OB_FH_NOR) != 0);
  ctrl->SetLinVelocityMin(blenderobject->min_vel);
  ctrl->SetLinVelocityMax(blenderobject->max_vel);
  ctrl->SetAngularVelocityMin(blenderobject->min_angvel);
  ctrl->SetAngularVelocityMax(blenderobject->max_angvel);

  /* Set the shared constraint group filter and store the body's unique ID
   * in SubGroupID for constraint "Disable Collisions" filtering.
   * Primary group/mask filtering is done by JoltObjectLayerPairFilter
   * via the ObjectLayer set above. */
  body->SetCollisionGroup(JPH::CollisionGroup(m_constraintGroupFilter,
                                               (JPH::CollisionGroup::GroupID)col_group,
                                               body->GetID().GetIndexAndSequenceNumber()));

  /* Store user data on body for fast lookup in ContactListener. */
  body->SetUserData(reinterpret_cast<JPH::uint64>(gameobj->getClientInfo()));

  gameobj->SetPhysicsController(ctrl);
  ctrl->SetNewClientInfo(gameobj->getClientInfo());
  ctrl->SetCollisionGroup(col_group);
  ctrl->SetCollisionMask(col_mask);

  /* --- Character controller: create JoltCharacter alongside the rigid body --- */
  if (isCharacter) {
    float capsuleRadius = std::max(bounds_extends[0], bounds_extends[1]);
    float capsuleHalfHeight = bounds_extends[2] - capsuleRadius;
    if (capsuleHalfHeight < 0.01f) {
      capsuleHalfHeight = 0.01f;
    }
    float stepHeight = blenderobject->step_height;
    JoltCharacter *character = new JoltCharacter(
        ctrl, this, capsuleRadius, capsuleHalfHeight, stepHeight, pos);
    character->SetJumpSpeed(blenderobject->jump_speed);
    character->SetFallSpeed(blenderobject->fall_speed);
    character->SetMaxSlope(blenderobject->max_slope);
    character->SetMaxJumps(blenderobject->max_jumps);
    m_characterByObject[gameobj] = character;
  }

  /* Don't add sensor objects automatically — they are added when a collision sensor registers. */
  bool layCheck = (blenderobject->lay & activeLayerBitInfo) != 0;
  bool visCheck = (blenderobject->base_flag &
                            (BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT |
                             BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT)) != 0;
  if (!isSensor && layCheck && visCheck)
  {
    if (!m_hasSteppedSimulation) {
      QueueRigidBodyBodyAdd(body->GetID(), JPH::EActivation::Activate);
    }
    else {
      bi.AddBody(body->GetID(), JPH::EActivation::Activate);
      NotifyRigidBodyBodyAdded();
    }
    AddController(ctrl);
  }
  else if (!isSensor) {
    /* Body created but not added yet. */
    AddController(ctrl);
  }
  else {
    /* Sensor: controller tracked but body not added until AddSensor(). */
    AddController(ctrl);
  }

  /* Register blender object → controller mapping so soft-body pin_object lookup works. */
  RegisterControllerForObject(blenderobject, ctrl);

  /* Suspend dynamics for parented objects. */
  blender::Object *blenderRoot = blenderobject->parent;
  while (blenderRoot && blenderRoot->parent) {
    blenderRoot = blenderRoot->parent;
  }
  if (blenderRoot && converter) {
    converter->AddPendingSuspendDynamics(ctrl);
  }
}

void JoltPhysicsEnvironment::SetupObjectConstraints(
    KX_GameObject *obj_src,
    KX_GameObject *obj_dest,
    blender::bRigidBodyJointConstraint *dat,
    bool replicate_dupli)
{
  PHY_IPhysicsController *phy_src = obj_src->GetPhysicsController();
  PHY_IPhysicsController *phy_dest = obj_dest->GetPhysicsController();

  /* Build constraint frame from stored axes. */
  MT_Matrix3x3 localCFrame(MT_Vector3(dat->axX, dat->axY, dat->axZ));
  MT_Vector3 axis0 = localCFrame.getColumn(0);
  MT_Vector3 axis1 = localCFrame.getColumn(1);
  MT_Vector3 axis2 = localCFrame.getColumn(2);
  MT_Vector3 scale = obj_src->NodeGetWorldScaling();

  PHY_IConstraint *constraint = CreateConstraint(phy_src,
                                                  phy_dest,
                                                  (PHY_ConstraintType)dat->type,
                                                  (float)(dat->pivX * scale.x()),
                                                  (float)(dat->pivY * scale.y()),
                                                  (float)(dat->pivZ * scale.z()),
                                                  (float)(axis0.x() * scale.x()),
                                                  (float)(axis0.y() * scale.y()),
                                                  (float)(axis0.z() * scale.z()),
                                                  (float)(axis1.x() * scale.x()),
                                                  (float)(axis1.y() * scale.y()),
                                                  (float)(axis1.z() * scale.z()),
                                                  (float)(axis2.x() * scale.x()),
                                                  (float)(axis2.y() * scale.y()),
                                                  (float)(axis2.z() * scale.z()),
                                                  dat->flag,
                                                  replicate_dupli);

  if (!constraint) {
    return;
  }

  /* Set per-DOF limits based on constraint type. */
  int dof = 0;
  int dof_max = 0;
  int dofbit = 0;

  switch (dat->type) {
    case PHY_GENERIC_6DOF_CONSTRAINT:
      dof_max = 6;
      dofbit = 1;
      break;
    case PHY_CONE_TWIST_CONSTRAINT:
      dof = 3;
      dof_max = 6;
      dofbit = 1 << 3;
      break;
    case PHY_LINEHINGE_CONSTRAINT:
    case PHY_ANGULAR_CONSTRAINT:
      dof = 3;
      dof_max = 4;
      dofbit = 1 << 3;
      break;
    default:
      break;
  }

  for (; dof < dof_max; dof++) {
    if (dat->flag & dofbit) {
      constraint->SetParam(dof, dat->minLimit[dof], dat->maxLimit[dof]);
    }
    else {
      constraint->SetParam(dof, 1.0f, -1.0f);
    }
    dofbit <<= 1;
  }

  if (dat->flag & CONSTRAINT_USE_BREAKING) {
    constraint->SetBreakingThreshold(dat->breaking);
  }
}

int JoltPhysicsEnvironment::CreateRigidBodyConstraint(KX_GameObject *gameobj1,
                                                       KX_GameObject *gameobj2,
                                                       const MT_Vector3 &pivotLocal,
                                                       const MT_Matrix3x3 &basisLocal,
                                                       blender::RigidBodyCon *rbc)
{
  using namespace blender;

  if (!gameobj1 || !rbc) {
    return -1;
  }

  if (!gameobj1->GetSGNode()) {
    return -1;
  }

  PHY_IPhysicsController *ctrl1 = gameobj1->GetPhysicsController();
  PHY_IPhysicsController *ctrl2 = gameobj2 ? gameobj2->GetPhysicsController() : nullptr;

  if (!ctrl1) {
    return -1;
  }

  MT_Vector3 axis0 = basisLocal.getColumn(0).safe_normalized();
  MT_Vector3 axis1 = basisLocal.getColumn(1).safe_normalized();
  MT_Vector3 axis2 = basisLocal.getColumn(2).safe_normalized();

  /* Build an orthonormal frame for Jolt from the incoming local basis.
   * Jolt is stricter than Bullet about valid constraint frames and may reject or
   * behave badly with scaled/sheared axes from authoring transforms. */
  if (axis0.length2() < 1.0e-8f) {
    axis0 = MT_Vector3(1.0f, 0.0f, 0.0f);
  }

  axis1 = (axis1 - axis0.dot(axis1) * axis0).safe_normalized();
  if (axis1.length2() < 1.0e-8f) {
    axis1 = (axis2 - axis0.dot(axis2) * axis0).safe_normalized();
  }
  if (axis1.length2() < 1.0e-8f) {
    const MT_Vector3 fallback = (std::abs(axis0.z()) < 0.999f) ? MT_Vector3(0.0f, 0.0f, 1.0f) :
                                                              MT_Vector3(0.0f, 1.0f, 0.0f);
    axis1 = (fallback - axis0.dot(fallback) * axis0).safe_normalized();
  }

  axis2 = axis0.cross(axis1).safe_normalized();

  PHY_ConstraintType type = PHY_GENERIC_6DOF_CONSTRAINT;
  bool use_springs = false;
  bool is_fixed = false;
  bool is_slider = false;
  bool is_piston = false;
  bool is_motor = false;

  switch (rbc->type) {
    case RBC_TYPE_POINT:
      type = PHY_POINT2POINT_CONSTRAINT;
      break;
    case RBC_TYPE_HINGE:
      type = PHY_LINEHINGE_CONSTRAINT;
      std::swap(axis0, axis2);
      std::swap(axis1, axis2);
      break;
    case RBC_TYPE_SLIDER:
      type = PHY_GENERIC_6DOF_CONSTRAINT;
      is_slider = true;
      break;
    case RBC_TYPE_6DOF:
      type = PHY_GENERIC_6DOF_CONSTRAINT;
      break;
    case RBC_TYPE_6DOF_SPRING:
      type = (rbc->spring_type == RBC_SPRING_TYPE2) ? PHY_GENERIC_6DOF_SPRING2_CONSTRAINT :
                                                      PHY_GENERIC_6DOF_CONSTRAINT;
      use_springs = true;
      break;
    case RBC_TYPE_FIXED:
      type = PHY_GENERIC_6DOF_CONSTRAINT;
      is_fixed = true;
      break;
    case RBC_TYPE_PISTON:
      type = PHY_GENERIC_6DOF_CONSTRAINT;
      is_piston = true;
      break;
    case RBC_TYPE_MOTOR:
      type = PHY_GENERIC_6DOF_CONSTRAINT;
      is_motor = true;
      break;
    default:
      break;
  }

  int flag = 0;
  if (rbc->flag & RBC_FLAG_DISABLE_COLLISIONS) {
    flag |= CCD_CONSTRAINT_DISABLE_LINKED_COLLISION;
  }

  PHY_IConstraint *constraint = CreateConstraint(ctrl1,
                                                  ctrl2,
                                                  type,
                                                  pivotLocal.x(),
                                                  pivotLocal.y(),
                                                  pivotLocal.z(),
                                                  axis0.x(),
                                                  axis0.y(),
                                                  axis0.z(),
                                                  axis1.x(),
                                                  axis1.y(),
                                                  axis1.z(),
                                                  axis2.x(),
                                                  axis2.y(),
                                                  axis2.z(),
                                                  flag);

  if (!constraint) {
    return -1;
  }

  if (rbc->flag & RBC_FLAG_USE_BREAKING) {
    constraint->SetBreakingThreshold(rbc->breaking_threshold);
  }

  if (rbc->flag & RBC_FLAG_OVERRIDE_SOLVER_ITERATIONS) {
    constraint->SetSolverIterations(rbc->num_solver_iterations);
  }

  auto set_limit = [&](int axis, bool use_flag, float lower, float upper) {
    constraint->SetParam(axis, use_flag ? lower : 1.0f, use_flag ? upper : -1.0f);
  };

  if (type == PHY_GENERIC_6DOF_CONSTRAINT || type == PHY_GENERIC_6DOF_SPRING2_CONSTRAINT) {
    if (is_fixed) {
      for (int i = 0; i < 6; ++i) {
        constraint->SetParam(i, 0.0f, 0.0f);
      }
    }
    else if (is_slider) {
      set_limit(0, (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X), rbc->limit_lin_x_lower, rbc->limit_lin_x_upper);
      constraint->SetParam(1, 0.0f, 0.0f);
      constraint->SetParam(2, 0.0f, 0.0f);
      constraint->SetParam(3, 0.0f, 0.0f);
      constraint->SetParam(4, 0.0f, 0.0f);
      constraint->SetParam(5, 0.0f, 0.0f);
    }
    else if (is_piston) {
      set_limit(0, (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X), rbc->limit_lin_x_lower, rbc->limit_lin_x_upper);
      constraint->SetParam(1, 0.0f, 0.0f);
      constraint->SetParam(2, 0.0f, 0.0f);
      set_limit(3, (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_X), rbc->limit_ang_x_lower, rbc->limit_ang_x_upper);
      constraint->SetParam(4, 0.0f, 0.0f);
      constraint->SetParam(5, 0.0f, 0.0f);
    }
    else if (is_motor) {
      for (int i = 0; i < 6; ++i) {
        constraint->SetParam(i, 1.0f, -1.0f);
      }
      if (rbc->flag & RBC_FLAG_USE_MOTOR_LIN) {
        constraint->SetParam(6, rbc->motor_lin_target_velocity, rbc->motor_lin_max_impulse);
      }
      if (rbc->flag & RBC_FLAG_USE_MOTOR_ANG) {
        constraint->SetParam(9, rbc->motor_ang_target_velocity, rbc->motor_ang_max_impulse);
      }
    }
    else {
      set_limit(0, (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_X), rbc->limit_lin_x_lower, rbc->limit_lin_x_upper);
      set_limit(1, (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_Y), rbc->limit_lin_y_lower, rbc->limit_lin_y_upper);
      set_limit(2, (rbc->flag & RBC_FLAG_USE_LIMIT_LIN_Z), rbc->limit_lin_z_lower, rbc->limit_lin_z_upper);
      set_limit(3, (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_X), rbc->limit_ang_x_lower, rbc->limit_ang_x_upper);
      set_limit(4, (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Y), rbc->limit_ang_y_lower, rbc->limit_ang_y_upper);
      set_limit(5, (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Z), rbc->limit_ang_z_lower, rbc->limit_ang_z_upper);

      if (use_springs) {
        if (rbc->flag & RBC_FLAG_USE_SPRING_X) {
          constraint->SetParam(12, rbc->spring_stiffness_x, rbc->spring_damping_x);
        }
        if (rbc->flag & RBC_FLAG_USE_SPRING_Y) {
          constraint->SetParam(13, rbc->spring_stiffness_y, rbc->spring_damping_y);
        }
        if (rbc->flag & RBC_FLAG_USE_SPRING_Z) {
          constraint->SetParam(14, rbc->spring_stiffness_z, rbc->spring_damping_z);
        }
        if (rbc->flag & RBC_FLAG_USE_SPRING_ANG_X) {
          constraint->SetParam(15, rbc->spring_stiffness_ang_x, rbc->spring_damping_ang_x);
        }
        if (rbc->flag & RBC_FLAG_USE_SPRING_ANG_Y) {
          constraint->SetParam(16, rbc->spring_stiffness_ang_y, rbc->spring_damping_ang_y);
        }
        if (rbc->flag & RBC_FLAG_USE_SPRING_ANG_Z) {
          constraint->SetParam(17, rbc->spring_stiffness_ang_z, rbc->spring_damping_ang_z);
        }
      }
    }
  }
  else if (type == PHY_LINEHINGE_CONSTRAINT && (rbc->flag & RBC_FLAG_USE_LIMIT_ANG_Z)) {
    constraint->SetParam(3, rbc->limit_ang_z_lower, rbc->limit_ang_z_upper);
  }

  return constraint->GetIdentifier();
}

void JoltPhysicsEnvironment::SetRigidBodyConstraintEnabled(int constraintid, bool enabled)
{
  auto it = m_constraintById.find(constraintid);
  if (it == m_constraintById.end()) {
    return;
  }
  it->second->SetEnabled(enabled);
}

void JoltPhysicsEnvironment::ExportFile(const std::string &filename)
{
  /* Optional: serialize Jolt world state. */
}

bool JoltPhysicsEnvironment::SavePhysicsState(std::vector<uint8_t> &outBuffer)
{
  JPH::StateRecorderImpl recorder;
  m_physicsSystem->SaveState(recorder);

  std::string data = recorder.GetData();
  outBuffer.assign(data.begin(), data.end());
  return !outBuffer.empty();
}

bool JoltPhysicsEnvironment::RestorePhysicsState(const std::vector<uint8_t> &inBuffer)
{
  if (inBuffer.empty()) {
    return false;
  }

  JPH::StateRecorderImpl recorder;
  /* Write the saved data into the recorder's stream, then rewind for reading. */
  recorder.WriteBytes(inBuffer.data(), inBuffer.size());
  recorder.Rewind();

  if (!m_physicsSystem->RestoreState(recorder)) {
    return false;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltPhysicsEnvironment — Jolt-specific methods
 * \{ */

JPH::BodyInterface &JoltPhysicsEnvironment::GetBodyInterface()
{
  return m_physicsSystem->GetBodyInterface();
}

JPH::BodyInterface &JoltPhysicsEnvironment::GetBodyInterfaceNoLock()
{
  return m_physicsSystem->GetBodyInterfaceNoLock();
}

void JoltPhysicsEnvironment::AddController(JoltPhysicsController *ctrl)
{
  if (!ctrl) {
    return;
  }

  const std::pair<std::set<JoltPhysicsController *>::iterator, bool> insertResult =
      m_controllers.insert(ctrl);
  if (insertResult.second) {
    m_controllersIterationCacheDirty = true;
  }
}

bool JoltPhysicsEnvironment::RemoveController(JoltPhysicsController *ctrl)
{
  if (!ctrl) {
    return false;
  }

  const bool removed = m_controllers.erase(ctrl) > 0;
  if (!removed) {
    return false;
  }

  m_controllersIterationCacheDirty = true;
  m_pendingCompoundSubShapesByController.erase(ctrl);

  const JPH::BodyID bodyID = ctrl->GetBodyID();
  if (bodyID.IsInvalid()) {
    return true;
  }
  RemovePendingRigidBodyBodyAdd(bodyID);

  return true;
}

const std::vector<JoltPhysicsController *> &JoltPhysicsEnvironment::GetControllersForIteration()
{
  if (!m_controllersIterationCacheDirty) {
    return m_controllersIterationCache;
  }

  m_controllersIterationCache.clear();
  m_controllersIterationCache.reserve(m_controllers.size());
  for (JoltPhysicsController *ctrl : m_controllers) {
    m_controllersIterationCache.push_back(ctrl);
  }
  m_controllersIterationCacheDirty = false;

  return m_controllersIterationCache;
}

void JoltPhysicsEnvironment::AddGraphicController(JoltGraphicController *ctrl)
{
  m_graphicControllers.insert(ctrl);
}

void JoltPhysicsEnvironment::RemoveGraphicController(JoltGraphicController *ctrl)
{
  m_graphicControllers.erase(ctrl);
}

void JoltPhysicsEnvironment::CallbackTriggers()
{
  if (!m_triggerCallbacks[PHY_OBJECT_RESPONSE]) {
    return;
  }

  if (!m_collectContactsForCallbacks.load(std::memory_order_relaxed)) {
    /* If callback requests were removed during/after Update(), flush leftovers. */
    m_contactPairsScratch.clear();
    m_contactListener.SwapContacts(m_contactPairsScratch);
    m_contactPairsScratch.clear();
    return;
  }

  /* Swap contact pairs from the listener (accumulated during Update()). */
  m_contactPairsScratch.clear();
  m_contactListener.SwapContacts(m_contactPairsScratch);

  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();

  for (const JoltContactPair &pair : m_contactPairsScratch) {
    /* Lock both bodies atomically to read their user data.
     * BodyLockMultiRead computes a combined mutex mask so each unique
     * internal Jolt mutex is locked exactly once, avoiding the undefined
     * behaviour (and potential std::system_error / deadlock) that occurs
     * when two separate BodyLockRead calls happen to map to the same
     * mutex slot in Jolt's MutexArray. */
    JPH::BodyID ids[2] = { pair.bodyID1, pair.bodyID2 };
    JPH::BodyLockMultiRead multiLock(lockInterface, ids, 2);
    const JPH::Body *body1 = multiLock.GetBody(0);
    const JPH::Body *body2 = multiLock.GetBody(1);
    if (!body1 || !body2) {
      continue;
    }

    KX_ClientObjectInfo *info1 = reinterpret_cast<KX_ClientObjectInfo *>(body1->GetUserData());
    KX_ClientObjectInfo *info2 = reinterpret_cast<KX_ClientObjectInfo *>(body2->GetUserData());
    if (!info1 || !info2 || !info1->m_gameobject || !info2->m_gameobject) {
      continue;
    }

    PHY_IPhysicsController *ctrl1 = info1->m_gameobject->GetPhysicsController();
    PHY_IPhysicsController *ctrl2 = info2->m_gameobject->GetPhysicsController();
    if (!ctrl1 || !ctrl2) {
      continue;
    }

    JoltPhysicsController *jctrl1 = static_cast<JoltPhysicsController *>(ctrl1);
    JoltPhysicsController *jctrl2 = static_cast<JoltPhysicsController *>(ctrl2);

    /* Check if at least one controller is registered for collision callbacks. */
    bool first;
    if (m_collisionCallbackControllers.count(jctrl1)) {
      first = true;
    }
    else if (m_collisionCallbackControllers.count(jctrl2)) {
      first = false;
    }
    else {
      continue;
    }

    /* Build collision data from the contact pair. */
    JoltCollData *collData = new JoltCollData();
    MT_Vector3 worldPoint = JoltMath::ToMT(JPH::Vec3(pair.contactPosition));
    MT_Vector3 normal = JoltMath::ToMT(pair.contactNormal);
    /* localA and localB are approximated as the world point for now.
     * Full local-space computation requires body transforms. */
    collData->AddContactPoint(worldPoint,
                              worldPoint,
                              worldPoint,
                              normal,
                              pair.combinedFriction,
                              pair.combinedRestitution);

    /* Invoke the collision callback. */
    m_triggerCallbacks[PHY_OBJECT_RESPONSE](
        m_triggerCallbacksUserPtrs[PHY_OBJECT_RESPONSE], ctrl1, ctrl2, collData, first);
  }
}

void JoltPhysicsEnvironment::ProcessFhSprings(float timeStep)
{
  /* FH (Floating Height) springs: cast a ray downward from each dynamic body
   * that has FH enabled, and apply a spring force to maintain hover height.
   *
   * This replicates CcdPhysicsEnvironment::ProcessFhSprings() using Jolt ray casts.
   * Per-object parameters: fhSpring (force), fhDamping, fhDistance, fhNormal, fhRot. */
  if (timeStep <= 0.0f) {
    return;
  }

  JPH::BodyInterface &bi = GetBodyInterface();
  const JPH::NarrowPhaseQuery &npq = m_physicsSystem->GetNarrowPhaseQuery();
  const std::vector<JoltPhysicsController *> &controllers = GetControllersForIteration();

  for (JoltPhysicsController *ctrl : controllers) {
    if (!ctrl->IsDynamic() || ctrl->IsPhysicsSuspended()) {
      continue;
    }

    if (!ctrl->GetFhEnabled()) {
      continue;
    }

    float fhDist = ctrl->GetFhDistance();
    float fhSpring = ctrl->GetFhSpring();
    if (fhDist <= 0.0f || fhSpring <= 0.0f) {
      continue;
    }

    JPH::BodyID bodyID = ctrl->GetBodyID();
    if (bodyID.IsInvalid()) {
      continue;
    }

    /* Cast ray downward in Jolt Y-up space (Blender -Z). */
    JPH::RVec3 bodyPos = bi.GetCenterOfMassPosition(bodyID);
    JPH::Vec3 rayDir(0.0f, -1.0f, 0.0f);

    JPH::RRayCast ray(bodyPos, rayDir * fhDist);
    JPH::RayCastResult hit;

    if (!npq.CastRay(ray, hit)) {
      continue;
    }

    if (hit.mBodyID == bodyID) {
      continue;
    }

    float hitFraction = hit.mFraction;
    float hitDist = hitFraction * fhDist;

    if (hitDist >= fhDist) {
      continue;
    }

    /* Spring force proportional to how far inside the FH distance we are.
     * Matches Bullet: F = fhSpring * (1 - hitDist/fhDist) - fhDamping * vel_along_normal. */
    float penetrationRatio = 1.0f - (hitDist / fhDist);

    /* Get body mass via body lock. */
    float bodyMass = 1.0f;
    {
      JPH::BodyLockRead bodyLock(m_physicsSystem->GetBodyLockInterface(), bodyID);
      if (bodyLock.Succeeded()) {
        const JPH::Body &body = bodyLock.GetBody();
        if (body.GetMotionProperties()) {
          float invMass = body.GetMotionProperties()->GetInverseMass();
          if (invMass > 0.0f) {
            bodyMass = 1.0f / invMass;
          }
        }
      }
    }

    /* Get surface normal at hit point (default to up if unavailable). */
    JPH::Vec3 hitNormal(0.0f, 1.0f, 0.0f);
    if (ctrl->GetFhNormal()) {
      JPH::BodyLockRead hitLock(m_physicsSystem->GetBodyLockInterface(), hit.mBodyID);
      if (hitLock.Succeeded()) {
        const JPH::Body &hitBody = hitLock.GetBody();
        JPH::RVec3 hitPoint = ray.GetPointOnRay(hitFraction);
        JPH::Vec3 localHitPoint = JPH::Vec3(hitPoint - hitBody.GetCenterOfMassPosition());
        hitNormal = hitBody.GetShape()->GetSurfaceNormal(
            hit.mSubShapeID2, localHitPoint);
      }
    }

    JPH::Vec3 linVel = bi.GetLinearVelocity(bodyID);
    float velAlongNormal = linVel.Dot(hitNormal);
    float fhDamping = ctrl->GetFhDamping();

    float forceMag = fhSpring * penetrationRatio - fhDamping * velAlongNormal;
    if (forceMag < 0.0f) {
      forceMag = 0.0f;
    }

    /* Apply force along the surface normal (or straight up). */
    JPH::Vec3 forceVec = hitNormal * (forceMag * bodyMass);
    bi.AddForce(bodyID, forceVec);

    /* Rotate-from-normal: apply torque to align body up-axis with surface normal. */
    if (ctrl->GetFhRotEnabled()) {
      JPH::Vec3 bodyUp = bi.GetRotation(bodyID) * JPH::Vec3(0.0f, 1.0f, 0.0f);
      JPH::Vec3 torqueAxis = bodyUp.Cross(hitNormal);
      float sinAngle = torqueAxis.Length();
      if (sinAngle > 1e-4f) {
        torqueAxis = torqueAxis / sinAngle;
        float torqueMag = sinAngle * fhSpring * 0.5f * bodyMass;
        bi.AddTorque(bodyID, torqueAxis * torqueMag);
      }
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Deferred Physics Operations
 * \{ */

bool JoltPhysicsEnvironment::QueueDeferredOperation(const JoltDeferredOp &op)
{
  if (!m_isPhysicsUpdating) {
    /* Physics not updating - caller should execute immediately. */
    return false;
  }

  m_deferredOps.push_back(op);
  return true;
}

void JoltPhysicsEnvironment::ProcessDeferredOperations()
{
  /* Swap to local copy to allow new operations to be queued during processing. */
  std::vector<JoltDeferredOp> ops;
  ops.swap(m_deferredOps);

  if (ops.empty()) {
    return;
  }

  JPH::BodyInterface &bi = GetBodyInterface();

  auto removePendingRigidAdds = [&](const std::vector<JPH::BodyID> &bodyIDs) {
    if (bodyIDs.empty() || m_pendingRigidBodyBodyAdds.empty()) {
      return;
    }

    std::unordered_set<JPH::uint32> bodyIDSet;
    bodyIDSet.reserve(bodyIDs.size());
    for (const JPH::BodyID bodyID : bodyIDs) {
      bodyIDSet.insert(bodyID.GetIndexAndSequenceNumber());
    }

    auto newEnd = std::remove_if(
        m_pendingRigidBodyBodyAdds.begin(),
        m_pendingRigidBodyBodyAdds.end(),
        [&](const PendingBodyAddEntry &entry) {
          return bodyIDSet.find(entry.bodyID.GetIndexAndSequenceNumber()) != bodyIDSet.end();
        });
    if (newEnd != m_pendingRigidBodyBodyAdds.end()) {
      m_pendingRigidBodyBodyAdds.erase(newEnd, m_pendingRigidBodyBodyAdds.end());
    }
  };

  auto removeBodiesBatch = [&](const std::vector<JPH::BodyID> &bodyIDs) {
    if (!bodyIDs.empty()) {
      std::vector<JPH::BodyID> mutableBodyIDs = bodyIDs;
      bi.RemoveBodies(mutableBodyIDs.data(), (int)mutableBodyIDs.size());
    }
  };

  auto addBodiesBatch = [&](const std::vector<JPH::BodyID> &bodyIDs,
                            JPH::EActivation activation) {
    if (bodyIDs.empty()) {
      return;
    }

    std::vector<JPH::BodyID> addBodyIDs = bodyIDs;
    JPH::BodyID *rawBodyIDs = addBodyIDs.data();
    const int bodyCount = (int)addBodyIDs.size();
    JPH::BodyInterface::AddState addState = bi.AddBodiesPrepare(rawBodyIDs, bodyCount);
    if (addState) {
      bi.AddBodiesFinalize(rawBodyIDs, bodyCount, addState, activation);
    }
    else {
      for (int bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        bi.AddBody(rawBodyIDs[bodyIndex], activation);
      }

      /* Fallback used one-by-one insertion. Track this as runtime add pressure
       * for the regular threshold/idle broadphase optimize policy. */
      m_pendingRigidBodyAddsForOptimize += bodyCount;
      m_rigidBodyAddsSinceLastStep += bodyCount;
    }
  };

  auto destroyBodiesBatch = [&](const std::vector<JPH::BodyID> &bodyIDs) {
    if (bodyIDs.empty()) {
      return;
    }

    std::vector<JPH::BodyID> addedBodyIDs;
    addedBodyIDs.reserve(bodyIDs.size());
    for (const JPH::BodyID bodyID : bodyIDs) {
      if (bi.IsAdded(bodyID)) {
        addedBodyIDs.push_back(bodyID);
      }
    }

    if (!addedBodyIDs.empty()) {
      bi.RemoveBodies(addedBodyIDs.data(), (int)addedBodyIDs.size());
    }
    bi.DestroyBodies(bodyIDs.data(), (int)bodyIDs.size());
  };

  auto collectRunUniqueBodyIDs = [&](size_t &index, JoltDeferredOpType type) {
    std::vector<JPH::BodyID> bodyIDs;
    std::unordered_set<JPH::uint32> bodyIDSet;
    while (index < ops.size() && ops[index].type == type) {
      const JoltDeferredOp &runOp = ops[index];
      if (!runOp.bodyID.IsInvalid() &&
          bodyIDSet.insert(runOp.bodyID.GetIndexAndSequenceNumber()).second) {
        bodyIDs.push_back(runOp.bodyID);
      }
      ++index;
    }
    return bodyIDs;
  };

  size_t i = 0;
  while (i < ops.size()) {
    const JoltDeferredOp &op = ops[i];

    switch (op.type) {
      case JoltDeferredOpType::AddBody: {
        std::vector<JPH::BodyID> bodyIDs = collectRunUniqueBodyIDs(i, JoltDeferredOpType::AddBody);
        std::vector<JPH::BodyID> addBodyIDs;
        addBodyIDs.reserve(bodyIDs.size());
        for (const JPH::BodyID bodyID : bodyIDs) {
          if (!bi.IsAdded(bodyID)) {
            addBodyIDs.push_back(bodyID);
          }
        }
        addBodiesBatch(addBodyIDs, JPH::EActivation::Activate);
        break;
      }

      case JoltDeferredOpType::RemoveBody: {
        std::vector<JPH::BodyID> bodyIDs = collectRunUniqueBodyIDs(i, JoltDeferredOpType::RemoveBody);
        removePendingRigidAdds(bodyIDs);

        std::vector<JPH::BodyID> addedBodyIDs;
        addedBodyIDs.reserve(bodyIDs.size());
        for (const JPH::BodyID bodyID : bodyIDs) {
          if (bi.IsAdded(bodyID)) {
            addedBodyIDs.push_back(bodyID);
          }
        }
        removeBodiesBatch(addedBodyIDs);
        break;
      }

      case JoltDeferredOpType::DestroyBody: {
        std::vector<JPH::BodyID> bodyIDs = collectRunUniqueBodyIDs(i, JoltDeferredOpType::DestroyBody);
        removePendingRigidAdds(bodyIDs);
        destroyBodiesBatch(bodyIDs);
        break;
      }

      case JoltDeferredOpType::SuspendDynamics: {
        ++i;
        if (op.bodyID.IsInvalid()) {
          break;
        }

        if (op.ghost) {
          /* Ghost mode: make body a static sensor. */
          bi.SetMotionType(op.bodyID, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
          if (bi.IsAdded(op.bodyID)) {
            {
              JPH::BodyLockWrite lock(m_physicsSystem->GetBodyLockInterface(), op.bodyID);
              if (lock.Succeeded()) {
                lock.GetBody().SetIsSensor(true);
              }
            }
            bi.SetObjectLayer(op.bodyID,
                              JoltMakeObjectLayer(op.collisionGroup, op.collisionMask, JOLT_BP_SENSOR));
          }
        }
        else {
          /* Non-ghost: remove body from world entirely (no collision). */
          std::vector<JPH::BodyID> bodyIDs = {op.bodyID};
          removePendingRigidAdds(bodyIDs);
          if (bi.IsAdded(op.bodyID)) {
            bi.RemoveBody(op.bodyID);
          }
        }
        break;
      }

      case JoltDeferredOpType::RestoreDynamics: {
        ++i;
        if (op.bodyID.IsInvalid()) {
          break;
        }

        /* If body is not in the world (removed on suspend), add it back. */
        if (!bi.IsAdded(op.bodyID)) {
          std::vector<JPH::BodyID> addBodyIDs = {op.bodyID};
          addBodiesBatch(addBodyIDs, JPH::EActivation::Activate);
        }

        /* Restore motion type. */
        bi.SetMotionType(op.bodyID, op.motionType, JPH::EActivation::Activate);

        /* Restore sensor flag. */
        {
          JPH::BodyLockWrite lock(m_physicsSystem->GetBodyLockInterface(), op.bodyID);
          if (lock.Succeeded()) {
            lock.GetBody().SetIsSensor(op.ghost);
          }
        }

        /* Restore layer. */
        if (bi.IsAdded(op.bodyID)) {
          bi.SetObjectLayer(op.bodyID,
                            JoltMakeObjectLayer(op.collisionGroup, op.collisionMask, op.bpCategory));
        }
        break;
      }

      case JoltDeferredOpType::SetObjectLayer: {
        ++i;
        if (!op.bodyID.IsInvalid() && bi.IsAdded(op.bodyID)) {
          bi.SetObjectLayer(op.bodyID, op.objectLayer);
        }
        break;
      }

      case JoltDeferredOpType::SetMotionType: {
        ++i;
        if (!op.bodyID.IsInvalid()) {
          bi.SetMotionType(op.bodyID, op.motionType, JPH::EActivation::Activate);
        }
        break;
      }
    }
  }
}

/** \} */

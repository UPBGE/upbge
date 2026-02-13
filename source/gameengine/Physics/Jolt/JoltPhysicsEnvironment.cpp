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
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
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

#include "BKE_object.hh"
#include "BLI_bounds.hh"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BL_SceneConverter.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "KX_KetsjiEngine.h"
#include "KX_Scene.h"
#include "PHY_IMotionState.h"
#include "RAS_MeshObject.h"
#include "SG_Node.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdarg>

static int g_joltConstraintUid = 1;

#define CCD_CONSTRAINT_DISABLE_LINKED_COLLISION 0x80
#include <cstdio>
#include <thread>

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
  /* Called from physics threads — must be thread-safe. */
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

  /* Set default gravity (Blender Z-up: -9.81 on Z → Jolt Y-up: -9.81 on Y). */
  m_physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

  if (visualizePhysics) {
    m_debugMode = 1;
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
  for (JoltSoftBody *sb : m_softBodies) {
    delete sb;
  }
  m_softBodies.clear();

  /* Controllers are normally destroyed by KX_GameObject before we get here.
   * Each controller destructor removes its body and erases itself from m_controllers.
   * Handle any stragglers that weren't cleaned up by game objects. */
  {
    JPH::BodyInterface &bodyInterface = m_physicsSystem->GetBodyInterface();
    /* Copy the set since controller destructors modify m_controllers. */
    std::vector<JoltPhysicsController *> remaining(m_controllers.begin(), m_controllers.end());
    for (JoltPhysicsController *ctrl : remaining) {
      JPH::BodyID bodyID = ctrl->GetBodyID();
      if (!bodyID.IsInvalid()) {
        if (bodyInterface.IsAdded(bodyID)) {
          bodyInterface.RemoveBody(bodyID);
        }
        bodyInterface.DestroyBody(bodyID);
      }
      ctrl->SetEnvironment(nullptr);  /* Prevent controller destructor from double-freeing. */
      delete ctrl;  /* Free the controller and its motion state. */
    }
    m_controllers.clear();
  }

  /* Graphic controllers are owned by KX_GameObject, but clear stale pointers. */
  m_graphicControllers.clear();

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
    maxBodyPairs = 65536;
  }

  int maxContactConstraints = blenderscene->gm.jolt_max_contact_constraints;
  if (maxContactConstraints <= 0) {
    maxContactConstraints = 65536;
  }

  int tempAllocatorMB = blenderscene->gm.jolt_temp_allocator_mb;
  /* Jolt's Update() needs roughly maxBodies * 900 bytes of temp memory.
   * Compute a safe minimum and use the larger of user setting vs minimum. */
  int minTempMB = std::max(32, (int)((int64_t)maxBodies * 900 / (1024 * 1024)) + 16);
  if (tempAllocatorMB < minTempMB) {
    tempAllocatorMB = minTempMB;
  }
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

  int collisionSteps = m_numTimeSubSteps;
  if (collisionSteps < 1) {
    collisionSteps = 1;
  }

  /* Full simulation step sequence (matching Bullet's order for physics correctness):
   *   1. Sync motion states BEFORE physics (kinematic bodies read from game objects)
   *   2. PhysicsSystem::Update()
   *   3. Process FH springs
   *   4. Sync motion states AFTER physics (dynamic bodies write to game objects)
   *   5. CallbackTriggers()
   */

  /* One-time broad-phase optimization after scene load. */
  if (m_needsBroadPhaseOptimize) {
    m_physicsSystem->OptimizeBroadPhase();
    m_needsBroadPhaseOptimize = false;
  }

  /* Step 1: Write game object transforms to kinematic bodies. */
  for (JoltPhysicsController *ctrl : m_controllers) {
    ctrl->WriteMotionStateToDynamics(true);  /* nondynaonly = true → only kinematic/static */
  }

  /* Step 2: Run the physics simulation. */
  JPH::EPhysicsUpdateError updateErr = m_physicsSystem->Update(
      timeStep, collisionSteps, m_tempAllocator.get(), m_jobSystem.get());

  if (updateErr != JPH::EPhysicsUpdateError::None) {
    printf("Jolt: Update returned error %d\n", (int)updateErr);
  }

  /* Step 3: Process FH (Floating Height) springs. */
  ProcessFhSprings(timeStep);

  /* Step 3b: SimulationTick — clamp velocities (min/max linear/angular). */
  for (JoltPhysicsController *ctrl : m_controllers) {
    ctrl->SimulationTick(timeStep);
  }

  /* Step 4: Read Jolt body transforms back to game objects. */
  for (JoltPhysicsController *ctrl : m_controllers) {
    ctrl->SynchronizeMotionStates(timeStep);
  }

  /* Step 5: Update characters. */
  for (auto &pair : m_characterByObject) {
    pair.second->Update(timeStep);
  }

  /* Step 6: Sync vehicle wheels. */
  for (JoltVehicle *veh : m_vehicles) {
    veh->SyncWheels();
  }

  /* Step 7: Check breaking thresholds on constraints. */
  for (auto it = m_constraintById.begin(); it != m_constraintById.end(); ) {
    JoltConstraint *con = it->second;
    if (con->CheckBreaking()) {
      JPH::Constraint *joltCon = con->GetConstraint();
      if (joltCon) {
        m_physicsSystem->RemoveConstraint(joltCon);
      }
      delete con;
      it = m_constraintById.erase(it);
    }
    else {
      ++it;
    }
  }

  /* Step 8: Fire collision callbacks. */
  CallbackTriggers();

  /* Track active body count. */
  m_activeBodyCount = m_bodyActivationListener.GetActiveCount();

  return true;
}

void JoltPhysicsEnvironment::UpdateSoftBodies()
{
  for (JoltSoftBody *sb : m_softBodies) {
    sb->SyncVertices();
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
  /* Map to collision steps count. */
  if (numIter > 0) {
    m_numTimeSubSteps = numIter;
  }
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
  /* No direct Jolt equivalent. Store for reference. */
}

void JoltPhysicsEnvironment::SetERPContact(float erp2)
{
  /* No direct Jolt equivalent. Store for reference. */
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
  }
}

void JoltPhysicsEnvironment::RemoveSensor(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl || joltCtrl->GetBodyID().IsInvalid()) {
    return;
  }

  if (joltCtrl->Unregister()) {
    GetBodyInterface().RemoveBody(joltCtrl->GetBodyID());
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
  return true;
}

bool JoltPhysicsEnvironment::RemoveCollisionCallback(PHY_IPhysicsController *ctrl)
{
  JoltPhysicsController *joltCtrl = static_cast<JoltPhysicsController *>(ctrl);
  if (!joltCtrl) {
    return false;
  }
  m_collisionCallbackControllers.erase(joltCtrl);
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
  JPH::BodyLockRead lock0(lockInterface, jc0->GetBodyID());
  JPH::BodyLockRead lock1(lockInterface, jc1->GetBodyID());
  if (!lock0.Succeeded() || !lock1.Succeeded()) {
    return result;
  }

  const JPH::Body &body0 = lock0.GetBody();
  const JPH::Body &body1 = lock1.GetBody();

  /* Use CollideShape to check if the two shapes overlap. */
  JPH::CollideShapeSettings collideSettings;
  collideSettings.mMaxSeparationDistance = 0.0f;

  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

  const JPH::NarrowPhaseQuery &npq = m_physicsSystem->GetNarrowPhaseQuery();
  const JPH::Shape *shape0 = body0.GetShape();

  npq.CollideShape(
      shape0,
      JPH::Vec3::sReplicate(1.0f),
      body0.GetCenterOfMassTransform(),
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
    if (hit.mBodyID2 == body1.GetID()) {
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

  /* Skip compound children for now (Phase 3 handles full compound support). */
  if (isCompoundChild) {
    delete motionstate;
    return;
  }

  /* Determine bounds type. */
  char boundsType = isDyna ? OB_BOUND_SPHERE : OB_BOUND_TRIANGLE_MESH;
  if (!(blenderobject->gameflag & OB_BOUNDS)) {
    if (blenderobject->gameflag & OB_SOFT_BODY)
      boundsType = OB_BOUND_TRIANGLE_MESH;
    else if (blenderobject->gameflag & OB_CHARACTER)
      boundsType = OB_BOUND_SPHERE;
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
  float bounds_center[3] = {0.0f, 0.0f, 0.0f};
  float bounds_extends[3] = {1.0f, 1.0f, 1.0f};
  if (const std::optional<Bounds<float3>> bl_bounds = BKE_object_boundbox_eval_cached_get(
          blenderobject)) {
    const std::array<float3, 8> corners = bounds::corners(*bl_bounds);
    bounds_extends[0] = 0.5f * fabsf(corners[0][0] - corners[4][0]);
    bounds_extends[1] = 0.5f * fabsf(corners[0][1] - corners[2][1]);
    bounds_extends[2] = 0.5f * fabsf(corners[0][2] - corners[1][2]);
    bounds_center[0] = 0.5f * (corners[0][0] + corners[4][0]);
    bounds_center[1] = 0.5f * (corners[0][1] + corners[2][1]);
    bounds_center[2] = 0.5f * (corners[0][2] + corners[1][2]);
  }

  /* Build the collision shape. */
  JoltShapeBuilder shapeBuilder;
  float margin = isSoftBody ? 0.1f : blenderobject->margin;
  shapeBuilder.SetMargin(margin);

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

    JoltSoftBody *sb = new JoltSoftBody(this, ctrl);

    float stiffness = 1.0f;
    float sbFriction = blenderobject->friction;
    float sbDamping = blenderobject->damping;
    if (blenderobject->bsoft) {
      stiffness = blenderobject->bsoft->linStiff;
      sbDamping = blenderobject->bsoft->kDP;
    }

    const std::vector<float> &verts = shapeBuilder.GetVertexArray();
    const std::vector<int> &tris = shapeBuilder.GetTriangleArray();
    bool ok = sb->Create(verts.data(),
                         (int)verts.size() / 3,
                         tris.data(),
                         (int)tris.size() / 3,
                         mass,
                         pos,
                         margin,
                         stiffness,
                         sbFriction,
                         sbDamping);
    if (!ok) {
      delete sb;
      delete ctrl;
      delete motionstate;
      return;
    }

    ctrl->SetBodyID(sb->GetBodyID());
    ctrl->SetNewClientInfo(gameobj->getClientInfo());
    gameobj->SetPhysicsController(ctrl);

    /* Set collision group/mask on the soft body. */
    unsigned short collGroup = blenderobject->col_group;
    unsigned short collMask = blenderobject->col_mask;
    JPH::BodyInterface &bi = GetBodyInterface();
    const JPH::BodyLockInterface &lockIf = m_physicsSystem->GetBodyLockInterface();
    JPH::BodyLockWrite lock(lockIf, sb->GetBodyID());
    if (lock.Succeeded()) {
      lock.GetBody().SetCollisionGroup(
          JPH::CollisionGroup(m_constraintGroupFilter,
                              (JPH::CollisionGroup::GroupID)collGroup,
                              lock.GetBody().GetID().GetIndexAndSequenceNumber()));
      lock.GetBody().SetUserData(
          reinterpret_cast<JPH::uint64>(gameobj->getClientInfo()));
    }
    ctrl->SetCollisionGroup(collGroup);
    ctrl->SetCollisionMask(collMask);

    m_softBodies.push_back(sb);
    AddController(ctrl);
    return;
  }

  JPH::RefConst<JPH::Shape> shape = shapeBuilder.Build(useGimpact, scaling);
  if (!shape) {
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
    bi.AddBody(body->GetID(), JPH::EActivation::Activate);
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

int JoltPhysicsEnvironment::CreateRigidBodyConstraint(KX_GameObject *constraintObject,
                                                       KX_GameObject *gameobj1,
                                                       KX_GameObject *gameobj2,
                                                       blender::RigidBodyCon *rbc)
{
  using namespace blender;

  if (!constraintObject || !gameobj1 || !rbc) {
    return -1;
  }

  if (!constraintObject->GetSGNode() || !gameobj1->GetSGNode()) {
    return -1;
  }

  PHY_IPhysicsController *ctrl1 = gameobj1->GetPhysicsController();
  PHY_IPhysicsController *ctrl2 = gameobj2 ? gameobj2->GetPhysicsController() : nullptr;

  if (!ctrl1) {
    return -1;
  }

  const MT_Transform &worldTrans = constraintObject->NodeGetWorldTransform();
  const MT_Vector3 pivotWorld = worldTrans.getOrigin();
  const MT_Matrix3x3 basisWorld = worldTrans.getBasis();

  const MT_Transform &ob1Trans = gameobj1->NodeGetWorldTransform();
  MT_Transform ob1Inv;
  ob1Inv.invert(ob1Trans);

  MT_Vector3 pivotLocal = ob1Inv * pivotWorld;
  MT_Matrix3x3 basisLocal = ob1Inv.getBasis() * basisWorld;

  MT_Vector3 axis0 = basisLocal.getColumn(0).safe_normalized();
  MT_Vector3 axis1 = basisLocal.getColumn(1).safe_normalized();
  MT_Vector3 axis2 = basisLocal.getColumn(2).safe_normalized();

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
  m_controllers.insert(ctrl);
}

bool JoltPhysicsEnvironment::RemoveController(JoltPhysicsController *ctrl)
{
  return m_controllers.erase(ctrl) > 0;
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

  /* Swap contact pairs from the listener (accumulated during Update()). */
  std::vector<JoltContactPair> contacts;
  m_contactListener.SwapContacts(contacts);

  const JPH::BodyLockInterface &lockInterface = m_physicsSystem->GetBodyLockInterface();

  for (const JoltContactPair &pair : contacts) {
    /* Lock both bodies to read their user data. */
    JPH::BodyLockRead lock1(lockInterface, pair.bodyID1);
    JPH::BodyLockRead lock2(lockInterface, pair.bodyID2);
    if (!lock1.Succeeded() || !lock2.Succeeded()) {
      continue;
    }

    const JPH::Body &body1 = lock1.GetBody();
    const JPH::Body &body2 = lock2.GetBody();

    KX_ClientObjectInfo *info1 = reinterpret_cast<KX_ClientObjectInfo *>(body1.GetUserData());
    KX_ClientObjectInfo *info2 = reinterpret_cast<KX_ClientObjectInfo *>(body2.GetUserData());
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

  for (JoltPhysicsController *ctrl : m_controllers) {
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

/** \} */

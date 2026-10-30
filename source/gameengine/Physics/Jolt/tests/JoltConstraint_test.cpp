/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <Jolt/Jolt.h>
JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/StateRecorderImpl.h>

#include "DNA_scene_types.h"

#include "JoltConstraint.h"
#include "JoltDefaultMotionState.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "JoltSoftBody.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "SG_Node.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TestJoltPhysicsEnvironment : public JoltPhysicsEnvironment {
 public:
  using JoltPhysicsEnvironment::JoltPhysicsEnvironment;
  using JoltPhysicsEnvironment::ProcessBuoyancy;
  using JoltPhysicsEnvironment::ProcessPendingConstraintTopologyChanges;

  JoltConstraint *FindConstraint(const int id) const
  {
    const auto it = m_constraintById.find(id);
    return it != m_constraintById.end() ? it->second : nullptr;
  }

  void SeedLogicContact(const JPH::SubShapeIDPair &key,
                        JoltPhysicsController &controller1,
                        KX_GameObject &object1,
                        JoltPhysicsController &controller2,
                        KX_GameObject &object2)
  {
    JoltContactPair pair{};
    pair.subShapePair = key;
    pair.bodyID1 = controller1.GetBodyID();
    pair.bodyID2 = controller2.GetBodyID();
    pair.ctrl1 = &controller1;
    pair.ctrl2 = &controller2;
    pair.object1 = &object1;
    pair.object2 = &object2;
    pair.contactCount = 1;
    m_logicActiveContactPairs[key] = pair;
  }

  void QueueLogicContactRemoval(const JPH::SubShapeIDPair &key)
  {
    m_contactListener.OnContactRemoved(key);
  }

  void QueueLogicContactUpdate(const JPH::SubShapeIDPair &key)
  {
    const JPH::BodyID bodyIDs[2] = {key.GetBody1ID(), key.GetBody2ID()};
    JPH::BodyLockMultiRead lock(GetPhysicsSystem()->GetBodyLockInterface(), bodyIDs, 2);
    const JPH::Body *body1 = lock.GetBody(0);
    const JPH::Body *body2 = lock.GetBody(1);
    ASSERT_NE(body1, nullptr);
    ASSERT_NE(body2, nullptr);

    JPH::ContactManifold manifold{};
    manifold.mBaseOffset = JPH::RVec3::sZero();
    manifold.mWorldSpaceNormal = JPH::Vec3::sAxisX();
    manifold.mPenetrationDepth = 0.0f;
    manifold.mSubShapeID1 = key.GetSubShapeID1();
    manifold.mSubShapeID2 = key.GetSubShapeID2();
    manifold.mRelativeContactPointsOn1.push_back(JPH::Vec3::sZero());
    manifold.mRelativeContactPointsOn2.push_back(JPH::Vec3::sZero());

    JPH::ContactSettings settings{};
    settings.mCombinedFriction = 0.5f;
    settings.mCombinedRestitution = 0.0f;
    settings.mIsSensor = false;
    m_contactListener.OnContactAdded(*body1, *body2, manifold, settings);
  }

  size_t LogicContactCount() const
  {
    return m_logicActiveContactPairs.size();
  }

  bool IsLogicContactRetainedWhileDormant(const JPH::SubShapeIDPair &key) const
  {
    const auto it = m_logicActiveContactPairs.find(key);
    return it != m_logicActiveContactPairs.end() && it->second.retainedWhileDormant;
  }

  bool HasLogicContact(const JPH::SubShapeIDPair &key) const
  {
    return m_logicActiveContactPairs.find(key) != m_logicActiveContactPairs.end();
  }

  size_t LogicObjectSensorQueryCount() const
  {
    return m_logicObjectSensorOverlapsByController.size();
  }

  void SetPhysicsUpdatingForTest(const bool updating)
  {
    m_isPhysicsUpdating = updating;
  }

  JPH::JobSystem::Barrier *AcquireJobBarrierForTest()
  {
    return m_jobSystem ? m_jobSystem->CreateBarrier() : nullptr;
  }

  void ReleaseJobBarrierForTest(JPH::JobSystem::Barrier *barrier)
  {
    if (m_jobSystem && barrier) {
      m_jobSystem->DestroyBarrier(barrier);
    }
  }

  uint32_t ReportedUpdateErrors() const
  {
    return m_reportedUpdateErrors;
  }

  std::vector<uint8_t> SaveLegacyPhysicsState()
  {
    JPH::StateRecorderImpl recorder;
    GetPhysicsSystem()->SaveState(recorder);
    const std::string data = recorder.GetData();
    return std::vector<uint8_t>(data.begin(), data.end());
  }
};

class AcceptAllShapeCastFilter : public PHY_IShapeCastFilterCallback {
 public:
  AcceptAllShapeCastFilter() : PHY_IShapeCastFilterCallback(nullptr) {}
};

class AcceptAllRayCastFilter : public PHY_IRayCastFilterCallback {
 public:
  AcceptAllRayCastFilter() : PHY_IRayCastFilterCallback(nullptr) {}

  void reportHit(PHY_RayCastResult * /*result*/) override {}
};

class CountingRayCastFilter : public PHY_IRayCastFilterCallback {
 public:
  CountingRayCastFilter()
      : PHY_IRayCastFilterCallback(nullptr), caller_thread(std::this_thread::get_id())
  {
  }

  bool needBroadphaseRayCast(PHY_IPhysicsController *controller) override
  {
    if (std::this_thread::get_id() != caller_thread) {
      called_from_worker.store(true, std::memory_order_relaxed);
    }
    call_count.fetch_add(1, std::memory_order_relaxed);
    return controller != nullptr;
  }

  void reportHit(PHY_RayCastResult * /*result*/) override {}

  std::atomic<size_t> call_count = 0;
  std::atomic<bool> called_from_worker = false;

 private:
  std::thread::id caller_thread;
};

struct ShapeCastTarget {
  ShapeCastTarget(TestJoltPhysicsEnvironment &environment,
                  const JPH::RVec3 &position,
                  const bool sensor = false)
      : client_info(&object, sensor ? KX_ClientObjectInfo::SENSOR : KX_ClientObjectInfo::STATIC)
  {
    const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
    JPH::BodyCreationSettings settings(shape,
                                       position,
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Static,
                                       JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC));
    settings.mIsSensor = sensor;
    body_id = environment.GetBodyInterface().CreateAndAddBody(settings,
                                                              JPH::EActivation::DontActivate);
    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetNewClientInfo(&client_info);
    object.SetPhysicsController(&controller);
    environment.GetBodyInterface().SetUserData(body_id,
                                               reinterpret_cast<JPH::uint64>(&client_info));
  }

  ~ShapeCastTarget()
  {
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }

  KX_GameObject object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
};

struct LogicContactTarget {
  LogicContactTarget(TestJoltPhysicsEnvironment &environment,
                     const JPH::EMotionType motionType,
                     const JPH::RVec3 &position,
                     const unsigned short collisionLayer = 1,
                     JPH::RefConst<JPH::Shape> shape = nullptr,
                     const bool allowMotionTypeChange = false)
      : client_info(&object,
                    motionType == JPH::EMotionType::Static ? KX_ClientObjectInfo::STATIC :
                                                             KX_ClientObjectInfo::ACTOR)
  {
    const JoltBroadPhaseLayer broadPhaseLayer = motionType == JPH::EMotionType::Static ?
                                                    JOLT_BP_STATIC :
                                                    JOLT_BP_DYNAMIC;
    if (!shape) {
      shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
    }
    JPH::BodyCreationSettings settings(
        shape,
        position,
        JPH::Quat::sIdentity(),
        motionType,
        JoltMakeObjectLayer(collisionLayer, collisionLayer, broadPhaseLayer));
    settings.mAllowDynamicOrKinematic = allowMotionTypeChange;
    body_id = environment.GetBodyInterface().CreateAndAddBody(
        settings,
        motionType == JPH::EMotionType::Static ? JPH::EActivation::DontActivate :
                                                 JPH::EActivation::Activate);

    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetDynamic(motionType == JPH::EMotionType::Dynamic);
    controller.SetKinematic(motionType == JPH::EMotionType::Kinematic);
    controller.SetOriginalMotionType(motionType);
    controller.SetBroadPhaseCategory(broadPhaseLayer);
    controller.SetShape(shape, nullptr);
    controller.SetNewClientInfo(&client_info);
    object.SetPhysicsController(&controller);
    environment.AddController(&controller);
    environment.GetBodyInterface().SetUserData(body_id,
                                               reinterpret_cast<JPH::uint64>(&client_info));
  }

  ~LogicContactTarget()
  {
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }

  KX_GameObject object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
};

struct SoftBodyTarget {
  explicit SoftBodyTarget(TestJoltPhysicsEnvironment &environment)
      : client_info(&object, KX_ClientObjectInfo::ACTOR)
  {
    controller.SetEnvironment(&environment);
    controller.SetDynamic(true);
    controller.SetBroadPhaseCategory(JOLT_BP_DYNAMIC);
    controller.SetNewClientInfo(&client_info);

    std::unique_ptr<JoltSoftBody> softBody = std::make_unique<JoltSoftBody>(&environment,
                                                                            &controller);
    const float vertices[] = {
        -0.5f,
        -0.5f,
        0.0f,
        0.5f,
        -0.5f,
        0.0f,
        0.0f,
        0.5f,
        0.0f,
        0.0f,
        0.0f,
        0.75f,
    };
    const int triangles[] = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3};
    const std::unordered_map<int, int> vertexRemap = {{0, 0}, {1, 1}, {2, 2}, {3, 3}};
    JoltSoftBodySettings settings;
    settings.mass = 4.0f;
    settings.friction = 0.35f;
    settings.restitution = 0.1f;
    settings.damping = 0.15f;
    settings.pressure = 0.25f;
    settings.margin = 0.03f;
    settings.gravityFactor = 0.8f;
    settings.numIterations = 7;
    settings.facesDoubleSided = true;
    settings.pinVertexWeights = {{0, 1.0f}};
    valid = softBody->Create(vertices,
                             4,
                             triangles,
                             4,
                             MT_Vector3(0.0f, 0.0f, 2.0f),
                             MT_Matrix3x3::Identity(),
                             MT_Vector3(1.0f, 1.0f, 1.0f),
                             settings,
                             vertexRemap);
    EXPECT_TRUE(valid);
    if (!valid) {
      return;
    }

    body_id = softBody->GetBodyID();
    controller.SetBodyID(body_id);
    controller.SetSoftBody(softBody.release());
    object.SetPhysicsController(&controller);
    environment.AddController(&controller);
  }

  ~SoftBodyTarget()
  {
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }

  KX_GameObject object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
  bool valid = false;
};

struct LogicObjectSensorTarget {
  LogicObjectSensorTarget(
      TestJoltPhysicsEnvironment &environment,
      const JPH::RVec3 &position,
      const KX_ClientObjectInfo::clienttype sensorType = KX_ClientObjectInfo::OBSENSOR,
      const bool activeSceneObject = true,
      const bool includeStatic = false,
      const bool allowMotionTypeChange = false)
      : client_info(&object, sensorType)
  {
    const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
    JPH::BodyCreationSettings settings(shape,
                                       position,
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Static,
                                       JoltMakeObjectLayer(1, 1, JOLT_BP_SENSOR));
    settings.mIsSensor = true;
    settings.mAllowDynamicOrKinematic = allowMotionTypeChange;
    JPH::Body *body = environment.GetBodyInterface().CreateBody(settings);
    EXPECT_NE(body, nullptr);
    if (!body) {
      return;
    }

    body_id = body->GetID();
    body->SetUserData(reinterpret_cast<JPH::uint64>(&client_info));
    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetSensorFlag(true);
    controller.SetSensorPhysicsType(true);
    controller.SetGhostToggleSupported(false);
    controller.SetOriginalMotionType(JPH::EMotionType::Static);
    controller.SetBroadPhaseCategory(JOLT_BP_SENSOR);
    controller.SetShape(shape, nullptr);
    controller.SetNewClientInfo(&client_info);
    controller.SetLogicObjectSensorActive(activeSceneObject);
    controller.SetLogicObjectSensorIncludeStatic(includeStatic);
    object.SetPhysicsController(&controller);
    environment.AddController(&controller);
  }

  ~LogicObjectSensorTarget()
  {
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }

  KX_GameObject object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
};

struct DynamicSensorFlagTarget {
  DynamicSensorFlagTarget(TestJoltPhysicsEnvironment &environment,
                          const JPH::RVec3 &position,
                          const KX_ClientObjectInfo::clienttype clientType)
      : client_info(&object, clientType)
  {
    const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.25f));
    JPH::BodyCreationSettings settings(shape,
                                       position,
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Dynamic,
                                       JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC));
    settings.mIsSensor = true;
    body_id = environment.GetBodyInterface().CreateAndAddBody(settings,
                                                              JPH::EActivation::Activate);

    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetDynamic(true);
    controller.SetSensorFlag(true);
    controller.SetOriginalMotionType(JPH::EMotionType::Dynamic);
    controller.SetBroadPhaseCategory(JOLT_BP_DYNAMIC);
    controller.SetShape(shape, nullptr);
    controller.SetNewClientInfo(&client_info);
    object.SetPhysicsController(&controller);
    environment.AddController(&controller);
    environment.GetBodyInterface().SetUserData(body_id,
                                               reinterpret_cast<JPH::uint64>(&client_info));
  }

  ~DynamicSensorFlagTarget()
  {
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }

  KX_GameObject object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
};

struct TestObjectHierarchy {
  TestObjectHierarchy(KX_GameObject &parentObject, KX_GameObject &childObject)
      : parent(parentObject),
        child(childObject),
        parent_node(&parent, nullptr, callbacks),
        child_node(&child, nullptr, callbacks)
  {
    parent.SetSGNode(&parent_node);
    child.SetSGNode(&child_node);
    parent_node.AddChild(&child_node);
  }

  ~TestObjectHierarchy()
  {
    child_node.DisconnectFromParent();
    parent.SetSGNode(nullptr);
    child.SetSGNode(nullptr);
  }

  SG_Callbacks callbacks;
  KX_GameObject &parent;
  KX_GameObject &child;
  SG_Node parent_node;
  SG_Node child_node;
};

static void SetControllerQueryMotionState(JoltPhysicsController &controller,
                                          const MT_Vector3 &position)
{
  JoltDefaultMotionState *motionState = new JoltDefaultMotionState();
  motionState->SetWorldPosition(position);
  motionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  controller.SetMotionState(motionState);
}

static void EnableRuntimePhysicsTypeTransitions(JoltPhysicsController &controller)
{
  /* Conversion enables this only for a regular body that was created with
   * Jolt motion properties. Unit-test fixtures construct the controller
   * directly, so opt the fixture into the same contract explicitly. */
  controller.SetRuntimePhysicsTypeSupported(true);
  controller.CaptureOriginalPhysicsType();
}

TEST(JoltPhysicsController, TransformParentingDynamicRestoresAtCurrentSceneGraphPose)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamic(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(dynamic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  PHY_IMotionState *motion_state = dynamic.controller.GetMotionState();
  ASSERT_NE(motion_state, nullptr);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();
  const JPH::RefConst<JPH::Shape> original_shape = dynamic.controller.GetShape();

  ASSERT_TRUE(dynamic.controller.BeginTransformParenting());
  EXPECT_TRUE(dynamic.controller.IsTransformParentingActive());
  EXPECT_FALSE(body_interface.IsAdded(dynamic.body_id));

  motion_state->SetWorldPosition(MT_Vector3(5.0f, 0.0f, 0.0f));
  dynamic.controller.SetCollisionGroup(8);
  dynamic.controller.EndTransformParenting();

  EXPECT_FALSE(dynamic.controller.IsTransformParentingActive());
  EXPECT_TRUE(body_interface.IsAdded(dynamic.body_id));
  EXPECT_EQ(body_interface.GetMotionType(dynamic.body_id), JPH::EMotionType::Dynamic);
  EXPECT_NEAR(body_interface.GetPosition(dynamic.body_id).GetX(), 5.0f, 1.0e-5f);
  EXPECT_EQ(JoltGetCollisionLayers(body_interface.GetObjectLayer(dynamic.body_id)), 8);
  EXPECT_EQ(JoltGetCategory(body_interface.GetObjectLayer(dynamic.body_id)), JOLT_BP_DYNAMIC);
  EXPECT_EQ(dynamic.controller.GetShape().GetPtr(), original_shape.GetPtr());
}

TEST(JoltPhysicsController, AuthoredTransformParentingUsesManagedLifecycle)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  KX_GameObject parent;
  LogicContactTarget child(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  TestObjectHierarchy hierarchy(parent, child.object);
  SetControllerQueryMotionState(child.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  ASSERT_TRUE(child.object.BeginAuthoredTransformParenting());
  EXPECT_TRUE(child.object.GetParentPhysicsManaged());
  EXPECT_TRUE(child.controller.IsTransformParentingActive());
  EXPECT_FALSE(environment.GetBodyInterface().IsAdded(child.body_id));

  /* This covers recovery from an externally completed transition before the
   * object bookkeeping has been cleared. */
  child.controller.EndTransformParenting();
  ASSERT_TRUE(child.object.BeginAuthoredTransformParenting());
  EXPECT_TRUE(child.controller.IsTransformParentingActive());
  child.controller.EndTransformParenting();
}

TEST(JoltPhysicsController, GetVelocityUsesPointRelativeToBodyOrigin)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget body(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(11.0f, -7.0f, 5.0f));
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();
  body_interface.SetLinearVelocity(body.body_id, JPH::Vec3(1.0f, -2.0f, 3.0f));
  body_interface.SetAngularVelocity(body.body_id, JPH::Vec3(-0.5f, 0.75f, 1.25f));

  const MT_Vector3 relative_point(2.0f, -3.0f, 4.0f);
  const JPH::RVec3 world_point =
      body_interface.GetPosition(body.body_id) + JoltMath::ToJolt(relative_point);
  const MT_Vector3 expected_velocity = JoltMath::ToMT(
      body_interface.GetPointVelocity(body.body_id, world_point));
  const MT_Vector3 actual_velocity = body.controller.GetVelocity(relative_point);

  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(actual_velocity[axis], expected_velocity[axis], 1.0e-5f);
  }
}

TEST(JoltPhysicsController, RuntimePhysicsTypeTransitionsReuseExistingJoltBody)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamic(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(dynamic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  EnableRuntimePhysicsTypeTransitions(dynamic.controller);

  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  const JPH::Shape *const originalShape = dynamic.controller.GetShape().GetPtr();

  ASSERT_TRUE(dynamic.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  EXPECT_TRUE(dynamic.controller.IsKinematic());
  EXPECT_FALSE(dynamic.controller.IsDynamic());
  EXPECT_FALSE(dynamic.controller.GetRigidBodyRotationEnabled());
  EXPECT_TRUE(bodyInterface.IsAdded(dynamic.body_id));
  EXPECT_EQ(bodyInterface.GetMotionType(dynamic.body_id), JPH::EMotionType::Kinematic);
  EXPECT_EQ(dynamic.controller.GetShape().GetPtr(), originalShape);

  /* No Collision removes the body but keeps its conversion-time shape and
   * motion properties alive. An explicit type request reuses both rather than
   * rebuilding collision data in the gameplay path. */
  dynamic.controller.SetDynamicsMode(PHY_DynamicsMode::NoCollision, true);
  ASSERT_FALSE(bodyInterface.IsAdded(dynamic.body_id));
  ASSERT_TRUE(dynamic.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::RigidBody));
  EXPECT_TRUE(bodyInterface.IsAdded(dynamic.body_id));
  EXPECT_EQ(bodyInterface.GetMotionType(dynamic.body_id), JPH::EMotionType::Dynamic);
  EXPECT_TRUE(dynamic.controller.IsDynamic());
  EXPECT_TRUE(dynamic.controller.GetRigidBodyRotationEnabled());
  EXPECT_EQ(dynamic.controller.GetShape().GetPtr(), originalShape);

  ASSERT_TRUE(dynamic.controller.RestoreOriginalPhysicsType());
  EXPECT_EQ(bodyInterface.GetMotionType(dynamic.body_id), JPH::EMotionType::Dynamic);
  EXPECT_TRUE(dynamic.controller.IsDynamic());
  EXPECT_FALSE(dynamic.controller.IsKinematic());
  EXPECT_FALSE(dynamic.controller.GetRigidBodyRotationEnabled());
  EXPECT_FALSE(bodyInterface.IsSensor(dynamic.body_id));
  EXPECT_EQ(JoltGetCategory(bodyInterface.GetObjectLayer(dynamic.body_id)), JOLT_BP_DYNAMIC);
}

TEST(JoltPhysicsController, RuntimePhysicsTypePromotesPreparedStaticBodyToMovingRigidBody)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget staticBody(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero(), 1, nullptr, true);
  SetControllerQueryMotionState(staticBody.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  EnableRuntimePhysicsTypeTransitions(staticBody.controller);

  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  const JPH::Shape *const originalShape = staticBody.controller.GetShape().GetPtr();
  ASSERT_NE(originalShape, nullptr);
  ASSERT_TRUE(bodyInterface.IsAdded(staticBody.body_id));
  ASSERT_EQ(staticBody.client_info.m_type, KX_ClientObjectInfo::STATIC);

  environment.GetPhysicsSystem()->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
  ASSERT_TRUE(staticBody.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::RigidBody));
  EXPECT_EQ(bodyInterface.GetMotionType(staticBody.body_id), JPH::EMotionType::Dynamic);
  EXPECT_TRUE(bodyInterface.IsActive(staticBody.body_id));
  EXPECT_TRUE(staticBody.controller.IsDynamic());
  EXPECT_TRUE(staticBody.controller.GetRigidBodyRotationEnabled());
  EXPECT_EQ(staticBody.client_info.m_type, KX_ClientObjectInfo::ACTOR);
  EXPECT_EQ(staticBody.controller.GetShape().GetPtr(), originalShape);

  constexpr float timeStep = 1.0f / 60.0f;
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, timeStep, timeStep));
  EXPECT_LT(bodyInterface.GetPosition(staticBody.body_id).GetY(), 0.0f);

  ASSERT_TRUE(staticBody.controller.RestoreOriginalPhysicsType());
  EXPECT_EQ(bodyInterface.GetMotionType(staticBody.body_id), JPH::EMotionType::Static);
  EXPECT_EQ(staticBody.client_info.m_type, KX_ClientObjectInfo::STATIC);
  EXPECT_EQ(bodyInterface.GetLinearVelocity(staticBody.body_id), JPH::Vec3::sZero());
  EXPECT_EQ(bodyInterface.GetAngularVelocity(staticBody.body_id), JPH::Vec3::sZero());
  EXPECT_EQ(staticBody.controller.GetShape().GetPtr(), originalShape);
}

TEST(JoltPhysicsController, RuntimePhysicsTypePreservesIndependentGhostState)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget staticBody(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero(), 1, nullptr, true);
  SetControllerQueryMotionState(staticBody.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  staticBody.controller.SetGhostToggleSupported(true);
  EnableRuntimePhysicsTypeTransitions(staticBody.controller);

  staticBody.controller.SetDynamicsMode(PHY_DynamicsMode::Ghost, true);
  ASSERT_TRUE(environment.GetBodyInterface().IsSensor(staticBody.body_id));
  ASSERT_TRUE(staticBody.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::RigidBody));
  EXPECT_TRUE(environment.GetBodyInterface().IsSensor(staticBody.body_id));
  EXPECT_TRUE(staticBody.controller.IsSensor());

  ASSERT_TRUE(staticBody.controller.RestoreOriginalPhysicsType());
  EXPECT_FALSE(environment.GetBodyInterface().IsSensor(staticBody.body_id));
  EXPECT_FALSE(staticBody.controller.IsSensor());
}

TEST(JoltPhysicsController, RuntimePhysicsTypePreservesDynamicCcdPreference)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamic(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(dynamic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  dynamic.controller.SetCcdMotionThreshold(1.0f);
  EnableRuntimePhysicsTypeTransitions(dynamic.controller);

  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  ASSERT_EQ(bodyInterface.GetMotionQuality(dynamic.body_id), JPH::EMotionQuality::LinearCast);

  ASSERT_TRUE(dynamic.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  EXPECT_FALSE(dynamic.controller.GetCcdEnabled());
  EXPECT_EQ(bodyInterface.GetMotionQuality(dynamic.body_id), JPH::EMotionQuality::Discrete);

  ASSERT_TRUE(dynamic.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::RigidBody));
  EXPECT_TRUE(dynamic.controller.GetCcdEnabled());
  EXPECT_EQ(bodyInterface.GetMotionQuality(dynamic.body_id), JPH::EMotionQuality::LinearCast);

  dynamic.controller.SetCcdMotionThreshold(0.0f);
  ASSERT_TRUE(dynamic.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  ASSERT_TRUE(dynamic.controller.RestoreOriginalPhysicsType());
  EXPECT_FALSE(dynamic.controller.GetCcdEnabled());
  EXPECT_EQ(bodyInterface.GetMotionQuality(dynamic.body_id), JPH::EMotionQuality::Discrete);

  dynamic.controller.SetCcdMotionThreshold(1.0f);
  ASSERT_TRUE(dynamic.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  ASSERT_TRUE(dynamic.controller.RestoreOriginalPhysicsType());
  EXPECT_TRUE(dynamic.controller.GetCcdEnabled());
  EXPECT_EQ(bodyInterface.GetMotionQuality(dynamic.body_id), JPH::EMotionQuality::LinearCast);
}

TEST(JoltPhysicsController, KinematicGameStateRestoresDynamicCcdPreference)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget body(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(body.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  body.controller.SetCcdMotionThreshold(1.0f);
  EnableRuntimePhysicsTypeTransitions(body.controller);

  ASSERT_TRUE(body.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  PHY_GameStateRigidBodyState saved;
  ASSERT_TRUE(body.controller.CaptureGameStateRigidBody(saved));
  EXPECT_FALSE(saved.ccd_enabled);
  EXPECT_TRUE(saved.dynamic_ccd_enabled);

  ASSERT_TRUE(body.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Dynamic));
  body.controller.SetCcdMotionThreshold(0.0f);
  ASSERT_EQ(environment.GetBodyInterface().GetMotionQuality(body.body_id),
            JPH::EMotionQuality::Discrete);

  ASSERT_TRUE(body.controller.PrepareGameStateRigidBody(saved));
  body.controller.CommitPreparedGameStateRigidBody();
  EXPECT_TRUE(body.controller.IsKinematic());
  EXPECT_EQ(environment.GetBodyInterface().GetMotionQuality(body.body_id),
            JPH::EMotionQuality::Discrete);

  ASSERT_TRUE(body.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Dynamic));
  EXPECT_TRUE(body.controller.GetCcdEnabled());
  EXPECT_EQ(environment.GetBodyInterface().GetMotionQuality(body.body_id),
            JPH::EMotionQuality::LinearCast);
}

TEST(JoltPhysicsController, RuntimePhysicsTypePreservesDeferredCommandOrder)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamic(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(dynamic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  EnableRuntimePhysicsTypeTransitions(dynamic.controller);

  /* A Set Physics State request may be followed by legacy Static during the
   * same physics update. Restore Previous Physics State must restore the
   * logical Kinematic type, not the pre-request Dynamic Jolt body type. */
  environment.SetPhysicsUpdatingForTest(true);
  ASSERT_TRUE(dynamic.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  dynamic.controller.SetDynamicsMode(PHY_DynamicsMode::StaticCollider, true);
  environment.SetPhysicsUpdatingForTest(false);
  environment.ProcessDeferredOperations();

  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  EXPECT_TRUE(dynamic.controller.IsKinematic());
  EXPECT_TRUE(dynamic.controller.IsDynamicsSuspended());
  EXPECT_EQ(bodyInterface.GetMotionType(dynamic.body_id), JPH::EMotionType::Static);

  dynamic.controller.RestoreDynamics();
  EXPECT_FALSE(dynamic.controller.IsDynamicsSuspended());
  EXPECT_TRUE(dynamic.controller.IsKinematic());
  EXPECT_EQ(bodyInterface.GetMotionType(dynamic.body_id), JPH::EMotionType::Kinematic);
}

TEST(JoltPhysicsController, RuntimePhysicsTypeRejectsStaticBodiesWithoutMotionProperties)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget staticBody(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  staticBody.controller.SetRuntimePhysicsTypeSupported(true);
  staticBody.controller.CaptureOriginalPhysicsType();

  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  EXPECT_FALSE(staticBody.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Dynamic));
  EXPECT_FALSE(staticBody.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  EXPECT_EQ(bodyInterface.GetMotionType(staticBody.body_id), JPH::EMotionType::Static);
  EXPECT_FALSE(staticBody.controller.IsDynamic());
  EXPECT_FALSE(staticBody.controller.IsKinematic());
}

TEST(JoltPhysicsController, RuntimePhysicsTypeRejectsStaticOnlyShapeWithMotionProperties)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);

  JPH::VertexList vertices = {
      JPH::Float3(0.0f, 0.0f, 0.0f),
      JPH::Float3(0.0f, 0.0f, 1.0f),
      JPH::Float3(1.0f, 0.0f, 0.0f),
  };
  JPH::IndexedTriangleList triangles = {JPH::IndexedTriangle(0, 1, 2, 0, 0)};
  const JPH::Shape::ShapeResult meshResult =
      JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
  ASSERT_FALSE(meshResult.HasError());
  const JPH::RefConst<JPH::Shape> staticOnlyShape = meshResult.Get();
  ASSERT_TRUE(staticOnlyShape->MustBeStatic());

  LogicContactTarget staticBody(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero(), 1, staticOnlyShape, true);
  staticBody.controller.SetRuntimePhysicsTypeSupported(true);
  staticBody.controller.CaptureOriginalPhysicsType();

  EXPECT_TRUE(environment.GetBodyInterface().GetBodyType(staticBody.body_id) ==
              JPH::EBodyType::RigidBody);
  EXPECT_FALSE(staticBody.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::RigidBody));
  EXPECT_EQ(environment.GetBodyInterface().GetMotionType(staticBody.body_id),
            JPH::EMotionType::Static);
  EXPECT_EQ(staticBody.client_info.m_type, KX_ClientObjectInfo::STATIC);
}

TEST(JoltPhysicsController, RestoreOriginalPhysicsTypeRestoresAuthoredKinematicBody)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget kinematic(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(kinematic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  kinematic.controller.CaptureOriginalPhysicsType();

  /* The explicit Dynamic / Rigid Body / Kinematic overrides are deliberately
   * unavailable for this controller. Restoring its already-authored Kinematic
   * state remains safe because Jolt created its motion properties up front. */
  EXPECT_FALSE(kinematic.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  kinematic.controller.SetDynamicsMode(PHY_DynamicsMode::NoCollision, true);
  ASSERT_FALSE(environment.GetBodyInterface().IsAdded(kinematic.body_id));
  ASSERT_TRUE(kinematic.controller.RestoreOriginalPhysicsType());
  EXPECT_TRUE(environment.GetBodyInterface().IsAdded(kinematic.body_id));
  EXPECT_TRUE(kinematic.controller.IsKinematic());
  EXPECT_EQ(environment.GetBodyInterface().GetMotionType(kinematic.body_id),
            JPH::EMotionType::Kinematic);
}

TEST(JoltPhysicsController, TransformParentingHandlesStaticOnlyMeshSafely)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);

  JPH::VertexList vertices = {
      JPH::Float3(0.0f, 0.0f, 0.0f),
      JPH::Float3(0.0f, 0.0f, 1.0f),
      JPH::Float3(1.0f, 0.0f, 0.0f),
  };
  JPH::IndexedTriangleList triangles = {JPH::IndexedTriangle(0, 1, 2, 0, 0)};
  const JPH::Shape::ShapeResult mesh_result =
      JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
  ASSERT_FALSE(mesh_result.HasError());
  const JPH::RefConst<JPH::Shape> static_only_shape = mesh_result.Get();
  ASSERT_TRUE(static_only_shape->MustBeStatic());

  LogicContactTarget mesh(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero(), 1, static_only_shape);
  SetControllerQueryMotionState(mesh.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  PHY_IMotionState *motion_state = mesh.controller.GetMotionState();
  ASSERT_NE(motion_state, nullptr);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  EXPECT_TRUE(mesh.controller.SupportsTransformParenting());
  ASSERT_TRUE(mesh.controller.BeginTransformParenting());
  EXPECT_FALSE(body_interface.IsAdded(mesh.body_id));

  motion_state->SetWorldPosition(MT_Vector3(3.0f, 0.0f, 0.0f));
  mesh.controller.EndTransformParenting();

  EXPECT_TRUE(body_interface.IsAdded(mesh.body_id));
  EXPECT_EQ(body_interface.GetMotionType(mesh.body_id), JPH::EMotionType::Static);
  EXPECT_NEAR(body_interface.GetPosition(mesh.body_id).GetX(), 3.0f, 1.0e-5f);
  EXPECT_EQ(JoltGetCategory(body_interface.GetObjectLayer(mesh.body_id)), JOLT_BP_STATIC);
}

TEST(JoltPhysicsController, TransformParentingPreservesAuthoredKinematicAndSensorStates)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget kinematic(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(kinematic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  PHY_IMotionState *motion_state = kinematic.controller.GetMotionState();
  ASSERT_NE(motion_state, nullptr);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  ASSERT_TRUE(kinematic.controller.BeginTransformParenting());
  EXPECT_TRUE(kinematic.controller.IsKinematic());
  EXPECT_EQ(body_interface.GetMotionType(kinematic.body_id), JPH::EMotionType::Kinematic);
  EXPECT_FALSE(body_interface.IsSensor(kinematic.body_id));

  motion_state->SetWorldPosition(MT_Vector3(1.0f, 0.0f, 0.0f));
  constexpr float time_step = 0.25f;
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, time_step, time_step));
  ASSERT_GT(body_interface.GetLinearVelocity(kinematic.body_id).GetX(), 1.0f);
  kinematic.controller.EndTransformParenting();

  EXPECT_TRUE(kinematic.controller.IsKinematic());
  EXPECT_EQ(body_interface.GetMotionType(kinematic.body_id), JPH::EMotionType::Kinematic);
  EXPECT_FALSE(body_interface.IsSensor(kinematic.body_id));
  EXPECT_NEAR(body_interface.GetPosition(kinematic.body_id).GetX(), 1.0f, 1.0e-5f);
  EXPECT_NEAR(body_interface.GetLinearVelocity(kinematic.body_id).Length(), 0.0f, 1.0e-5f);
  EXPECT_EQ(JoltGetCategory(body_interface.GetObjectLayer(kinematic.body_id)), JOLT_BP_DYNAMIC);

  LogicObjectSensorTarget sensor(
      environment, JPH::RVec3::sZero(), KX_ClientObjectInfo::OBSENSOR, true, false, true);
  SetControllerQueryMotionState(sensor.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  PHY_IMotionState *sensor_motion_state = sensor.controller.GetMotionState();
  ASSERT_NE(sensor_motion_state, nullptr);

  ASSERT_TRUE(sensor.controller.BeginTransformParenting());
  EXPECT_TRUE(sensor.controller.IsKinematic());
  EXPECT_TRUE(body_interface.IsSensor(sensor.body_id));
  EXPECT_EQ(body_interface.GetMotionType(sensor.body_id), JPH::EMotionType::Kinematic);
  EXPECT_EQ(JoltGetCategory(body_interface.GetObjectLayer(sensor.body_id)), JOLT_BP_SENSOR);

  sensor_motion_state->SetWorldPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  sensor.controller.EndTransformParenting();

  EXPECT_FALSE(sensor.controller.IsKinematic());
  EXPECT_TRUE(body_interface.IsSensor(sensor.body_id));
  EXPECT_EQ(body_interface.GetMotionType(sensor.body_id), JPH::EMotionType::Static);
  EXPECT_NEAR(body_interface.GetPosition(sensor.body_id).GetX(), 2.0f, 1.0e-5f);
  EXPECT_EQ(JoltGetCategory(body_interface.GetObjectLayer(sensor.body_id)), JOLT_BP_SENSOR);
}

TEST(JoltPhysicsController, TransformParentingRestoresPreexistingSuspendedModes)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  LogicContactTarget dynamic(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(dynamic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  dynamic.controller.SuspendDynamics(true);
  ASSERT_TRUE(dynamic.controller.IsDynamicsSuspended());
  ASSERT_TRUE(body_interface.IsSensor(dynamic.body_id));
  ASSERT_EQ(body_interface.GetMotionType(dynamic.body_id), JPH::EMotionType::Static);

  ASSERT_TRUE(dynamic.controller.BeginTransformParenting());
  ASSERT_FALSE(body_interface.IsAdded(dynamic.body_id));
  dynamic.controller.EndTransformParenting();

  EXPECT_TRUE(dynamic.controller.IsDynamicsSuspended());
  EXPECT_TRUE(body_interface.IsAdded(dynamic.body_id));
  EXPECT_TRUE(body_interface.IsSensor(dynamic.body_id));
  EXPECT_EQ(body_interface.GetMotionType(dynamic.body_id), JPH::EMotionType::Static);
  dynamic.controller.RestoreDynamics();
  EXPECT_FALSE(body_interface.IsSensor(dynamic.body_id));
  EXPECT_EQ(body_interface.GetMotionType(dynamic.body_id), JPH::EMotionType::Dynamic);

  LogicContactTarget kinematic(
      environment, JPH::EMotionType::Kinematic, JPH::RVec3(2.0f, 0.0f, 0.0f));
  SetControllerQueryMotionState(kinematic.controller, MT_Vector3(2.0f, 0.0f, 0.0f));
  kinematic.controller.SetDynamicsMode(PHY_DynamicsMode::StaticCollider, true);
  ASSERT_TRUE(kinematic.controller.IsDynamicsSuspended());
  ASSERT_FALSE(body_interface.IsSensor(kinematic.body_id));
  ASSERT_EQ(body_interface.GetMotionType(kinematic.body_id), JPH::EMotionType::Static);

  ASSERT_TRUE(kinematic.controller.BeginTransformParenting());
  ASSERT_TRUE(kinematic.controller.IsKinematic());
  kinematic.controller.EndTransformParenting();

  EXPECT_TRUE(kinematic.controller.IsDynamicsSuspended());
  EXPECT_FALSE(body_interface.IsSensor(kinematic.body_id));
  EXPECT_EQ(body_interface.GetMotionType(kinematic.body_id), JPH::EMotionType::Static);
  kinematic.controller.RestoreDynamics();
  EXPECT_TRUE(kinematic.controller.IsKinematic());
  EXPECT_EQ(body_interface.GetMotionType(kinematic.body_id), JPH::EMotionType::Kinematic);
}

TEST(JoltPhysicsController, TransformParentingSafelyHandlesImmutableStaticSensor)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget sensor(environment, JPH::RVec3::sZero());
  SetControllerQueryMotionState(sensor.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  PHY_IMotionState *motion_state = sensor.controller.GetMotionState();
  ASSERT_NE(motion_state, nullptr);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();
  ASSERT_FALSE(body_interface.IsAdded(sensor.body_id));

  ASSERT_TRUE(sensor.controller.BeginTransformParenting());
  EXPECT_FALSE(sensor.controller.IsKinematic());
  EXPECT_FALSE(body_interface.IsAdded(sensor.body_id));
  EXPECT_TRUE(body_interface.IsSensor(sensor.body_id));
  EXPECT_EQ(body_interface.GetMotionType(sensor.body_id), JPH::EMotionType::Static);

  motion_state->SetWorldPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  sensor.controller.EndTransformParenting();

  EXPECT_FALSE(sensor.controller.IsKinematic());
  EXPECT_FALSE(body_interface.IsAdded(sensor.body_id));
  EXPECT_TRUE(body_interface.IsSensor(sensor.body_id));
  EXPECT_EQ(body_interface.GetMotionType(sensor.body_id), JPH::EMotionType::Static);
  EXPECT_NEAR(body_interface.GetPosition(sensor.body_id).GetX(), 2.0f, 1.0e-5f);
  EXPECT_EQ(JoltGetCategory(body_interface.GetObjectLayer(sensor.body_id)), JOLT_BP_SENSOR);
}

TEST(JoltPhysicsController, CompoundParentingOwnsChildShapeAndRestoresDynamicBody)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget root(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  LogicContactTarget child(environment, JPH::EMotionType::Dynamic, JPH::RVec3(2.0f, 0.0f, 0.0f));
  TestObjectHierarchy hierarchy(root.object, child.object);

  hierarchy.parent_node.SetWorldPosition(MT_Vector3(0.0f, 0.0f, 0.0f));
  hierarchy.parent_node.SetWorldOrientation(MT_Matrix3x3::Identity());
  hierarchy.parent_node.SetWorldScale(MT_Vector3(1.0f, 1.0f, 1.0f));
  hierarchy.child_node.SetWorldPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  hierarchy.child_node.SetWorldOrientation(MT_Matrix3x3::Identity());
  hierarchy.child_node.SetWorldScale(MT_Vector3(1.0f, 1.0f, 1.0f));
  SetControllerQueryMotionState(child.controller, MT_Vector3(2.0f, 0.0f, 0.0f));
  root.controller.SetCompoundFlag(true);

  JPH::BodyInterface &body_interface = environment.GetBodyInterface();
  ASSERT_TRUE(child.controller.SupportsCompoundParenting());
  ASSERT_TRUE(child.controller.BeginCompoundParenting());
  EXPECT_TRUE(child.controller.IsTransformParentingActive());
  EXPECT_FALSE(body_interface.IsAdded(child.body_id));

  ASSERT_TRUE(root.controller.TryAddCompoundChild(&child.controller));
  EXPECT_TRUE(root.controller.OwnsCompoundChildObject(&child.object));
  EXPECT_FALSE(child.controller.SupportsCompoundParenting());
  EXPECT_TRUE(root.controller.TryAddCompoundChild(&child.controller));

  root.controller.RemoveCompoundChild(&child.controller);
  EXPECT_FALSE(root.controller.OwnsCompoundChildObject(&child.object));
  child.controller.EndTransformParenting();

  EXPECT_FALSE(child.controller.IsTransformParentingActive());
  EXPECT_TRUE(child.controller.SupportsCompoundParenting());
  EXPECT_TRUE(body_interface.IsAdded(child.body_id));
  EXPECT_EQ(body_interface.GetMotionType(child.body_id), JPH::EMotionType::Dynamic);
  EXPECT_NEAR(body_interface.GetPosition(child.body_id).GetX(), 2.0f, 1.0e-5f);
}

TEST(JoltPhysicsController, CompoundParentingRejectsNonCompoundRootWithoutMutation)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget root(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  LogicContactTarget child(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  TestObjectHierarchy hierarchy(root.object, child.object);

  EXPECT_FALSE(root.controller.TryAddCompoundChild(&child.controller));
  EXPECT_FALSE(root.controller.OwnsCompoundChildObject(&child.object));
  EXPECT_TRUE(child.controller.SupportsCompoundParenting());
}

TEST(JoltPhysicsEnvironment, KinematicMotionStateUsesFixedStepVelocityAndStopsAtTarget)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget kinematic(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());
  LogicContactTarget dynamic(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  SetControllerQueryMotionState(kinematic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  PHY_IMotionState *motionState = kinematic.controller.GetMotionState();
  ASSERT_NE(motionState, nullptr);
  motionState->SetWorldPosition(MT_Vector3(0.5f, 0.0f, 0.0f));

  constexpr float timeStep = 0.25f;
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, timeStep, timeStep));

  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  EXPECT_NEAR(bodyInterface.GetPosition(kinematic.body_id).GetX(), 0.5f, 1.0e-4f);
  /* A body at rest has no velocity. The Dynamic body only moves if the
   * Kinematic target was supplied through MoveKinematic, not teleported. */
  EXPECT_GT(bodyInterface.GetPosition(dynamic.body_id).GetX(), 0.75f);

  /* The target is unchanged. A follow-up MoveKinematic must clear the motion
   * velocity; otherwise the body would keep travelling beyond the target. */
  ASSERT_TRUE(environment.ProceedDeltaTime(timeStep, timeStep, timeStep));
  EXPECT_NEAR(bodyInterface.GetPosition(kinematic.body_id).GetX(), 0.5f, 1.0e-4f);
  EXPECT_NEAR(bodyInterface.GetLinearVelocity(kinematic.body_id).Length(), 0.0f, 1.0e-4f);
}

TEST(JoltPhysicsEnvironment, KinematicSmallRotationTargetsAreNotQuantized)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget kinematic(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(kinematic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  PHY_IMotionState *motionState = kinematic.controller.GetMotionState();
  ASSERT_NE(motionState, nullptr);
  constexpr float smallAngle = 1.0e-3f;
  motionState->SetWorldOrientation(MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, smallAngle)));

  constexpr float timeStep = 1.0f / 60.0f;
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, timeStep, timeStep));

  const JPH::Quat rotation = environment.GetBodyInterface().GetRotation(kinematic.body_id);
  const float vectorPartLength = std::sqrt(rotation.GetX() * rotation.GetX() +
                                           rotation.GetY() * rotation.GetY() +
                                           rotation.GetZ() * rotation.GetZ());
  EXPECT_GT(vectorPartLength, 1.0e-4f);
  EXPECT_NEAR(vectorPartLength, std::sin(0.5f * smallAngle), 1.0e-5f);
}

TEST(JoltPhysicsController, KinematicReplicaStartsAtRestWithKinematicMotionType)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget source(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(source.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  /* Seed source-only in-flight velocity without advancing the simulation. */
  environment.GetBodyInterface().MoveKinematic(
      source.body_id, JPH::RVec3(3.0f, 0.0f, 0.0f), JPH::Quat::sIdentity(), 0.25f);
  ASSERT_GT(environment.GetBodyInterface().GetLinearVelocity(source.body_id).Length(), 1.0f);

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  JoltDefaultMotionState *replicaMotionState = new JoltDefaultMotionState();
  replicaMotionState->SetWorldPosition(MT_Vector3(5.0f, 0.0f, 0.0f));
  replicaMotionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  replica->PostProcessReplica(replicaMotionState, nullptr);

  ASSERT_FALSE(replica->GetBodyID().IsInvalid());
  EXPECT_TRUE(replica->IsKinematic());
  EXPECT_EQ(environment.GetBodyInterface().GetMotionType(replica->GetBodyID()),
            JPH::EMotionType::Kinematic);
  EXPECT_NEAR(
      environment.GetBodyInterface().GetPosition(replica->GetBodyID()).GetX(), 5.0f, 1.0e-5f);
  EXPECT_NEAR(environment.GetBodyInterface().GetLinearVelocity(replica->GetBodyID()).Length(),
              0.0f,
              1.0e-6f);
  EXPECT_FLOAT_EQ(replica->GetMass(), 0.0f);

  /* Replication code may reposition the completed hierarchy after the body was
   * cloned. Lifecycle rebasing must teleport to that final pose without
   * manufacturing a one-step velocity from the template location. */
  replicaMotionState->SetWorldPosition(MT_Vector3(9.0f, 0.0f, 0.0f));
  replica->RebaseKinematicMotionState();
  EXPECT_NEAR(
      environment.GetBodyInterface().GetPosition(replica->GetBodyID()).GetX(), 9.0f, 1.0e-5f);
  EXPECT_NEAR(environment.GetBodyInterface().GetLinearVelocity(replica->GetBodyID()).Length(),
              0.0f,
              1.0e-6f);

  replica->SetCcdMotionThreshold(1.0f);
  EXPECT_FALSE(replica->GetCcdEnabled());
  EXPECT_EQ(environment.GetBodyInterface().GetMotionQuality(replica->GetBodyID()),
            JPH::EMotionQuality::Discrete);
}

TEST(JoltPhysicsController, ReplicaRestoresAuthoredDynamicRotationOwnership)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget source(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(source.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  EnableRuntimePhysicsTypeTransitions(source.controller);

  /* The template was authored as non-rotating Dynamic, but its current
   * gameplay instance temporarily owns rotation as a Rigid Body. */
  ASSERT_TRUE(source.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::RigidBody));
  {
    JPH::BodyLockRead sourceLock(environment.GetPhysicsSystem()->GetBodyLockInterface(),
                                 source.body_id);
    ASSERT_TRUE(sourceLock.Succeeded());
    ASSERT_EQ(sourceLock.GetBody().GetMotionProperties()->GetAllowedDOFs(),
              JPH::EAllowedDOFs::All);
  }

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  JoltDefaultMotionState *replicaMotionState = new JoltDefaultMotionState();
  replicaMotionState->SetWorldPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  replicaMotionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  replica->PostProcessReplica(replicaMotionState, nullptr);

  ASSERT_FALSE(replica->GetBodyID().IsInvalid());
  EXPECT_FALSE(replica->GetRigidBodyRotationEnabled());
  JPH::BodyLockRead replicaLock(environment.GetPhysicsSystem()->GetBodyLockInterface(),
                                replica->GetBodyID());
  ASSERT_TRUE(replicaLock.Succeeded());
  const JPH::EAllowedDOFs translationOnly = JPH::EAllowedDOFs::TranslationX |
                                            JPH::EAllowedDOFs::TranslationY |
                                            JPH::EAllowedDOFs::TranslationZ;
  EXPECT_EQ(replicaLock.GetBody().GetMotionProperties()->GetAllowedDOFs(), translationOnly);
}

TEST(JoltPhysicsController, ReplicaRestoresAuthoredRigidBodyRotationOwnership)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget source(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(source.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  source.controller.SetRigidBody(true);
  EnableRuntimePhysicsTypeTransitions(source.controller);

  /* The inverse case is just as important: a Rigid Body template must not
   * inherit the rotation locks of a temporary Dynamic gameplay instance. */
  ASSERT_TRUE(source.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Dynamic));

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  JoltDefaultMotionState *replicaMotionState = new JoltDefaultMotionState();
  replicaMotionState->SetWorldPosition(MT_Vector3(3.0f, 0.0f, 0.0f));
  replicaMotionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  replica->PostProcessReplica(replicaMotionState, nullptr);

  ASSERT_FALSE(replica->GetBodyID().IsInvalid());
  EXPECT_TRUE(replica->GetRigidBodyRotationEnabled());
  JPH::BodyLockRead replicaLock(environment.GetPhysicsSystem()->GetBodyLockInterface(),
                                replica->GetBodyID());
  ASSERT_TRUE(replicaLock.Succeeded());
  EXPECT_EQ(replicaLock.GetBody().GetMotionProperties()->GetAllowedDOFs(),
            JPH::EAllowedDOFs::All);
}

TEST(JoltPhysicsController, DynamicReplicaDiscardsRuntimeKinematicVelocity)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget source(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(source.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  source.controller.SetCcdMotionThreshold(1.0f);
  EnableRuntimePhysicsTypeTransitions(source.controller);

  ASSERT_TRUE(source.controller.SetRuntimePhysicsType(PHY_RuntimePhysicsType::Kinematic));
  environment.GetBodyInterface().MoveKinematic(
      source.body_id, JPH::RVec3(4.0f, 0.0f, 0.0f), JPH::Quat::sIdentity(), 0.25f);
  ASSERT_GT(environment.GetBodyInterface().GetLinearVelocity(source.body_id).Length(), 1.0f);

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  JoltDefaultMotionState *replicaMotionState = new JoltDefaultMotionState();
  replicaMotionState->SetWorldPosition(MT_Vector3(6.0f, 0.0f, 0.0f));
  replicaMotionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  replica->PostProcessReplica(replicaMotionState, nullptr);

  ASSERT_FALSE(replica->GetBodyID().IsInvalid());
  EXPECT_TRUE(replica->IsDynamic());
  EXPECT_FALSE(replica->IsKinematic());
  EXPECT_FALSE(replica->GetRigidBodyRotationEnabled());
  EXPECT_EQ(environment.GetBodyInterface().GetMotionType(replica->GetBodyID()),
            JPH::EMotionType::Dynamic);
  EXPECT_EQ(environment.GetBodyInterface().GetMotionQuality(replica->GetBodyID()),
            JPH::EMotionQuality::LinearCast);
  EXPECT_NEAR(environment.GetBodyInterface().GetLinearVelocity(replica->GetBodyID()).Length(),
              0.0f,
              1.0e-6f);
  EXPECT_NEAR(environment.GetBodyInterface().GetAngularVelocity(replica->GetBodyID()).Length(),
              0.0f,
              1.0e-6f);
}

TEST(JoltPhysicsController, KinematicReplicaDiscardsTransientSuspensionGhostState)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget source(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(source.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  source.controller.SuspendDynamics(true);
  ASSERT_TRUE(source.controller.IsDynamicsSuspended());
  ASSERT_TRUE(environment.GetBodyInterface().IsSensor(source.body_id));

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  JoltDefaultMotionState *replicaMotionState = new JoltDefaultMotionState();
  replicaMotionState->SetWorldPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  replicaMotionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  replica->PostProcessReplica(replicaMotionState, nullptr);

  ASSERT_FALSE(replica->GetBodyID().IsInvalid());
  EXPECT_EQ(environment.GetBodyInterface().GetMotionType(replica->GetBodyID()),
            JPH::EMotionType::Kinematic);
  EXPECT_FALSE(environment.GetBodyInterface().IsSensor(replica->GetBodyID()));
  JPH::BodyLockRead lock(environment.GetPhysicsSystem()->GetBodyLockInterface(),
                         replica->GetBodyID());
  ASSERT_TRUE(lock.Succeeded());
  EXPECT_FALSE(lock.GetBody().GetCollideKinematicVsNonDynamic());
}

TEST(JoltPhysicsController, ReplicaDiscardsTransientTransformParentingState)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget source(
      environment, JPH::RVec3::sZero(), KX_ClientObjectInfo::OBSENSOR, true, false, true);
  SetControllerQueryMotionState(source.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  ASSERT_TRUE(source.controller.BeginTransformParenting());
  ASSERT_TRUE(source.controller.IsKinematic());
  ASSERT_EQ(environment.GetBodyInterface().GetMotionType(source.body_id),
            JPH::EMotionType::Kinematic);

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  JoltDefaultMotionState *replicaMotionState = new JoltDefaultMotionState();
  replicaMotionState->SetWorldPosition(MT_Vector3(3.0f, 0.0f, 0.0f));
  replicaMotionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  replica->PostProcessReplica(replicaMotionState, nullptr);

  ASSERT_FALSE(replica->GetBodyID().IsInvalid());
  EXPECT_FALSE(replica->IsKinematic());
  EXPECT_TRUE(environment.GetBodyInterface().IsSensor(replica->GetBodyID()));
  EXPECT_EQ(environment.GetBodyInterface().GetMotionType(replica->GetBodyID()),
            JPH::EMotionType::Static);
  EXPECT_FALSE(environment.GetBodyInterface().IsAdded(replica->GetBodyID()));
  JPH::BodyLockRead lock(environment.GetPhysicsSystem()->GetBodyLockInterface(),
                         replica->GetBodyID());
  ASSERT_TRUE(lock.Succeeded());
  EXPECT_FALSE(lock.GetBody().GetCollideKinematicVsNonDynamic());

  source.controller.EndTransformParenting();
}

TEST(JoltPhysicsController, DynamicReplicaDiscardsTransientSuspensionState)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget source(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(source.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  source.controller.SuspendDynamics(true);
  ASSERT_TRUE(source.controller.IsDynamicsSuspended());
  ASSERT_EQ(environment.GetBodyInterface().GetMotionType(source.body_id),
            JPH::EMotionType::Static);
  ASSERT_TRUE(environment.GetBodyInterface().IsSensor(source.body_id));

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  JoltDefaultMotionState *replicaMotionState = new JoltDefaultMotionState();
  replicaMotionState->SetWorldPosition(MT_Vector3(4.0f, 0.0f, 0.0f));
  replicaMotionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  replica->PostProcessReplica(replicaMotionState, nullptr);

  ASSERT_FALSE(replica->GetBodyID().IsInvalid());
  EXPECT_FALSE(replica->IsKinematic());
  EXPECT_FALSE(environment.GetBodyInterface().IsSensor(replica->GetBodyID()));
  EXPECT_EQ(environment.GetBodyInterface().GetMotionType(replica->GetBodyID()),
            JPH::EMotionType::Dynamic);
  EXPECT_TRUE(environment.GetBodyInterface().IsAdded(replica->GetBodyID()));
}

TEST(JoltPhysicsController, GhostReplicaCollidesLikeItsSource)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  /* A Static + Ghost pickup template: a Jolt sensor body, but not the Sensor physics type. */
  LogicContactTarget source(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  SetControllerQueryMotionState(source.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  {
    JPH::BodyLockWrite lock(environment.GetPhysicsSystem()->GetBodyLockInterface(),
                            source.body_id);
    ASSERT_TRUE(lock.Succeeded());
    lock.GetBody().SetIsSensor(true);
  }
  source.controller.SetSensorFlag(true);

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  JoltDefaultMotionState *replicaMotionState = new JoltDefaultMotionState();
  replicaMotionState->SetWorldPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  replicaMotionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  replica->PostProcessReplica(replicaMotionState, nullptr);

  ASSERT_FALSE(replica->GetBodyID().IsInvalid());
  EXPECT_TRUE(environment.GetBodyInterface().IsSensor(replica->GetBodyID()));
  /* Unlike a Sensor physics object, a ghost collides without a collision sensor. */
  EXPECT_TRUE(environment.GetBodyInterface().IsAdded(replica->GetBodyID()));
}

TEST(JoltPhysicsController, KinematicRestoreRebasesInactiveDisplacement)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget kinematic(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());
  SetControllerQueryMotionState(kinematic.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  PHY_IMotionState *motionState = kinematic.controller.GetMotionState();
  ASSERT_NE(motionState, nullptr);
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();

  bodyInterface.MoveKinematic(
      kinematic.body_id, JPH::RVec3(2.0f, 0.0f, 0.0f), JPH::Quat::sIdentity(), 0.25f);
  ASSERT_GT(bodyInterface.GetLinearVelocity(kinematic.body_id).Length(), 1.0f);

  kinematic.controller.SuspendPhysics(false);
  ASSERT_FALSE(bodyInterface.IsAdded(kinematic.body_id));
  motionState->SetWorldPosition(MT_Vector3(6.0f, 0.0f, 0.0f));
  kinematic.controller.RestorePhysics();
  ASSERT_TRUE(bodyInterface.IsAdded(kinematic.body_id));
  EXPECT_NEAR(bodyInterface.GetPosition(kinematic.body_id).GetX(), 6.0f, 1.0e-5f);
  EXPECT_NEAR(bodyInterface.GetLinearVelocity(kinematic.body_id).Length(), 0.0f, 1.0e-6f);

  kinematic.controller.SuspendDynamics(false);
  ASSERT_FALSE(bodyInterface.IsAdded(kinematic.body_id));
  motionState->SetWorldPosition(MT_Vector3(9.0f, 0.0f, 0.0f));
  kinematic.controller.RestoreDynamics();
  ASSERT_TRUE(bodyInterface.IsAdded(kinematic.body_id));
  EXPECT_EQ(bodyInterface.GetMotionType(kinematic.body_id), JPH::EMotionType::Kinematic);
  EXPECT_NEAR(bodyInterface.GetPosition(kinematic.body_id).GetX(), 9.0f, 1.0e-5f);
  EXPECT_NEAR(bodyInterface.GetLinearVelocity(kinematic.body_id).Length(), 0.0f, 1.0e-6f);

  /* Static Collider keeps the body in the world but temporarily changes its
   * Jolt motion type. Restoring it must still treat displacement as a lifecycle
   * teleport rather than a one-step Kinematic sweep. */
  kinematic.controller.SetDynamicsMode(PHY_DynamicsMode::StaticCollider, true);
  ASSERT_TRUE(kinematic.controller.IsDynamicsSuspended());
  ASSERT_TRUE(bodyInterface.IsAdded(kinematic.body_id));
  ASSERT_EQ(bodyInterface.GetMotionType(kinematic.body_id), JPH::EMotionType::Static);
  motionState->SetWorldPosition(MT_Vector3(12.0f, 0.0f, 0.0f));
  kinematic.controller.RestoreDynamics();
  EXPECT_EQ(bodyInterface.GetMotionType(kinematic.body_id), JPH::EMotionType::Kinematic);
  EXPECT_NEAR(bodyInterface.GetPosition(kinematic.body_id).GetX(), 12.0f, 1.0e-5f);
  EXPECT_NEAR(bodyInterface.GetLinearVelocity(kinematic.body_id).Length(), 0.0f, 1.0e-6f);
}

TEST(JoltPhysicsController, KinematicGhostControlsNonDynamicTriggerPairs)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget kinematic(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());

  const auto expectFlags = [&](const bool enabled) {
    JPH::BodyLockRead lock(environment.GetPhysicsSystem()->GetBodyLockInterface(),
                           kinematic.body_id);
    ASSERT_TRUE(lock.Succeeded());
    EXPECT_EQ(lock.GetBody().IsSensor(), enabled);
    EXPECT_EQ(lock.GetBody().GetCollideKinematicVsNonDynamic(), enabled);
  };

  expectFlags(false);
  kinematic.controller.SetDynamicsMode(PHY_DynamicsMode::Ghost, true);
  expectFlags(true);
  kinematic.controller.SetDynamicsMode(PHY_DynamicsMode::Ghost, false);
  expectFlags(false);
}

TEST(JoltPhysicsController, SensorPhysicsTypeCannotBecomeAnIncoherentSolidBody)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget sensor(environment, JPH::RVec3::sZero());
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();

  ASSERT_TRUE(sensor.controller.IsSensorPhysicsType());
  ASSERT_TRUE(bodyInterface.IsSensor(sensor.body_id));
  ASSERT_FALSE(bodyInterface.IsAdded(sensor.body_id));

  EXPECT_TRUE(sensor.controller.CanSetDynamicsMode(PHY_DynamicsMode::Ghost, true));
  EXPECT_FALSE(sensor.controller.CanSetDynamicsMode(PHY_DynamicsMode::Ghost, false));
  EXPECT_FALSE(sensor.controller.CanSetDynamicsMode(PHY_DynamicsMode::StaticCollider, true));

  sensor.controller.SetDynamicsMode(PHY_DynamicsMode::Ghost, false);
  sensor.controller.SetDynamicsMode(PHY_DynamicsMode::StaticCollider, true);
  EXPECT_FALSE(sensor.controller.IsDynamicsSuspended());
  EXPECT_TRUE(bodyInterface.IsSensor(sensor.body_id));
  EXPECT_EQ(bodyInterface.GetMotionType(sensor.body_id), JPH::EMotionType::Static);
  EXPECT_EQ(JoltGetCategory(bodyInterface.GetObjectLayer(sensor.body_id)), JOLT_BP_SENSOR);

  /* No Collision remains a valid reversible state, but restoring it must preserve the authored
   * Sensor's demand-driven broadphase lifecycle. */
  sensor.controller.SetDynamicsMode(PHY_DynamicsMode::NoCollision, true);
  EXPECT_TRUE(sensor.controller.IsDynamicsSuspended());
  sensor.controller.RestoreDynamics();
  EXPECT_FALSE(sensor.controller.IsDynamicsSuspended());
  EXPECT_FALSE(bodyInterface.IsAdded(sensor.body_id));
  EXPECT_TRUE(bodyInterface.IsSensor(sensor.body_id));

  environment.SetPhysicsUpdatingForTest(true);
  sensor.controller.SetDynamicsMode(PHY_DynamicsMode::NoCollision, true);
  environment.SetPhysicsUpdatingForTest(false);
  environment.ProcessDeferredOperations();
  ASSERT_TRUE(sensor.controller.IsDynamicsSuspended());

  environment.SetPhysicsUpdatingForTest(true);
  sensor.controller.RestoreDynamics();
  environment.SetPhysicsUpdatingForTest(false);
  environment.ProcessDeferredOperations();
  EXPECT_FALSE(sensor.controller.IsDynamicsSuspended());
  EXPECT_FALSE(bodyInterface.IsAdded(sensor.body_id));
  EXPECT_TRUE(bodyInterface.IsSensor(sensor.body_id));
}

TEST(JoltPhysicsController, KinematicIgnoresDynamicOnlyRotationAndAxisSettings)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget kinematic(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());

  kinematic.controller.SetRigidBody(true);
  kinematic.controller.SetRigidBodyAxisLocks(true, true, true, true, true, true);

  EXPECT_FALSE(kinematic.controller.GetRigidBodyRotationEnabled());
  bool lockTranslationX = true;
  bool lockTranslationY = true;
  bool lockTranslationZ = true;
  bool lockRotationX = true;
  bool lockRotationY = true;
  bool lockRotationZ = true;
  kinematic.controller.GetRigidBodyAxisLocks(lockTranslationX,
                                             lockTranslationY,
                                             lockTranslationZ,
                                             lockRotationX,
                                             lockRotationY,
                                             lockRotationZ);
  EXPECT_FALSE(lockTranslationX);
  EXPECT_FALSE(lockTranslationY);
  EXPECT_FALSE(lockTranslationZ);
  EXPECT_FALSE(lockRotationX);
  EXPECT_FALSE(lockRotationY);
  EXPECT_FALSE(lockRotationZ);
}

TEST(JoltPhysicsController, KinematicRejectsUnconvertibleStaticOnlyReplacementShape)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget kinematic(environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero());

  JPH::VertexList vertices = {
      JPH::Float3(0.0f, 0.0f, 0.0f),
      JPH::Float3(0.0f, 0.0f, 1.0f),
      JPH::Float3(1.0f, 0.0f, 0.0f),
  };
  JPH::IndexedTriangleList triangles = {JPH::IndexedTriangle(0, 1, 2, 0, 0)};
  const JPH::Shape::ShapeResult meshResult =
      JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
  ASSERT_FALSE(meshResult.HasError());
  const JPH::RefConst<JPH::Shape> staticOnlyShape = meshResult.Get();
  ASSERT_TRUE(staticOnlyShape->MustBeStatic());
  LogicContactTarget staticSource(
      environment, JPH::EMotionType::Static, JPH::RVec3(4.0f, 0.0f, 0.0f), 1, staticOnlyShape);

  const JPH::Shape *originalShape = kinematic.controller.GetShape().GetPtr();
  EXPECT_FALSE(kinematic.controller.ReplacePhysicsShape(&staticSource.controller));
  EXPECT_EQ(kinematic.controller.GetShape().GetPtr(), originalShape);
  EXPECT_FALSE(kinematic.controller.GetShape()->MustBeStatic());
}

class ScalingMotionState final : public PHY_IMotionState {
 public:
  explicit ScalingMotionState(const MT_Vector3 &scale) : m_scale(scale) {}

  MT_Vector3 GetWorldPosition() const override
  {
    return m_position;
  }
  MT_Vector3 GetWorldScaling() const override
  {
    return m_scale;
  }
  MT_Matrix3x3 GetWorldOrientation() const override
  {
    return m_orientation;
  }
  void SetWorldPosition(const MT_Vector3 &position) override
  {
    m_position = position;
  }
  void SetWorldOrientation(const MT_Matrix3x3 &orientation) override
  {
    m_orientation = orientation;
  }
  void SetWorldOrientation(const MT_Quaternion &orientation) override
  {
    m_orientation = MT_Matrix3x3(orientation);
  }
  void CalculateWorldTransformations() override {}
  void SetWorldScalingForTest(const MT_Vector3 &scale)
  {
    m_scale = scale;
  }

 private:
  MT_Vector3 m_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 m_scale;
  MT_Matrix3x3 m_orientation = MT_Matrix3x3::Identity();
};

TEST(JoltPhysicsController, KinematicFixedStepTracksParentComposedWorldScale)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  const JPH::RefConst<JPH::Shape> sourceShape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  LogicContactTarget kinematic(
      environment, JPH::EMotionType::Kinematic, JPH::RVec3::sZero(), 1, sourceShape);
  ScalingMotionState *motionState = new ScalingMotionState(MT_Vector3(1.0f, 1.0f, 1.0f));
  kinematic.controller.SetMotionState(motionState);
  motionState->SetWorldScalingForTest(MT_Vector3(2.0f, 3.0f, 4.0f));

  constexpr float timeStep = 1.0f / 60.0f;
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, timeStep, timeStep));

  const JPH::AABox bounds = kinematic.controller.GetShape()->GetLocalBounds();
  const JPH::Vec3 size = bounds.mMax - bounds.mMin;
  EXPECT_NEAR(size.GetX(), 2.0f, 1.0e-5f);
  EXPECT_NEAR(size.GetY(), 4.0f, 1.0e-5f); /* Blender Z -> Jolt Y. */
  EXPECT_NEAR(size.GetZ(), 3.0f, 1.0e-5f); /* Blender Y -> Jolt Z. */
}

static JPH::SubShapeIDPair MakeLogicContactKey(const JPH::BodyID body1,
                                               const JPH::BodyID body2,
                                               const JPH::uint32 subShape1,
                                               const JPH::uint32 subShape2)
{
  JPH::SubShapeID id1;
  JPH::SubShapeID id2;
  id1.SetValue(subShape1);
  id2.SetValue(subShape2);
  return JPH::SubShapeIDPair(body1, id1, body2, id2);
}

static PHY_IConstraint *CreateFixedConstraint(TestJoltPhysicsEnvironment &environment,
                                              JoltPhysicsController &controller1,
                                              JoltPhysicsController &controller2,
                                              int solverIterations)
{
  PHY_IConstraint *constraint = environment.CreateConstraint(&controller1,
                                                             &controller2,
                                                             PHY_FIXED_CONSTRAINT,
                                                             0.0f,
                                                             0.0f,
                                                             0.0f,
                                                             1.0f,
                                                             0.0f,
                                                             0.0f,
                                                             0.0f,
                                                             1.0f,
                                                             0.0f,
                                                             0.0f,
                                                             0.0f,
                                                             1.0f);
  if (!constraint) {
    return nullptr;
  }

  constraint->SetSolverIterations(solverIterations);
  return constraint;
}

TEST(JoltPhysicsController, TransformAndCompoundParentingRejectConstrainedChild)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget root(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  LogicContactTarget child(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(2.0f, 0.0f, 0.0f));
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  ASSERT_NE(CreateFixedConstraint(environment, child.controller, root.controller, 1), nullptr);
  ASSERT_TRUE(environment.HasConstraintsForBody(child.body_id));
  ASSERT_TRUE(body_interface.IsAdded(child.body_id));

  EXPECT_FALSE(child.controller.SupportsTransformParenting());
  EXPECT_FALSE(child.controller.BeginTransformParenting());
  EXPECT_FALSE(child.controller.SupportsCompoundParenting());
  EXPECT_FALSE(child.controller.BeginCompoundParenting());
  EXPECT_TRUE(environment.HasConstraintsForBody(child.body_id));
  EXPECT_TRUE(body_interface.IsAdded(child.body_id));
}

TEST(JoltPhysicsEnvironment, LogicObjectSensorQueriesStaticAndSleepingBodiesWithoutRegistration)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget sensor(
      environment, JPH::RVec3::sZero(), KX_ClientObjectInfo::OBSENSOR, true, true);
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget sleepingTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(-0.75f, 0.0f, 0.0f));
  LogicContactTarget layerFilteredTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.0f, 0.75f, 0.0f), 2);
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  bodyInterface.DeactivateBody(sleepingTarget.body_id);

  ASSERT_FALSE(bodyInterface.IsAdded(sensor.body_id));
  ASSERT_FALSE(bodyInterface.IsActive(sleepingTarget.body_id));

  /* The uncached first Logic Nodes tick must work before a physics step has populated cache. */
  PHY_CollisionTestResult directHit = environment.CheckCollision(
      &sensor.controller, &staticTarget.controller, true);
  ASSERT_TRUE(directHit.collide);
  ASSERT_TRUE(directHit.isFirst);
  ASSERT_NE(directHit.collData, nullptr);
  EXPECT_EQ(directHit.collData->GetNumContacts(), 1u);
  delete directHit.collData;
  EXPECT_FALSE(
      environment.CheckCollision(&staticTarget.controller, &sensor.controller, false).collide);

  environment.SetLogicCollisionContactCacheEnabled(true, true);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 0u);

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&sensor.controller, contacts, true));
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 1u);
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->size(), 2u);

  bool foundStatic = false;
  bool foundSleeping = false;
  for (const PHY_CachedCollisionContact *contact : *contacts) {
    ASSERT_NE(contact, nullptr);
    EXPECT_EQ(contact->ctrl0, &sensor.controller);
    EXPECT_EQ(contact->object0, &sensor.object);
    EXPECT_EQ(contact->contact_count, 1);
    ASSERT_EQ(contact->points.size(), 1u);
    ASSERT_EQ(contact->normals.size(), 1u);
    for (int axis = 0; axis < 3; ++axis) {
      EXPECT_TRUE(std::isfinite(contact->points[0][axis]));
      EXPECT_TRUE(std::isfinite(contact->normals[0][axis]));
    }
    foundStatic |= contact->object1 == &staticTarget.object;
    foundSleeping |= contact->object1 == &sleepingTarget.object;
  }
  EXPECT_TRUE(foundStatic);
  EXPECT_TRUE(foundSleeping);
  EXPECT_FALSE(
      environment.CheckCollision(&sensor.controller, &layerFilteredTarget.controller, false)
          .collide);

  /* Trigger overlaps are deliberately not injected into the solid targets' collision lists. */
  ASSERT_TRUE(
      environment.GetCachedCollisionContactRefs(&staticTarget.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
  ASSERT_TRUE(
      environment.GetCachedCollisionContactRefs(&sleepingTarget.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
}

TEST(JoltPhysicsEnvironment, LogicObjectSensorDetectsGhostButNotTrueSensor)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget sensor(
      environment, JPH::RVec3::sZero(), KX_ClientObjectInfo::OBSENSOR, true, true);
  LogicContactTarget ghost(environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicObjectSensorTarget trueSensor(
      environment, JPH::RVec3(-0.75f, 0.0f, 0.0f), KX_ClientObjectInfo::OBSENSOR, false);
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();

  /* A suspended/static Ghost can occupy JOLT_BP_SENSOR too. Its client type, not Jolt's
   * shared sensor flag or broadphase category, distinguishes it from a true Sensor object. */
  bodyInterface.SetIsSensor(ghost.body_id, true);
  bodyInterface.SetObjectLayer(ghost.body_id, JoltMakeObjectLayer(1, 1, JOLT_BP_SENSOR));
  ghost.controller.SetSensorFlag(true);
  ghost.controller.SetBroadPhaseCategory(JOLT_BP_SENSOR);
  bodyInterface.AddBody(trueSensor.body_id, JPH::EActivation::DontActivate);

  ASSERT_TRUE(bodyInterface.IsSensor(ghost.body_id));
  ASSERT_TRUE(bodyInterface.IsSensor(trueSensor.body_id));
  EXPECT_TRUE(environment.CheckCollision(&sensor.controller, &ghost.controller, false).collide);
  EXPECT_FALSE(
      environment.CheckCollision(&sensor.controller, &trueSensor.controller, false).collide);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  environment.CallbackTriggers();

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&sensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->size(), 1u);
  EXPECT_EQ((*contacts)[0]->object1, &ghost.object);

  /* Sensor query overlaps remain one-sided and never become solid collision data. */
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&ghost.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
}

TEST(JoltPhysicsEnvironment, LogicObjectSensorStaticOptInPrunesBroadphaseTrees)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget sensor(environment, JPH::RVec3::sZero());
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget sleepingTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(-0.75f, 0.0f, 0.0f));
  LogicContactTarget kinematicTarget(
      environment, JPH::EMotionType::Kinematic, JPH::RVec3(0.0f, 0.75f, 0.0f));
  LogicContactTarget staticGhost(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.0f, -0.75f, 0.0f));
  DynamicSensorFlagTarget dynamicGhost(
      environment, JPH::RVec3(0.0f, 0.0f, 0.6f), KX_ClientObjectInfo::ACTOR);
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();

  bodyInterface.DeactivateBody(sleepingTarget.body_id);
  bodyInterface.SetIsSensor(staticGhost.body_id, true);
  staticGhost.controller.SetSensorFlag(true);

  ASSERT_FALSE(sensor.controller.GetLogicObjectSensorIncludeStatic());
  EXPECT_FALSE(
      environment.CheckCollision(&sensor.controller, &staticTarget.controller, false).collide);
  EXPECT_FALSE(
      environment.CheckCollision(&sensor.controller, &staticGhost.controller, false).collide);
  EXPECT_TRUE(
      environment.CheckCollision(&sensor.controller, &sleepingTarget.controller, false).collide);
  EXPECT_TRUE(
      environment.CheckCollision(&sensor.controller, &kinematicTarget.controller, false).collide);
  EXPECT_TRUE(
      environment.CheckCollision(&sensor.controller, &dynamicGhost.controller, false).collide);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 0u);

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&sensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->size(), 3u);
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 1u);

  bool foundSleeping = false;
  bool foundKinematic = false;
  bool foundDynamicGhost = false;
  for (const PHY_CachedCollisionContact *contact : *contacts) {
    foundSleeping |= contact->object1 == &sleepingTarget.object;
    foundKinematic |= contact->object1 == &kinematicTarget.object;
    foundDynamicGhost |= contact->object1 == &dynamicGhost.object;
    EXPECT_NE(contact->object1, &staticTarget.object);
    EXPECT_NE(contact->object1, &staticGhost.object);
  }
  EXPECT_TRUE(foundSleeping);
  EXPECT_TRUE(foundKinematic);
  EXPECT_TRUE(foundDynamicGhost);

  /* A runtime configuration change invalidates this tick's cached empty/static decision. */
  sensor.controller.SetLogicObjectSensorIncludeStatic(true);
  EXPECT_TRUE(sensor.controller.GetLogicObjectSensorIncludeStatic());
  EXPECT_FALSE(environment.GetCachedCollisionContactRefs(&sensor.controller, contacts, false));
  EXPECT_TRUE(
      environment.CheckCollision(&sensor.controller, &staticTarget.controller, false).collide);
  EXPECT_TRUE(
      environment.CheckCollision(&sensor.controller, &staticGhost.controller, false).collide);

  environment.CallbackTriggers();
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&sensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_EQ(contacts->size(), 5u);
}

TEST(JoltPhysicsEnvironment, BuoyancyAffectsDynamicGhostButNotTrueSensor)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget volume(environment, JPH::RVec3::sZero());
  DynamicSensorFlagTarget ghost(
      environment, JPH::RVec3(0.2f, -0.1f, 0.0f), KX_ClientObjectInfo::ACTOR);
  DynamicSensorFlagTarget trueSensor(
      environment, JPH::RVec3(-0.2f, -0.1f, 0.0f), KX_ClientObjectInfo::OBSENSOR);
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();

  environment.GetPhysicsSystem()->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
  volume.controller.SetBuoyancyVolumeEnabled(true);
  volume.controller.SetBuoyancy(2.0f);
  volume.controller.SetBuoyancyLinearDrag(0.0f);
  volume.controller.SetBuoyancyAngularDrag(0.0f);
  bodyInterface.DeactivateBody(ghost.body_id);

  ASSERT_FALSE(bodyInterface.IsAdded(volume.body_id));
  ASSERT_TRUE(bodyInterface.IsSensor(ghost.body_id));
  ASSERT_TRUE(bodyInterface.IsSensor(trueSensor.body_id));
  ASSERT_TRUE(ghost.client_info.isActor());
  ASSERT_TRUE(trueSensor.client_info.isSensor());
  ASSERT_TRUE(ghost.controller.IsDynamic());
  ASSERT_TRUE(trueSensor.controller.IsDynamic());
  ASSERT_FALSE(bodyInterface.IsActive(ghost.body_id));
  EXPECT_TRUE(bodyInterface.GetLinearVelocity(ghost.body_id).IsNearZero());
  EXPECT_TRUE(bodyInterface.GetLinearVelocity(trueSensor.body_id).IsNearZero());

  environment.ProcessBuoyancy(1.0f / 60.0f);

  EXPECT_GT(bodyInterface.GetLinearVelocity(ghost.body_id).GetY(), 0.0f);
  EXPECT_TRUE(bodyInterface.IsActive(ghost.body_id));
  EXPECT_TRUE(bodyInterface.GetLinearVelocity(trueSensor.body_id).IsNearZero());
  EXPECT_FALSE(bodyInterface.IsAdded(volume.body_id));

  /* Legacy collision registration may temporarily add the same volume, but removing the
   * registration must not disable its independent query-based buoyancy behavior. */
  environment.AddSensor(&volume.controller);
  EXPECT_TRUE(bodyInterface.IsAdded(volume.body_id));
  environment.RemoveSensor(&volume.controller);
  EXPECT_FALSE(bodyInterface.IsAdded(volume.body_id));
  bodyInterface.SetLinearVelocity(ghost.body_id, JPH::Vec3::sZero());
  bodyInterface.DeactivateBody(ghost.body_id);
  environment.ProcessBuoyancy(1.0f / 60.0f);
  EXPECT_GT(bodyInterface.GetLinearVelocity(ghost.body_id).GetY(), 0.0f);
}

TEST(JoltPhysicsEnvironment, LogicObjectSensorRespectsActorActivityAndSuspensionRules)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget actorSensor(
      environment, JPH::RVec3::sZero(), KX_ClientObjectInfo::OBACTORSENSOR);
  LogicObjectSensorTarget inactiveSensor(
      environment, JPH::RVec3::sZero(), KX_ClientObjectInfo::OBSENSOR, false);
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.5f, 0.0f, 0.0f));
  LogicContactTarget actorTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(-0.5f, 0.0f, 0.0f));
  environment.GetBodyInterface().DeactivateBody(actorTarget.body_id);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  environment.CallbackTriggers();

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&actorSensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->size(), 1u);
  EXPECT_EQ((*contacts)[0]->object1, &actorTarget.object);

  ASSERT_TRUE(
      environment.GetCachedCollisionContactRefs(&inactiveSensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
  EXPECT_FALSE(
      environment.CheckCollision(&inactiveSensor.controller, &staticTarget.controller, false)
          .collide);

  actorSensor.controller.SuspendPhysics(false);
  environment.CallbackTriggers();
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&actorSensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
}

TEST(JoltPhysicsEnvironment, ParentedSuspendedRigidChildDoesNotQueryOrCollide)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget parent(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  LogicContactTarget child(environment, JPH::EMotionType::Dynamic, JPH::RVec3(10.0f, 0.0f, 0.0f));
  LogicContactTarget externalTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  TestObjectHierarchy hierarchy(parent.object, child.object);
  SetControllerQueryMotionState(child.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  child.controller.SetLogicCollisionQueryActive(true);
  child.controller.SuspendDynamics(false);
  EXPECT_FALSE(environment.GetBodyInterface().IsAdded(child.body_id));
  EXPECT_FALSE(child.controller.IsLogicCollisionQueryActive());

  EXPECT_FALSE(
      environment.CheckCollision(&child.controller, &externalTarget.controller, false).collide);
  EXPECT_FALSE(
      environment.CheckCollision(&externalTarget.controller, &child.controller, false).collide);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 0u);

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&child.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 0u);
}

TEST(JoltPhysicsEnvironment, RefreshCollisionDoesNotReactivateNoCollisionBody)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget target(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());

  /* Match the runtime command sequence across two frames: the first layer
   * write refreshes an added body, then No Collision removes it. Repeating
   * the same layer write must not reactivate the removed body. */
  target.object.SetCollisionGroup(5);
  target.controller.SuspendDynamics(false);
  ASSERT_FALSE(environment.GetBodyInterface().IsAdded(target.body_id));
  ASSERT_FALSE(environment.GetBodyInterface().IsActive(target.body_id));

  target.object.SetCollisionGroup(5);
  EXPECT_FALSE(environment.GetBodyInterface().IsAdded(target.body_id));
  EXPECT_FALSE(environment.GetBodyInterface().IsActive(target.body_id));
}

TEST(JoltPhysicsController, PreparedGameStateScalingMutatesOnlyAtCommit)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget target(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();
  const JPH::Vec3 linear_velocity(1.0f, -2.0f, 3.0f);
  const JPH::Vec3 angular_velocity(-0.5f, 0.75f, 1.25f);
  body_interface.SetLinearVelocity(target.body_id, linear_velocity);
  body_interface.SetAngularVelocity(target.body_id, angular_velocity);
  ASSERT_TRUE(body_interface.IsActive(target.body_id));

  const JPH::Shape *original_controller_shape = target.controller.GetShape().GetPtr();
  const JPH::Shape *original_body_shape = nullptr;
  {
    JPH::BodyLockRead lock(environment.GetPhysicsSystem()->GetBodyLockInterface(), target.body_id);
    ASSERT_TRUE(lock.Succeeded());
    original_body_shape = lock.GetBody().GetShape();
  }
  ASSERT_NE(original_controller_shape, nullptr);
  ASSERT_EQ(original_body_shape, original_controller_shape);

  const MT_Vector3 scale(2.0f, 1.5f, 0.75f);
  const MT_Vector3 original_scale(1.0f, 1.0f, 1.0f);
  ASSERT_TRUE(target.controller.PrepareGameStateScaling(scale, original_scale));
  EXPECT_EQ(target.controller.GetShape().GetPtr(), original_controller_shape);
  {
    JPH::BodyLockRead lock(environment.GetPhysicsSystem()->GetBodyLockInterface(), target.body_id);
    ASSERT_TRUE(lock.Succeeded());
    EXPECT_EQ(lock.GetBody().GetShape(), original_body_shape);
  }
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_FLOAT_EQ(body_interface.GetLinearVelocity(target.body_id)[axis], linear_velocity[axis]);
    EXPECT_FLOAT_EQ(body_interface.GetAngularVelocity(target.body_id)[axis],
                    angular_velocity[axis]);
  }
  EXPECT_TRUE(body_interface.IsActive(target.body_id));

  target.controller.ClearPreparedGameStateScaling();
  target.controller.CommitPreparedGameStateScaling();
  EXPECT_EQ(target.controller.GetShape().GetPtr(), original_controller_shape);
  EXPECT_TRUE(body_interface.IsActive(target.body_id));

  ASSERT_TRUE(target.controller.PrepareGameStateScaling(scale, original_scale));
  target.controller.CommitPreparedGameStateScaling();
  const JPH::Shape *committed_shape = target.controller.GetShape().GetPtr();
  EXPECT_NE(committed_shape, original_controller_shape);
  {
    JPH::BodyLockRead lock(environment.GetPhysicsSystem()->GetBodyLockInterface(), target.body_id);
    ASSERT_TRUE(lock.Succeeded());
    EXPECT_EQ(lock.GetBody().GetShape(), committed_shape);
  }
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_FLOAT_EQ(body_interface.GetLinearVelocity(target.body_id)[axis], linear_velocity[axis]);
    EXPECT_FLOAT_EQ(body_interface.GetAngularVelocity(target.body_id)[axis],
                    angular_velocity[axis]);
  }
  EXPECT_TRUE(body_interface.IsActive(target.body_id));
}

TEST(JoltPhysicsController, RepeatedGameStateScalingIsAbsoluteForBakedShapes)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  const JPH::RefConst<JPH::Shape> source_shape = new JPH::SphereShape(0.5f);
  LogicContactTarget target(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero(), 1, source_shape);

  const MT_Vector3 scales[] = {
      MT_Vector3(2.0f, 3.0f, 4.0f),
      MT_Vector3(0.5f, 1.25f, 2.0f),
  };
  const auto expect_scale = [&](const MT_Vector3 &scale) {
    const JPH::Vec3 expected_scale(scale[0], scale[2], scale[1]);
    const JPH::Shape::ShapeResult expected_result = source_shape->ScaleShape(expected_scale);
    EXPECT_FALSE(expected_result.HasError());
    if (expected_result.HasError()) {
      return;
    }
    const JPH::AABox expected_bounds = expected_result.Get()->GetLocalBounds();
    const JPH::AABox actual_bounds = target.controller.GetShape()->GetLocalBounds();
    for (int axis = 0; axis < 3; ++axis) {
      EXPECT_NEAR(actual_bounds.mMin[axis], expected_bounds.mMin[axis], 1.0e-5f);
      EXPECT_NEAR(actual_bounds.mMax[axis], expected_bounds.mMax[axis], 1.0e-5f);
    }
  };
  MT_Vector3 current_scale(1.0f, 1.0f, 1.0f);
  for (int iteration = 0; iteration < 8; ++iteration) {
    const MT_Vector3 &target_scale = scales[iteration % 2];
    ASSERT_TRUE(target.controller.PrepareGameStateScaling(target_scale, current_scale));
    target.controller.CommitPreparedGameStateScaling();
    expect_scale(target_scale);

    const MT_Vector3 gameplay_scale(1.5f, 0.75f, 2.5f);
    target.controller.SetScaling(gameplay_scale);
    expect_scale(gameplay_scale);
    current_scale = gameplay_scale;
  }
}

TEST(JoltPhysicsController, ReplicaScalingIsAbsoluteForBakedSourceShape)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  const JPH::RefConst<JPH::Shape> source_shape = new JPH::SphereShape(0.5f);
  const MT_Vector3 source_scale(2.0f, 3.0f, 4.0f);
  const JPH::Shape::ShapeResult baked_result = source_shape->ScaleShape(
      JPH::Vec3(source_scale[0], source_scale[2], source_scale[1]));
  ASSERT_FALSE(baked_result.HasError());
  ASSERT_NE(baked_result.Get(), nullptr);

  LogicContactTarget source(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero(), 1, baked_result.Get());
  source.controller.SetMotionState(new ScalingMotionState(source_scale));

  std::unique_ptr<JoltPhysicsController> replica(
      static_cast<JoltPhysicsController *>(source.controller.GetReplica()));
  ASSERT_NE(replica, nullptr);
  replica->PostProcessReplica(new ScalingMotionState(source_scale), nullptr);
  ASSERT_TRUE(replica->HasValidGameStateBody());

  const MT_Vector3 target_scale(1.0f, 6.0f, 2.0f);
  replica->SetScaling(target_scale);
  const JPH::Shape::ShapeResult expected_result = source_shape->ScaleShape(
      JPH::Vec3(target_scale[0], target_scale[2], target_scale[1]));
  ASSERT_FALSE(expected_result.HasError());
  ASSERT_NE(expected_result.Get(), nullptr);
  const JPH::AABox expected_bounds = expected_result.Get()->GetLocalBounds();
  const JPH::AABox actual_bounds = replica->GetShape()->GetLocalBounds();
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(actual_bounds.mMin[axis], expected_bounds.mMin[axis], 1.0e-5f);
    EXPECT_NEAR(actual_bounds.mMax[axis], expected_bounds.mMax[axis], 1.0e-5f);
  }
}

TEST(JoltPhysicsController, SoftBodyRejectsPreparedGameStateScaling)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  SoftBodyTarget target(environment);
  ASSERT_TRUE(target.valid);
  EXPECT_FALSE(target.controller.PrepareGameStateScaling(MT_Vector3(2.0f, 2.0f, 2.0f),
                                                         MT_Vector3(1.0f, 1.0f, 1.0f)));
  target.controller.CommitPreparedGameStateScaling();
}

TEST(JoltPhysicsController, PreparedRigidBodyGameStateRoundTripsSemanticRuntimeState)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget target(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  target.controller.SetRigidBodyFlag(true);
  target.controller.SetMassPropertiesTemplate(target.controller.GetShape()->GetMassProperties());
  target.controller.SetCollisionGroup(1);
  target.controller.SetCollisionMask(1);
  target.controller.SetMass(3.25f);
  target.controller.SetFriction(0.7f);
  target.controller.SetRestitution(0.2f);
  target.controller.SetDamping(0.15f, 0.35f);
  target.controller.SetGravityFactor(0.4f);
  target.controller.SetAllowSleeping(false);
  target.controller.SetLinVelocityMin(0.25f);
  target.controller.SetLinVelocityMax(20.0f);
  target.controller.SetAngularVelocityMin(0.1f);
  target.controller.SetAngularVelocityMax(10.0f);
  target.controller.SetRigidBodyAxisLocks(false, true, false, false, false, true);
  target.controller.SetCcdMotionThreshold(1.0f);
  target.controller.SetCollisionGroup(7);
  target.controller.SetCollisionMask(0x55);
  target.controller.SetLinearVelocity(MT_Vector3(1.0f, 0.0f, 2.0f), false);
  target.controller.SetAngularVelocity(MT_Vector3(0.5f, 0.75f, 0.0f), false);
  target.controller.ApplyForce(MT_Vector3(4.0f, -2.0f, 1.0f), false);
  target.controller.ApplyTorque(MT_Vector3(-1.0f, 3.0f, 2.0f), false);
  target.controller.SetDynamicsMode(PHY_DynamicsMode::Ghost, true);

  PHY_GameStateRigidBodyState saved;
  ASSERT_TRUE(target.controller.CaptureGameStateRigidBody(saved));
  EXPECT_EQ(saved.mode, PHY_DynamicsMode::Dynamic);
  EXPECT_TRUE(saved.ghost);
  EXPECT_TRUE(saved.awake);

  target.controller.SetDynamicsMode(PHY_DynamicsMode::Ghost, false);
  target.controller.SuspendDynamics(false);
  target.controller.SetMass(9.0f);
  target.controller.SetFriction(0.1f);
  target.controller.SetDamping(0.9f, 0.8f);
  target.controller.SetCollisionGroup(2);
  target.controller.SetCollisionMask(3);
  ASSERT_FALSE(body_interface.IsAdded(target.body_id));

  PHY_GameStateRigidBodyState mutated;
  ASSERT_TRUE(target.controller.CaptureGameStateRigidBody(mutated));
  ASSERT_TRUE(target.controller.PrepareGameStateRigidBody(saved));
  PHY_GameStateRigidBodyState still_mutated;
  ASSERT_TRUE(target.controller.CaptureGameStateRigidBody(still_mutated));
  EXPECT_EQ(still_mutated.mode, mutated.mode);
  EXPECT_FLOAT_EQ(still_mutated.mass, mutated.mass);
  EXPECT_FLOAT_EQ(still_mutated.friction, mutated.friction);

  target.controller.CommitPreparedGameStateRigidBody();
  ASSERT_TRUE(body_interface.IsAdded(target.body_id));
  EXPECT_EQ(body_interface.GetMotionType(target.body_id), JPH::EMotionType::Dynamic);
  EXPECT_TRUE(body_interface.IsSensor(target.body_id));
  EXPECT_TRUE(body_interface.IsActive(target.body_id));

  PHY_GameStateRigidBodyState restored;
  ASSERT_TRUE(target.controller.CaptureGameStateRigidBody(restored));
  EXPECT_EQ(restored.dynamic_body, saved.dynamic_body);
  EXPECT_EQ(restored.mode, saved.mode);
  EXPECT_EQ(restored.ghost, saved.ghost);
  EXPECT_EQ(restored.physics_suspended, saved.physics_suspended);
  EXPECT_EQ(restored.awake, saved.awake);
  EXPECT_EQ(restored.axis_locks, saved.axis_locks);
  EXPECT_EQ(restored.collision_group, saved.collision_group);
  EXPECT_EQ(restored.collision_mask, saved.collision_mask);
  EXPECT_EQ(restored.allow_sleeping, saved.allow_sleeping);
  EXPECT_EQ(restored.rotation_enabled, saved.rotation_enabled);
  EXPECT_EQ(restored.ccd_enabled, saved.ccd_enabled);
  EXPECT_EQ(restored.dynamic_ccd_enabled, saved.dynamic_ccd_enabled);
  EXPECT_FLOAT_EQ(restored.mass, saved.mass);
  EXPECT_FLOAT_EQ(restored.friction, saved.friction);
  EXPECT_FLOAT_EQ(restored.restitution, saved.restitution);
  EXPECT_FLOAT_EQ(restored.linear_damping, saved.linear_damping);
  EXPECT_FLOAT_EQ(restored.angular_damping, saved.angular_damping);
  EXPECT_FLOAT_EQ(restored.linear_velocity_min, saved.linear_velocity_min);
  EXPECT_FLOAT_EQ(restored.linear_velocity_max, saved.linear_velocity_max);
  EXPECT_FLOAT_EQ(restored.angular_velocity_min, saved.angular_velocity_min);
  EXPECT_FLOAT_EQ(restored.angular_velocity_max, saved.angular_velocity_max);
  EXPECT_FLOAT_EQ(restored.gravity_factor, saved.gravity_factor);
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_FLOAT_EQ(restored.linear_velocity[axis], saved.linear_velocity[axis]);
    EXPECT_FLOAT_EQ(restored.angular_velocity[axis], saved.angular_velocity[axis]);
    EXPECT_FLOAT_EQ(restored.pending_force[axis], saved.pending_force[axis]);
    EXPECT_FLOAT_EQ(restored.pending_torque[axis], saved.pending_torque[axis]);
  }
}

TEST(JoltPhysicsController, SuspendedModeRetainsIndependentGhostAcrossGameStateRestore)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget target(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  target.controller.SetRigidBodyFlag(true);
  target.controller.SetMassPropertiesTemplate(target.controller.GetShape()->GetMassProperties());
  target.controller.SetCollisionGroup(1);
  target.controller.SetCollisionMask(1);
  target.controller.SetDynamicsMode(PHY_DynamicsMode::Ghost, true);
  target.controller.SetDynamicsMode(PHY_DynamicsMode::StaticCollider, true);

  PHY_GameStateRigidBodyState saved;
  ASSERT_TRUE(target.controller.CaptureGameStateRigidBody(saved));
  EXPECT_EQ(saved.mode, PHY_DynamicsMode::StaticCollider);
  EXPECT_TRUE(saved.ghost);
  EXPECT_FALSE(saved.awake);
  EXPECT_EQ(body_interface.GetMotionType(target.body_id), JPH::EMotionType::Static);
  EXPECT_FALSE(body_interface.IsSensor(target.body_id));

  target.controller.RestoreDynamics();
  target.controller.SetDynamicsMode(PHY_DynamicsMode::Ghost, false);
  ASSERT_TRUE(target.controller.PrepareGameStateRigidBody(saved));
  target.controller.CommitPreparedGameStateRigidBody();

  PHY_GameStateRigidBodyState restored;
  ASSERT_TRUE(target.controller.CaptureGameStateRigidBody(restored));
  EXPECT_EQ(restored.mode, PHY_DynamicsMode::StaticCollider);
  EXPECT_TRUE(restored.ghost);
  EXPECT_EQ(body_interface.GetMotionType(target.body_id), JPH::EMotionType::Static);
  EXPECT_FALSE(body_interface.IsSensor(target.body_id));

  target.controller.RestoreDynamics();
  EXPECT_EQ(body_interface.GetMotionType(target.body_id), JPH::EMotionType::Dynamic);
  EXPECT_TRUE(body_interface.IsSensor(target.body_id));
}

TEST(JoltPhysicsController, PreparedRigidBodyGameStateRejectsOutOfRangeValues)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget target(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());

  target.controller.SetRigidBodyFlag(true);
  target.controller.SetMassPropertiesTemplate(target.controller.GetShape()->GetMassProperties());
  target.controller.SetCollisionGroup(1);
  target.controller.SetCollisionMask(1);

  PHY_GameStateRigidBodyState valid;
  ASSERT_TRUE(target.controller.CaptureGameStateRigidBody(valid));

  PHY_GameStateRigidBodyState invalid = valid;
  invalid.friction = -0.01f;
  EXPECT_FALSE(target.controller.PrepareGameStateRigidBody(invalid));

  invalid = valid;
  invalid.restitution = 1.01f;
  EXPECT_FALSE(target.controller.PrepareGameStateRigidBody(invalid));

  invalid = valid;
  invalid.linear_damping = 1.01f;
  EXPECT_FALSE(target.controller.PrepareGameStateRigidBody(invalid));

  invalid = valid;
  invalid.angular_velocity_min = -1.01f;
  EXPECT_FALSE(target.controller.PrepareGameStateRigidBody(invalid));
}

TEST(JoltSoftBody, RigidTransformMovesBodyAndPreservesDeformation)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  SoftBodyTarget target(environment);
  ASSERT_TRUE(target.valid);

  const MT_Vector3 oldOffset = target.controller.GetSoftBody()->GetBodyOriginOffset();
  const JPH::RVec3 oldBodyJolt = environment.GetBodyInterface().GetPosition(target.body_id);
  const MT_Vector3 oldBody = JoltMath::ToMT(JPH::Vec3(oldBodyJolt));
  const MT_Vector3 oldOrigin = oldBody - oldOffset;
  const MT_Vector3 translation(3.0f, -2.0f, 4.0f);
  const MT_Matrix3x3 orientation(0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);

  std::vector<MT_Vector3> oldPositions;
  std::vector<MT_Vector3> oldVelocities;
  {
    JPH::BodyLockWrite lock(environment.GetPhysicsSystem()->GetBodyLockInterface(),
                            target.body_id);
    ASSERT_TRUE(lock.Succeeded());
    JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(
        lock.GetBody().GetMotionProperties());
    ASSERT_NE(motion, nullptr);
    for (size_t index = 0; index < motion->GetVertices().size(); ++index) {
      JPH::SoftBodyVertex &vertex = motion->GetVertices()[index];
      vertex.mVelocity = JoltMath::ToJolt(
          MT_Vector3(0.5f + float(index), -0.25f * float(index), 1.0f));
      oldPositions.push_back(JoltMath::ToMT(vertex.mPosition));
      oldVelocities.push_back(JoltMath::ToMT(vertex.mVelocity));
    }
  }

  target.controller.SetSoftBodyTransform(translation, orientation);

  const MT_Vector3 expectedOffset = orientation * oldOffset;
  const MT_Vector3 expectedBody = oldOrigin + translation + expectedOffset;
  const MT_Vector3 observedBody = JoltMath::ToMT(
      JPH::Vec3(environment.GetBodyInterface().GetPosition(target.body_id)));
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(observedBody[axis], expectedBody[axis], 1.0e-5f);
    EXPECT_NEAR(target.controller.GetSoftBody()->GetBodyOriginOffset()[axis],
                expectedOffset[axis],
                1.0e-5f);
  }

  {
    JPH::BodyLockRead lock(environment.GetPhysicsSystem()->GetBodyLockInterface(), target.body_id);
    ASSERT_TRUE(lock.Succeeded());
    const JPH::SoftBodyMotionProperties *motion =
        static_cast<const JPH::SoftBodyMotionProperties *>(
            lock.GetBody().GetMotionPropertiesUnchecked());
    ASSERT_NE(motion, nullptr);
    ASSERT_EQ(motion->GetVertices().size(), oldPositions.size());
    for (size_t index = 0; index < motion->GetVertices().size(); ++index) {
      const MT_Vector3 position = JoltMath::ToMT(motion->GetVertices()[index].mPosition);
      const MT_Vector3 velocity = JoltMath::ToMT(motion->GetVertices()[index].mVelocity);
      const MT_Vector3 expectedPosition = orientation * oldPositions[index];
      const MT_Vector3 expectedVelocity = orientation * oldVelocities[index];
      for (int axis = 0; axis < 3; ++axis) {
        EXPECT_NEAR(position[axis], expectedPosition[axis], 1.0e-5f);
        EXPECT_NEAR(velocity[axis], expectedVelocity[axis], 1.0e-5f);
      }
    }
  }

  /* Reapplying the same absolute orientation must translate without rotating deformation twice. */
  const MT_Vector3 secondTranslation(-1.0f, 0.5f, 2.0f);
  target.controller.SetSoftBodyTransform(secondTranslation, orientation);
  const MT_Vector3 twiceMovedBody = JoltMath::ToMT(
      JPH::Vec3(environment.GetBodyInterface().GetPosition(target.body_id)));
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(twiceMovedBody[axis], expectedBody[axis] + secondTranslation[axis], 1.0e-5f);
  }
  {
    JPH::BodyLockRead lock(environment.GetPhysicsSystem()->GetBodyLockInterface(), target.body_id);
    ASSERT_TRUE(lock.Succeeded());
    const JPH::SoftBodyMotionProperties *motion =
        static_cast<const JPH::SoftBodyMotionProperties *>(
            lock.GetBody().GetMotionPropertiesUnchecked());
    ASSERT_NE(motion, nullptr);
    ASSERT_EQ(motion->GetVertices().size(), oldPositions.size());
    for (size_t index = 0; index < motion->GetVertices().size(); ++index) {
      const MT_Vector3 position = JoltMath::ToMT(motion->GetVertices()[index].mPosition);
      const MT_Vector3 velocity = JoltMath::ToMT(motion->GetVertices()[index].mVelocity);
      const MT_Vector3 expectedPosition = orientation * oldPositions[index];
      const MT_Vector3 expectedVelocity = orientation * oldVelocities[index];
      for (int axis = 0; axis < 3; ++axis) {
        EXPECT_NEAR(position[axis], expectedPosition[axis], 1.0e-5f);
        EXPECT_NEAR(velocity[axis], expectedVelocity[axis], 1.0e-5f);
      }
    }
  }
}

TEST(JoltPhysicsEnvironment, ParentedSensorRemainsAnActiveQueryAtCurrentTransform)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  KX_GameObject parent;
  LogicObjectSensorTarget sensor(
      environment, JPH::RVec3(10.0f, 0.0f, 0.0f), KX_ClientObjectInfo::OBSENSOR, true, true);
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  TestObjectHierarchy hierarchy(parent, sensor.object);
  SetControllerQueryMotionState(sensor.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  sensor.controller.SuspendDynamics(false);
  EXPECT_TRUE(sensor.controller.IsLogicCollisionQueryActive());
  EXPECT_TRUE(sensor.controller.IsLogicObjectSensorActive());
  EXPECT_TRUE(
      environment.CheckCollision(&sensor.controller, &staticTarget.controller, false).collide);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  environment.CallbackTriggers();
  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&sensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->size(), 1u);
  EXPECT_EQ((*contacts)[0]->object1, &staticTarget.object);
}

TEST(JoltPhysicsEnvironment, CompoundSubShapeContactsRouteToLogicalChildObject)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  environment.GetPhysicsSystem()->SetGravity(JPH::Vec3::sZero());
  LogicContactTarget parent(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  KX_GameObject child;
  TestObjectHierarchy hierarchy(parent.object, child);

  const JPH::RefConst<JPH::Shape> rootShape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.1f));
  const JPH::RefConst<JPH::Shape> childShape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  const JPH::uint32 childUserData = parent.controller.RegisterCompoundChildBinding(
      &child, JPH::Vec3::sZero(), JPH::Quat::sIdentity());
  ASSERT_NE(childUserData, 0u);

  JPH::StaticCompoundShapeSettings compoundSettings;
  compoundSettings.AddShape(JPH::Vec3(-3.0f, 0.0f, 0.0f), JPH::Quat::sIdentity(), rootShape, 0);
  compoundSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), childShape, childUserData);
  JPH::Shape::ShapeResult compoundResult = compoundSettings.Create();
  ASSERT_FALSE(compoundResult.HasError());
  parent.controller.SetShapePreservingMassPropertiesAndCenterOfMass(compoundResult.Get(), nullptr);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0f / 60.0f, 1.0f / 60.0f));

  const std::vector<const PHY_CachedCollisionContact *> *childContacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefsForObject(&child, childContacts, false));
  ASSERT_NE(childContacts, nullptr);
  ASSERT_FALSE(childContacts->empty());
  for (const PHY_CachedCollisionContact *contact : *childContacts) {
    ASSERT_NE(contact, nullptr);
    EXPECT_TRUE(contact->object0 == &child || contact->object1 == &child);
    EXPECT_TRUE(contact->object0 == &staticTarget.object ||
                contact->object1 == &staticTarget.object);
  }

  const std::vector<const PHY_CachedCollisionContact *> *parentContacts = nullptr;
  ASSERT_TRUE(
      environment.GetCachedCollisionContactRefs(&parent.controller, parentContacts, false));
  ASSERT_NE(parentContacts, nullptr);
  EXPECT_FALSE(parentContacts->empty());

  environment.GetBodyInterface().SetPosition(
      staticTarget.body_id, JPH::RVec3(10.0f, 0.0f, 0.0f), JPH::EActivation::Activate);
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0f / 60.0f, 1.0f / 60.0f));
  ASSERT_TRUE(environment.GetCachedCollisionContactRefsForObject(&child, childContacts, false));
  ASSERT_NE(childContacts, nullptr);
  EXPECT_TRUE(childContacts->empty());
}

TEST(JoltPhysicsEnvironment, LogicContactRemovalRetainsOnlyDormantTouchingPairs)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget staticTarget(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  environment.SetLogicCollisionContactCacheEnabled(true, false);

  const JPH::SubShapeIDPair key = MakeLogicContactKey(
      dynamicTarget.body_id, staticTarget.body_id, 1, 2);
  ASSERT_TRUE(bodyInterface.IsActive(dynamicTarget.body_id));
  ASSERT_TRUE(
      environment.CheckCollision(&dynamicTarget.controller, &staticTarget.controller, false)
          .collide);

  /* A live body can lose or replace a box/mesh manifold while its speculative
   * shapes still overlap. That exact contact must be removed. */
  environment.SeedLogicContact(key,
                               dynamicTarget.controller,
                               dynamicTarget.object,
                               staticTarget.controller,
                               staticTarget.object);
  environment.QueueLogicContactRemoval(key);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicContactCount(), 0u);

  /* Jolt also removes contacts when every movable body in the pair sleeps.
   * Preserve this case so sleep is not exposed as a collision exit. */
  environment.SeedLogicContact(key,
                               dynamicTarget.controller,
                               dynamicTarget.object,
                               staticTarget.controller,
                               staticTarget.object);
  bodyInterface.DeactivateBody(dynamicTarget.body_id);
  ASSERT_FALSE(bodyInterface.IsActive(dynamicTarget.body_id));
  environment.QueueLogicContactRemoval(key);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicContactCount(), 1u);
  EXPECT_TRUE(environment.IsLogicContactRetainedWhileDormant(key));

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(
      environment.GetCachedCollisionContactRefs(&dynamicTarget.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_EQ(contacts->size(), 1u);

  /* A retained sleep contact is only valid while the pair remains dormant.
   * Waking without a fresh contact update must expose the collision exit. */
  bodyInterface.ActivateBody(dynamicTarget.body_id);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicContactCount(), 0u);
  ASSERT_TRUE(
      environment.GetCachedCollisionContactRefs(&dynamicTarget.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
}

TEST(JoltPhysicsEnvironment, LogicContactRemovalDoesNotRetainDormantSeparation)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget staticTarget(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  environment.SetLogicCollisionContactCacheEnabled(true, false);

  const JPH::SubShapeIDPair key = MakeLogicContactKey(
      dynamicTarget.body_id, staticTarget.body_id, 3, 4);
  environment.SeedLogicContact(key,
                               dynamicTarget.controller,
                               dynamicTarget.object,
                               staticTarget.controller,
                               staticTarget.object);
  bodyInterface.DeactivateBody(dynamicTarget.body_id);
  bodyInterface.SetPosition(
      dynamicTarget.body_id, JPH::RVec3(10.0f, 0.0f, 0.0f), JPH::EActivation::DontActivate);
  ASSERT_FALSE(bodyInterface.IsActive(dynamicTarget.body_id));
  ASSERT_FALSE(
      environment.CheckCollision(&dynamicTarget.controller, &staticTarget.controller, false)
          .collide);

  environment.QueueLogicContactRemoval(key);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicContactCount(), 0u);
}

TEST(JoltPhysicsEnvironment, LogicContactEventsPreservePerPairCallbackOrder)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget staticTarget(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  environment.SetLogicCollisionContactCacheEnabled(true, false);

  const JPH::SubShapeIDPair key = MakeLogicContactKey(
      dynamicTarget.body_id, staticTarget.body_id, 5, 6);

  /* A later removal in the same game tick must win. */
  environment.QueueLogicContactUpdate(key);
  environment.QueueLogicContactRemoval(key);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicContactCount(), 0u);

  /* A later update in the same game tick must win. */
  environment.QueueLogicContactRemoval(key);
  environment.QueueLogicContactUpdate(key);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicContactCount(), 1u);
}

TEST(JoltPhysicsEnvironment, LogicContactEventsPreserveIndicesAcrossShards)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget firstTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget secondTarget(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  LogicContactTarget thirdTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(1.5f, 0.0f, 0.0f));
  environment.SetLogicCollisionContactCacheEnabled(true, false);

  const JPH::SubShapeIDPair removedKey = MakeLogicContactKey(
      firstTarget.body_id, secondTarget.body_id, 11, 12);
  const JPH::SubShapeIDPair addedKey = MakeLogicContactKey(
      firstTarget.body_id, thirdTarget.body_id, 13, 14);
  const auto shardForKey = [](const JPH::SubShapeIDPair &key) {
    return (size_t(key.GetBody1ID().GetIndexAndSequenceNumber()) ^
            (size_t(key.GetBody2ID().GetIndexAndSequenceNumber()) * 0x9E3779B1u)) &
           (JOLT_CONTACT_BUFFER_SHARDS - 1);
  };
  ASSERT_NE(shardForKey(removedKey), shardForKey(addedKey));

  environment.QueueLogicContactUpdate(removedKey);
  environment.QueueLogicContactRemoval(removedKey);
  environment.QueueLogicContactRemoval(addedKey);
  environment.QueueLogicContactUpdate(addedKey);
  environment.CallbackTriggers();

  EXPECT_FALSE(environment.HasLogicContact(removedKey));
  EXPECT_TRUE(environment.HasLogicContact(addedKey));
  EXPECT_EQ(environment.LogicContactCount(), 1u);
}

TEST(JoltPhysicsEnvironment, SuspendPhysicsImmediatelyInvalidatesLogicContacts)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget staticTarget(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  environment.SetLogicCollisionContactCacheEnabled(true, false);

  const JPH::SubShapeIDPair key = MakeLogicContactKey(
      dynamicTarget.body_id, staticTarget.body_id, 7, 8);
  environment.SeedLogicContact(key,
                               dynamicTarget.controller,
                               dynamicTarget.object,
                               staticTarget.controller,
                               staticTarget.object);
  ASSERT_EQ(environment.LogicContactCount(), 1u);

  dynamicTarget.controller.SuspendPhysics(false);
  EXPECT_EQ(environment.LogicContactCount(), 0u);
  EXPECT_FALSE(environment.GetBodyInterface().IsAdded(dynamicTarget.body_id));
}

TEST(JoltPhysicsEnvironment, DeferredSuspendInvalidatesLogicContactsBeforeCallbacks)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget staticTarget(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  environment.SetLogicCollisionContactCacheEnabled(true, false);

  const JPH::SubShapeIDPair key = MakeLogicContactKey(
      dynamicTarget.body_id, staticTarget.body_id, 15, 16);
  environment.SeedLogicContact(key,
                               dynamicTarget.controller,
                               dynamicTarget.object,
                               staticTarget.controller,
                               staticTarget.object);
  ASSERT_EQ(environment.LogicContactCount(), 1u);

  environment.SetPhysicsUpdatingForTest(true);
  dynamicTarget.controller.SuspendPhysics(false);
  EXPECT_TRUE(environment.GetBodyInterface().IsAdded(dynamicTarget.body_id));
  environment.SetPhysicsUpdatingForTest(false);
  environment.ProcessDeferredOperations();

  EXPECT_EQ(environment.LogicContactCount(), 0u);
  EXPECT_FALSE(environment.GetBodyInterface().IsAdded(dynamicTarget.body_id));
}

TEST(JoltPhysicsEnvironment, RestorePhysicsStateRestoresLogicContactsAndDropsLaterEvents)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget staticTarget(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  environment.SetLogicCollisionContactCacheEnabled(true, false);

  const JPH::SubShapeIDPair savedKey = MakeLogicContactKey(
      dynamicTarget.body_id, staticTarget.body_id, 9, 10);
  environment.SeedLogicContact(savedKey,
                               dynamicTarget.controller,
                               dynamicTarget.object,
                               staticTarget.controller,
                               staticTarget.object);
  environment.GetBodyInterface().DeactivateBody(dynamicTarget.body_id);
  ASSERT_FALSE(environment.GetBodyInterface().IsActive(dynamicTarget.body_id));

  std::vector<uint8_t> state;
  ASSERT_TRUE(environment.SavePhysicsState(state));

  const JPH::SubShapeIDPair laterKey = MakeLogicContactKey(
      dynamicTarget.body_id, staticTarget.body_id, 17, 18);
  environment.SeedLogicContact(laterKey,
                               dynamicTarget.controller,
                               dynamicTarget.object,
                               staticTarget.controller,
                               staticTarget.object);
  environment.QueueLogicContactUpdate(laterKey);
  ASSERT_EQ(environment.LogicContactCount(), 2u);

  ASSERT_TRUE(environment.RestorePhysicsState(state));
  EXPECT_FALSE(environment.GetBodyInterface().IsActive(dynamicTarget.body_id));
  EXPECT_TRUE(environment.HasLogicContact(savedKey));
  EXPECT_FALSE(environment.HasLogicContact(laterKey));
  EXPECT_EQ(environment.LogicContactCount(), 1u);

  /* The queued post-snapshot update must not resurrect the discarded timeline. */
  environment.CallbackTriggers();
  EXPECT_TRUE(environment.HasLogicContact(savedKey));
  EXPECT_FALSE(environment.HasLogicContact(laterKey));
  EXPECT_EQ(environment.LogicContactCount(), 1u);
}

TEST(JoltPhysicsEnvironment, RestorePhysicsStateAcceptsLegacyJoltBuffer)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));

  const JPH::RVec3 savedPosition = environment.GetBodyInterface().GetPosition(
      dynamicTarget.body_id);
  const std::vector<uint8_t> legacyState = environment.SaveLegacyPhysicsState();
  ASSERT_FALSE(legacyState.empty());

  environment.GetBodyInterface().SetPosition(
      dynamicTarget.body_id, JPH::RVec3(5.0f, 0.0f, 0.0f), JPH::EActivation::Activate);
  ASSERT_NE(environment.GetBodyInterface().GetPosition(dynamicTarget.body_id), savedPosition);

  ASSERT_TRUE(environment.RestorePhysicsState(legacyState));
  EXPECT_EQ(environment.GetBodyInterface().GetPosition(dynamicTarget.body_id), savedPosition);
}

TEST(JoltPhysicsEnvironment, RigidBodyConstraintUsesNativeJoltMappings)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  const JPH::BodyID dynamic_body = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic,
                                JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC)),
      JPH::EActivation::Activate);
  const JPH::BodyID static_body = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3(1.0f, 0.0f, 0.0f),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Static,
                                JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC)),
      JPH::EActivation::DontActivate);
  ASSERT_FALSE(dynamic_body.IsInvalid());
  ASSERT_FALSE(static_body.IsInvalid());

  JoltPhysicsController dynamic_controller;
  dynamic_controller.SetEnvironment(&environment);
  dynamic_controller.SetBodyID(dynamic_body);
  JoltPhysicsController static_controller;
  static_controller.SetEnvironment(&environment);
  static_controller.SetBodyID(static_body);

  KX_GameObject object1;
  KX_GameObject object2;
  SG_Callbacks callbacks;
  SG_Node node1(&object1, nullptr, callbacks);
  SG_Node node2(&object2, nullptr, callbacks);
  object1.SetSGNode(&node1);
  object2.SetSGNode(&node2);
  object1.SetPhysicsController(&dynamic_controller);
  object2.SetPhysicsController(&static_controller);

  PHY_RigidBodyConstraintSettings settings;
  settings.type = PHY_RigidBodyConstraintType::Fixed;
  int constraint_id = environment.CreateRigidBodyConstraint(
      &object1, &object2, MT_Vector3(0.0f, 0.0f, 0.0f), MT_Matrix3x3::Identity(), settings);
  ASSERT_NE(constraint_id, -1);
  JoltConstraint *wrapper = environment.FindConstraint(constraint_id);
  ASSERT_NE(wrapper, nullptr);
  EXPECT_EQ(wrapper->GetConstraint()->GetSubType(), JPH::EConstraintSubType::Fixed);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(wrapper->GetConstraint()->GetConstraintPriority(), 1u);
  EXPECT_TRUE(environment.RemoveConstraintById(constraint_id, true));

  settings.type = PHY_RigidBodyConstraintType::Slider;
  settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X;
  settings.limit_lin_x_lower = -1.0f;
  settings.limit_lin_x_upper = 2.0f;
  constraint_id = environment.CreateRigidBodyConstraint(
      &object1, &object2, MT_Vector3(0.0f, 0.0f, 0.0f), MT_Matrix3x3::Identity(), settings);
  ASSERT_NE(constraint_id, -1);
  wrapper = environment.FindConstraint(constraint_id);
  ASSERT_NE(wrapper, nullptr);
  ASSERT_EQ(wrapper->GetConstraint()->GetSubType(), JPH::EConstraintSubType::Slider);
  JPH::SliderConstraint *slider = static_cast<JPH::SliderConstraint *>(wrapper->GetConstraint());
  EXPECT_EQ(slider->GetBody1()->GetID(), static_body);
  EXPECT_FLOAT_EQ(slider->GetLimitsMin(), -1.0f);
  EXPECT_FLOAT_EQ(slider->GetLimitsMax(), 2.0f);
  EXPECT_TRUE(environment.RemoveConstraintById(constraint_id, true));

  settings.limit_lin_x_lower = 1.0f;
  settings.limit_lin_x_upper = 2.0f;
  constraint_id = environment.CreateRigidBodyConstraint(
      &object1, &object2, MT_Vector3(0.0f, 0.0f, 0.0f), MT_Matrix3x3::Identity(), settings);
  ASSERT_NE(constraint_id, -1);
  wrapper = environment.FindConstraint(constraint_id);
  ASSERT_NE(wrapper, nullptr);
  ASSERT_EQ(wrapper->GetConstraint()->GetSubType(), JPH::EConstraintSubType::SixDOF);
  JPH::SixDOFConstraint *six_dof = static_cast<JPH::SixDOFConstraint *>(wrapper->GetConstraint());
  EXPECT_FLOAT_EQ(six_dof->GetLimitsMin(JPH::SixDOFConstraint::EAxis::TranslationX), 1.0f);
  EXPECT_FLOAT_EQ(six_dof->GetLimitsMax(JPH::SixDOFConstraint::EAxis::TranslationX), 2.0f);
  EXPECT_TRUE(environment.RemoveConstraintById(constraint_id, true));

  settings = PHY_RigidBodyConstraintSettings{};
  settings.type = PHY_RigidBodyConstraintType::GenericSpring;
  settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_X;
  settings.spring_stiffness_x = 25.0f;
  settings.spring_damping_x = 3.0f;
  constraint_id = environment.CreateRigidBodyConstraint(
      &object1, &object2, MT_Vector3(0.0f, 0.0f, 0.0f), MT_Matrix3x3::Identity(), settings);
  ASSERT_NE(constraint_id, -1);
  wrapper = environment.FindConstraint(constraint_id);
  ASSERT_NE(wrapper, nullptr);
  ASSERT_EQ(wrapper->GetConstraint()->GetSubType(), JPH::EConstraintSubType::SixDOF);
  six_dof = static_cast<JPH::SixDOFConstraint *>(wrapper->GetConstraint());
  const JPH::MotorSettings &motor = six_dof->GetMotorSettings(
      JPH::SixDOFConstraint::EAxis::TranslationX);
  EXPECT_EQ(motor.mSpringSettings.mMode, JPH::ESpringMode::StiffnessAndDamping);
  EXPECT_FLOAT_EQ(motor.mSpringSettings.mStiffness, 25.0f);
  EXPECT_FLOAT_EQ(motor.mSpringSettings.mDamping, 3.0f);
  EXPECT_EQ(six_dof->GetMotorState(JPH::SixDOFConstraint::EAxis::TranslationX),
            JPH::EMotorState::Position);
  EXPECT_TRUE(environment.RemoveConstraintById(constraint_id, true));

  object1.SetPhysicsController(nullptr);
  object2.SetPhysicsController(nullptr);
  object1.SetSGNode(nullptr);
  object2.SetSGNode(nullptr);
}

TEST(JoltPhysicsEnvironment, RayAndShapeCastReturnMeshFaceAndUVThroughMirroredScale)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);

  JPH::VertexList vertices = {
      JPH::Float3(0.0f, 0.0f, 0.0f),
      JPH::Float3(0.0f, 0.0f, 1.0f),
      JPH::Float3(1.0f, 0.0f, 0.0f),
  };
  JPH::IndexedTriangleList triangles = {JPH::IndexedTriangle(0, 1, 2, 0, 0)};
  JPH::MeshShapeSettings meshSettings(std::move(vertices), std::move(triangles));
  meshSettings.mPerTriangleUserData = true;
  const JPH::Shape::ShapeResult meshResult = meshSettings.Create();
  ASSERT_FALSE(meshResult.HasError());
  const JPH::RefConst<JPH::Shape> meshShape = meshResult.Get();
  const JPH::RefConst<JPH::Shape> scaledShape = new JPH::ScaledShape(meshShape.GetPtr(),
                                                                     JPH::Vec3(-1.0f, 1.0f, 1.0f));
  JPH::StaticCompoundShapeSettings compoundSettings;
  compoundSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), scaledShape);
  const JPH::Shape::ShapeResult compoundResult = compoundSettings.Create();
  ASSERT_FALSE(compoundResult.HasError());
  const JPH::RefConst<JPH::Shape> compoundShape = compoundResult.Get();

  std::shared_ptr<JoltMeshQueryData> meshData = std::make_shared<JoltMeshQueryData>();
  meshData->polygonIndices = {42};
  JoltTriangleUV triangleUV;
  triangleUV.corners = {MT_Vector2(0.0f, 0.0f), MT_Vector2(0.0f, 1.0f), MT_Vector2(1.0f, 0.0f)};
  meshData->triangleUVs = {triangleUV};
  std::shared_ptr<JoltShapeQueryData> shapeQueryData = std::make_shared<JoltShapeQueryData>();
  shapeQueryData->Add(meshShape.GetPtr(), meshData);

  const JPH::BodyID bodyID = environment.GetBodyInterface().CreateAndAddBody(
      JPH::BodyCreationSettings(compoundShape,
                                JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Static,
                                JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC)),
      JPH::EActivation::DontActivate);
  ASSERT_FALSE(bodyID.IsInvalid());

  KX_GameObject object;
  KX_ClientObjectInfo clientInfo(&object, KX_ClientObjectInfo::STATIC);
  JoltPhysicsController controller;
  controller.SetEnvironment(&environment);
  controller.SetBodyID(bodyID);
  controller.SetNewClientInfo(&clientInfo);
  controller.SetShape(compoundShape, shapeQueryData);
  object.SetPhysicsController(&controller);
  environment.GetBodyInterface().SetUserData(bodyID, reinterpret_cast<JPH::uint64>(&clientInfo));

  PHY_RayQuerySettings settings;
  settings.origin = MT_Vector3(-0.25f, -0.25f, 1.0f);
  settings.destination = MT_Vector3(-0.25f, -0.25f, -1.0f);
  settings.detail_flags = PHY_RAY_QUERY_DETAIL_FACE_INDEX | PHY_RAY_QUERY_DETAIL_UV;
  settings.hit_back_faces = true;
  AcceptAllRayCastFilter filter;
  std::vector<PHY_RayCastResult> results;
  ASSERT_TRUE(environment.RayCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].m_polygon, 42);
  EXPECT_EQ(results[0].m_hitUVOK, 1);
  EXPECT_NEAR(results[0].m_hitUV[0], 0.25f, 1.0e-5f);
  EXPECT_NEAR(results[0].m_hitUV[1], 0.25f, 1.0e-5f);

  PHY_ShapeCastSettings shapeSettings;
  shapeSettings.origin = settings.origin;
  shapeSettings.destination = settings.destination;
  shapeSettings.radius = 0.1f;
  shapeSettings.detail_flags = PHY_RAY_QUERY_DETAIL_FACE_INDEX | PHY_RAY_QUERY_DETAIL_UV;
  shapeSettings.hit_back_faces = true;
  AcceptAllShapeCastFilter shapeFilter;
  std::vector<PHY_ShapeCastResult> shapeResults;
  ASSERT_TRUE(environment.ShapeCast(shapeSettings, shapeFilter, shapeResults));
  ASSERT_EQ(shapeResults.size(), 1u);
  EXPECT_EQ(shapeResults[0].polygon_index, 42);
  EXPECT_TRUE(shapeResults[0].has_uv);
  EXPECT_NEAR(shapeResults[0].hit_uv[0], 0.25f, 1.0e-5f);
  EXPECT_NEAR(shapeResults[0].hit_uv[1], 0.25f, 1.0e-5f);

  settings.detail_flags = PHY_RAY_QUERY_DETAIL_UV;
  ASSERT_TRUE(environment.RayCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].m_polygon, 42);
  EXPECT_EQ(results[0].m_hitUVOK, 1);

  meshData->triangleUVs.clear();
  ASSERT_TRUE(environment.RayCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].m_polygon, 42);
  EXPECT_EQ(results[0].m_hitUVOK, 0);

  settings.detail_flags = PHY_RAY_QUERY_DETAIL_NONE;
  ASSERT_TRUE(environment.RayCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].m_polygon, -1);
  EXPECT_EQ(results[0].m_hitUVOK, 0);

  object.SetPhysicsController(nullptr);
  controller.SetNewClientInfo(nullptr);
}

TEST(JoltPhysicsEnvironment, RemoveConstraintUpdatesIslandAndOwnerRegistry)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  const JPH::ObjectLayer layer = JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC);
  const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  const JPH::BodyID body1 = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(
          shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, layer),
      JPH::EActivation::Activate);
  const JPH::BodyID body2 = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3(0.0f, 1.0f, 0.0f),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic,
                                layer),
      JPH::EActivation::Activate);
  ASSERT_FALSE(body1.IsInvalid());
  ASSERT_FALSE(body2.IsInvalid());

  JoltPhysicsController controller1;
  controller1.SetEnvironment(&environment);
  controller1.SetBodyID(body1);
  JoltPhysicsController controller2;
  controller2.SetEnvironment(&environment);
  controller2.SetBodyID(body2);

  PHY_IConstraint *low_constraint = CreateFixedConstraint(
      environment, controller1, controller2, 1);
  ASSERT_NE(low_constraint, nullptr);
  const int low_constraint_id = low_constraint->GetIdentifier();

  body_interface.AddImpulse(body2, JPH::Vec3(1.0f, 0.0f, 0.0f));
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0f / 60.0f, 1.0f / 60.0f));
  ASSERT_GT(static_cast<JoltConstraint *>(low_constraint)->GetAppliedImpulse(), 0.0f);

  PHY_IConstraint *high_constraint = CreateFixedConstraint(
      environment, controller1, controller2, 255);
  ASSERT_NE(high_constraint, nullptr);
  const int high_constraint_id = high_constraint->GetIdentifier();
  ASSERT_EQ(environment.GetPhysicsSystem()->GetConstraints().size(), 2u);

  KX_GameObject owner;
  SG_Callbacks callbacks;
  SG_Node owner_node(&owner, nullptr, callbacks);
  owner.SetSGNode(&owner_node);
  static_cast<JoltConstraint *>(low_constraint)->SetRigidBodyConstraintOwner(&owner);
  static_cast<JoltConstraint *>(high_constraint)->SetRigidBodyConstraintOwner(&owner);
  owner.AddRuntimeRigidBodyConstraint("low_constraint",
                                      &owner,
                                      nullptr,
                                      PHY_RigidBodyConstraintSettings{},
                                      MT_Vector3(0.0f, 0.0f, 0.0f),
                                      MT_Matrix3x3::Identity(),
                                      low_constraint_id);
  owner.AddRuntimeRigidBodyConstraint("high_constraint",
                                      &owner,
                                      nullptr,
                                      PHY_RigidBodyConstraintSettings{},
                                      MT_Vector3(0.0f, 0.0f, 0.0f),
                                      MT_Matrix3x3::Identity(),
                                      high_constraint_id);
  ASSERT_TRUE(owner.HasRigidBodyConstraints());

  EXPECT_TRUE(environment.RemoveConstraintById(high_constraint_id, true));
  environment.ProcessPendingConstraintTopologyChanges();
  const JPH::Constraints remaining_constraints = environment.GetPhysicsSystem()->GetConstraints();
  ASSERT_EQ(remaining_constraints.size(), 1u);
  EXPECT_EQ(remaining_constraints[0],
            static_cast<JoltConstraint *>(low_constraint)->GetConstraint());
  EXPECT_EQ(remaining_constraints[0]->GetNumPositionStepsOverride(), 1u);
  EXPECT_EQ(remaining_constraints[0]->GetNumVelocityStepsOverride(), 1u);
  EXPECT_FLOAT_EQ(static_cast<JoltConstraint *>(low_constraint)->GetAppliedImpulse(), 0.0f);
  EXPECT_NE(owner.FindRigidBodyConstraint("low_constraint"), nullptr);
  EXPECT_EQ(owner.FindRigidBodyConstraint("high_constraint"), nullptr);
  EXPECT_FALSE(environment.RemoveConstraintById(high_constraint_id, true));

  PHY_IConstraint *wake_constraint = CreateFixedConstraint(
      environment, controller1, controller2, 1);
  ASSERT_NE(wake_constraint, nullptr);
  const int wake_constraint_id = wake_constraint->GetIdentifier();
  environment.ProcessPendingConstraintTopologyChanges();
  body_interface.DeactivateBody(body1);
  body_interface.DeactivateBody(body2);
  ASSERT_FALSE(body_interface.IsActive(body1));
  ASSERT_FALSE(body_interface.IsActive(body2));
  EXPECT_TRUE(environment.RemoveConstraintById(wake_constraint_id, true));
  EXPECT_TRUE(body_interface.IsActive(body1));
  EXPECT_TRUE(body_interface.IsActive(body2));

  EXPECT_TRUE(environment.RemoveConstraintById(low_constraint_id, true));
  EXPECT_TRUE(environment.GetPhysicsSystem()->GetConstraints().empty());
  EXPECT_FALSE(owner.HasRigidBodyConstraints());
  owner.SetSGNode(nullptr);
}

TEST(JoltPhysicsEnvironment, BulkRemoveConstraintsPreservesJoltAndOwnerSideEffects)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  SG_Callbacks callbacks;
  std::unique_ptr<SG_Node> ownerNode;
  std::unique_ptr<SG_Node> target1Node;
  std::unique_ptr<SG_Node> target2Node;
  LogicContactTarget owner(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  LogicContactTarget target1(environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.0f, 2.0f, 0.0f));
  LogicContactTarget target2(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.0f, -2.0f, 0.0f));
  ownerNode = std::make_unique<SG_Node>(&owner.object, nullptr, callbacks);
  target1Node = std::make_unique<SG_Node>(&target1.object, nullptr, callbacks);
  target2Node = std::make_unique<SG_Node>(&target2.object, nullptr, callbacks);
  owner.object.SetSGNode(ownerNode.get());
  target1.object.SetSGNode(target1Node.get());
  target2.object.SetSGNode(target2Node.get());
  owner.object.SetName("bulk_owner");
  target1.object.SetName("bulk_target_1");
  target2.object.SetName("bulk_target_2");

  PHY_RigidBodyConstraintSettings settings;
  settings.type = PHY_RigidBodyConstraintType::Fixed;
  const int constraint1 = environment.CreateRigidBodyConstraint(&owner.object,
                                                                &target1.object,
                                                                MT_Vector3(0.0f, 0.0f, 0.0f),
                                                                MT_Matrix3x3::Identity(),
                                                                settings,
                                                                "bulk_constraint_1");
  const int constraint2 = environment.CreateRigidBodyConstraint(&owner.object,
                                                                &target2.object,
                                                                MT_Vector3(0.0f, 0.0f, 0.0f),
                                                                MT_Matrix3x3::Identity(),
                                                                settings,
                                                                "bulk_constraint_2");
  const int constraint3 = environment.CreateRigidBodyConstraint(&target1.object,
                                                                &target2.object,
                                                                MT_Vector3(0.0f, 0.0f, 0.0f),
                                                                MT_Matrix3x3::Identity(),
                                                                settings,
                                                                "bulk_constraint_3");
  ASSERT_NE(constraint1, -1);
  ASSERT_NE(constraint2, -1);
  ASSERT_NE(constraint3, -1);
  ASSERT_EQ(environment.GetPhysicsSystem()->GetConstraints().size(), 3u);

  owner.object.AddRuntimeRigidBodyConstraint("bulk_constraint_1",
                                             &owner.object,
                                             &target1.object,
                                             settings,
                                             MT_Vector3(0.0f, 0.0f, 0.0f),
                                             MT_Matrix3x3::Identity(),
                                             constraint1);
  owner.object.AddRuntimeRigidBodyConstraint("bulk_constraint_2",
                                             &owner.object,
                                             &target2.object,
                                             settings,
                                             MT_Vector3(0.0f, 0.0f, 0.0f),
                                             MT_Matrix3x3::Identity(),
                                             constraint2);
  target1.object.AddRuntimeRigidBodyConstraint("bulk_constraint_3",
                                               &target1.object,
                                               &target2.object,
                                               settings,
                                               MT_Vector3(0.0f, 0.0f, 0.0f),
                                               MT_Matrix3x3::Identity(),
                                               constraint3);

  JoltConstraintGroupFilter *groupFilter = environment.GetConstraintGroupFilter();
  const JPH::CollisionGroup ownerGroup(groupFilter, 0, owner.body_id.GetIndexAndSequenceNumber());
  const JPH::CollisionGroup target1Group(
      groupFilter, 0, target1.body_id.GetIndexAndSequenceNumber());
  const JPH::CollisionGroup target2Group(
      groupFilter, 0, target2.body_id.GetIndexAndSequenceNumber());
  EXPECT_FALSE(ownerGroup.CanCollide(target1Group));
  EXPECT_FALSE(ownerGroup.CanCollide(target2Group));
  EXPECT_FALSE(target1Group.CanCollide(target2Group));

  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  bodyInterface.DeactivateBody(owner.body_id);
  bodyInterface.DeactivateBody(target1.body_id);
  bodyInterface.DeactivateBody(target2.body_id);

  const std::vector<int> requested = {constraint2, -12345, constraint1, constraint2, constraint3};
  std::vector<int> removed;
  environment.RemoveConstraintsByIds(requested, true, removed);

  EXPECT_EQ(removed, (std::vector<int>{constraint2, constraint1, constraint3}));
  EXPECT_TRUE(environment.GetPhysicsSystem()->GetConstraints().empty());
  EXPECT_EQ(environment.FindConstraint(constraint1), nullptr);
  EXPECT_EQ(environment.FindConstraint(constraint2), nullptr);
  EXPECT_EQ(environment.FindConstraint(constraint3), nullptr);
  EXPECT_FALSE(owner.object.HasRigidBodyConstraints());
  EXPECT_FALSE(target1.object.HasRigidBodyConstraints());
  EXPECT_TRUE(ownerGroup.CanCollide(target1Group));
  EXPECT_TRUE(ownerGroup.CanCollide(target2Group));
  EXPECT_TRUE(target1Group.CanCollide(target2Group));
  EXPECT_TRUE(bodyInterface.IsActive(owner.body_id));
  EXPECT_TRUE(bodyInterface.IsActive(target1.body_id));
  EXPECT_TRUE(bodyInterface.IsActive(target2.body_id));

  removed.push_back(42);
  environment.RemoveConstraintsByIds(requested, true, removed);
  EXPECT_TRUE(removed.empty());

  owner.object.SetSGNode(nullptr);
  target1.object.SetSGNode(nullptr);
  target2.object.SetSGNode(nullptr);
}

TEST(JoltPhysicsEnvironment, AutomaticConstraintBreakPublishesStableEventSnapshot)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget owner_target(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  LogicContactTarget target_target(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.0f, 1.0f, 0.0f));

  owner_target.object.SetName("constraint_owner");
  target_target.object.SetName("constraint_target");
  SG_Callbacks callbacks;
  SG_Node owner_node(&owner_target.object, nullptr, callbacks);
  SG_Node target_node(&target_target.object, nullptr, callbacks);
  owner_target.object.SetSGNode(&owner_node);
  target_target.object.SetSGNode(&target_node);

  PHY_RigidBodyConstraintSettings settings;
  settings.type = PHY_RigidBodyConstraintType::Fixed;
  settings.flags |= PHY_RB_CONSTRAINT_USE_BREAKING;
  settings.breaking_threshold = 1.0e-6f;
  const int constraint_id = environment.CreateRigidBodyConstraint(&owner_target.object,
                                                                  &target_target.object,
                                                                  MT_Vector3(0.0f, 0.0f, 0.0f),
                                                                  MT_Matrix3x3::Identity(),
                                                                  settings,
                                                                  "breakable_joint");
  ASSERT_NE(constraint_id, -1);

  environment.GetBodyInterface().AddImpulse(owner_target.body_id, JPH::Vec3(5.0f, 0.0f, 0.0f));
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0f / 60.0f, 1.0f / 60.0f));
  EXPECT_EQ(environment.FindConstraint(constraint_id), nullptr);

  std::vector<PHY_RigidBodyConstraintBreakEvent> events;
  environment.ConsumeRigidBodyConstraintBreakEvents(events);
  ASSERT_EQ(events.size(), 1u);
  const PHY_RigidBodyConstraintBreakEvent &event = events.front();
  EXPECT_EQ(event.owner_name, "constraint_owner");
  EXPECT_EQ(event.target_name, "constraint_target");
  EXPECT_EQ(event.constraint_name, "breakable_joint");
  EXPECT_TRUE(std::isfinite(event.anchor_position.x()));
  EXPECT_TRUE(std::isfinite(event.anchor_position.y()));
  EXPECT_TRUE(std::isfinite(event.anchor_position.z()));
  EXPECT_GT(event.applied_impulse, 0.0f);
  EXPECT_GE(event.linear_impulse, 0.0f);
  EXPECT_GE(event.angular_impulse, 0.0f);
  EXPECT_FLOAT_EQ(event.applied_impulse, event.linear_impulse + event.angular_impulse);

  environment.ConsumeRigidBodyConstraintBreakEvents(events);
  EXPECT_EQ(events.size(), 1u);

  owner_target.object.SetSGNode(nullptr);
  target_target.object.SetSGNode(nullptr);
}

TEST(JoltPhysicsEnvironment, ConstraintBreakAnchorUsesTheAuthoredOwnerFrameAfterSliderSwap)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget owner_target(environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  LogicContactTarget target_target(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.0f, 1.0f, 0.0f));

  owner_target.object.SetName("slider_owner");
  target_target.object.SetName("slider_target");
  SG_Callbacks callbacks;
  SG_Node owner_node(&owner_target.object, nullptr, callbacks);
  SG_Node target_node(&target_target.object, nullptr, callbacks);
  owner_target.object.SetSGNode(&owner_node);
  target_target.object.SetSGNode(&target_node);

  PHY_RigidBodyConstraintSettings settings;
  settings.type = PHY_RigidBodyConstraintType::Slider;
  const int constraint_id = environment.CreateRigidBodyConstraint(&owner_target.object,
                                                                  &target_target.object,
                                                                  MT_Vector3(0.2f, 0.3f, 0.4f),
                                                                  MT_Matrix3x3::Identity(),
                                                                  settings,
                                                                  "slider_joint");
  ASSERT_NE(constraint_id, -1);

  JoltConstraint *wrapper = environment.FindConstraint(constraint_id);
  ASSERT_NE(wrapper, nullptr);
  const JPH::TwoBodyConstraint *constraint = static_cast<const JPH::TwoBodyConstraint *>(
      wrapper->GetConstraint());
  ASSERT_NE(constraint, nullptr);
  ASSERT_EQ(constraint->GetBody2()->GetID(), owner_target.body_id);

  PHY_RigidBodyConstraintBreakEvent event;
  ASSERT_TRUE(wrapper->BuildRigidBodyConstraintBreakEvent(event));
  const MT_Vector3 expected_anchor = JoltMath::ToMT(
      (constraint->GetBody2()->GetCenterOfMassTransform() *
       constraint->GetConstraintToBody2Matrix())
          .GetTranslation());
  EXPECT_NEAR(event.anchor_position.x(), expected_anchor.x(), 1.0e-5f);
  EXPECT_NEAR(event.anchor_position.y(), expected_anchor.y(), 1.0e-5f);
  EXPECT_NEAR(event.anchor_position.z(), expected_anchor.z(), 1.0e-5f);

  EXPECT_TRUE(environment.RemoveConstraintById(constraint_id, true));
  owner_target.object.SetSGNode(nullptr);
  target_target.object.SetSGNode(nullptr);
}

TEST(JoltPhysicsEnvironment, ConstraintTopologyResetDoesNotCrossStaticAnchors)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  const JPH::BodyID anchor = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Static,
                                JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC)),
      JPH::EActivation::DontActivate);
  const JPH::ObjectLayer dynamic_layer = JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC);
  const JPH::BodyID body1 = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3(-1.0f, 0.0f, 0.0f),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic,
                                dynamic_layer),
      JPH::EActivation::Activate);
  const JPH::BodyID body2 = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3(1.0f, 0.0f, 0.0f),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic,
                                dynamic_layer),
      JPH::EActivation::Activate);
  ASSERT_FALSE(anchor.IsInvalid());
  ASSERT_FALSE(body1.IsInvalid());
  ASSERT_FALSE(body2.IsInvalid());

  JoltPhysicsController anchor_controller;
  anchor_controller.SetEnvironment(&environment);
  anchor_controller.SetBodyID(anchor);
  JoltPhysicsController controller1;
  controller1.SetEnvironment(&environment);
  controller1.SetBodyID(body1);
  JoltPhysicsController controller2;
  controller2.SetEnvironment(&environment);
  controller2.SetBodyID(body2);

  PHY_IConstraint *constraint1 = CreateFixedConstraint(
      environment, anchor_controller, controller1, 1);
  PHY_IConstraint *constraint2 = CreateFixedConstraint(
      environment, anchor_controller, controller2, 1);
  ASSERT_NE(constraint1, nullptr);
  ASSERT_NE(constraint2, nullptr);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint1)->GetConstraint()->GetConstraintPriority(),
            1u);
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint2)->GetConstraint()->GetConstraintPriority(),
            1u);

  body_interface.AddImpulse(body1, JPH::Vec3(1.0f, 0.0f, 0.0f));
  body_interface.AddImpulse(body2, JPH::Vec3(-1.0f, 0.0f, 0.0f));
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0f / 60.0f, 1.0f / 60.0f));
  ASSERT_GT(static_cast<JoltConstraint *>(constraint1)->GetAppliedImpulse(), 0.0f);
  ASSERT_GT(static_cast<JoltConstraint *>(constraint2)->GetAppliedImpulse(), 0.0f);

  PHY_IConstraint *temporary = CreateFixedConstraint(
      environment, anchor_controller, controller1, 255);
  ASSERT_NE(temporary, nullptr);
  const int temporary_id = temporary->GetIdentifier();

  ASSERT_TRUE(environment.RemoveConstraintById(temporary_id, true));
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_FLOAT_EQ(static_cast<JoltConstraint *>(constraint1)->GetAppliedImpulse(), 0.0f);
  EXPECT_GT(static_cast<JoltConstraint *>(constraint2)->GetAppliedImpulse(), 0.0f);
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint2)->GetConstraint()->GetConstraintPriority(),
            1u);

  EXPECT_TRUE(environment.RemoveConstraintById(constraint1->GetIdentifier(), true));
  EXPECT_TRUE(environment.RemoveConstraintById(constraint2->GetIdentifier(), true));
}

TEST(JoltPhysicsEnvironment, ConstraintTopologyResetPropagatesAcrossDynamicChain)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  const JPH::ObjectLayer layer = JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC);
  JPH::BodyID body_ids[3];
  JoltPhysicsController controllers[3];
  for (int i = 0; i < 3; ++i) {
    body_ids[i] = body_interface.CreateAndAddBody(
        JPH::BodyCreationSettings(shape,
                                  JPH::RVec3(0.0f, float(i), 0.0f),
                                  JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Dynamic,
                                  layer),
        JPH::EActivation::Activate);
    ASSERT_FALSE(body_ids[i].IsInvalid());
    controllers[i].SetEnvironment(&environment);
    controllers[i].SetBodyID(body_ids[i]);
  }

  PHY_IConstraint *constraint1 = CreateFixedConstraint(
      environment, controllers[0], controllers[1], 1);
  PHY_IConstraint *constraint2 = CreateFixedConstraint(
      environment, controllers[1], controllers[2], 1);
  ASSERT_NE(constraint1, nullptr);
  ASSERT_NE(constraint2, nullptr);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint1)->GetConstraint()->GetConstraintPriority(),
            0u);
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint2)->GetConstraint()->GetConstraintPriority(),
            0u);

  body_interface.SetMotionType(
      body_ids[0], JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
  environment.NotifyConstraintBodyMotionTypeChanged(body_ids[0]);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint1)->GetConstraint()->GetConstraintPriority(),
            2u);
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint2)->GetConstraint()->GetConstraintPriority(),
            1u);

  body_interface.SetMotionType(body_ids[0], JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
  environment.NotifyConstraintBodyMotionTypeChanged(body_ids[0]);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint1)->GetConstraint()->GetConstraintPriority(),
            0u);
  EXPECT_EQ(static_cast<JoltConstraint *>(constraint2)->GetConstraint()->GetConstraintPriority(),
            0u);

  body_interface.AddImpulse(body_ids[2], JPH::Vec3(1.0f, 0.0f, 0.0f));
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0f / 60.0f, 1.0f / 60.0f));
  ASSERT_GT(static_cast<JoltConstraint *>(constraint1)->GetAppliedImpulse(), 0.0f);
  ASSERT_GT(static_cast<JoltConstraint *>(constraint2)->GetAppliedImpulse(), 0.0f);

  PHY_IConstraint *temporary = CreateFixedConstraint(
      environment, controllers[0], controllers[1], 255);
  ASSERT_NE(temporary, nullptr);
  ASSERT_TRUE(environment.RemoveConstraintById(temporary->GetIdentifier(), true));
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_FLOAT_EQ(static_cast<JoltConstraint *>(constraint1)->GetAppliedImpulse(), 0.0f);
  EXPECT_FLOAT_EQ(static_cast<JoltConstraint *>(constraint2)->GetAppliedImpulse(), 0.0f);

  EXPECT_TRUE(environment.RemoveConstraintById(constraint1->GetIdentifier(), true));
  EXPECT_TRUE(environment.RemoveConstraintById(constraint2->GetIdentifier(), true));
}

TEST(JoltPhysicsEnvironment, ConstraintPrioritiesFollowAnchoredTreeTopology)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  const JPH::BodyID anchor = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Static,
                                JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC)),
      JPH::EActivation::DontActivate);
  ASSERT_FALSE(anchor.IsInvalid());

  JoltPhysicsController anchor_controller;
  anchor_controller.SetEnvironment(&environment);
  anchor_controller.SetBodyID(anchor);
  const JPH::ObjectLayer layer = JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC);
  JPH::BodyID body_ids[3];
  JoltPhysicsController controllers[3];
  for (int i = 0; i < 3; ++i) {
    body_ids[i] = body_interface.CreateAndAddBody(
        JPH::BodyCreationSettings(shape,
                                  JPH::RVec3(float(i + 1), 0.0f, 0.0f),
                                  JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Dynamic,
                                  layer),
        JPH::EActivation::Activate);
    ASSERT_FALSE(body_ids[i].IsInvalid());
    controllers[i].SetEnvironment(&environment);
    controllers[i].SetBodyID(body_ids[i]);
  }

  PHY_IConstraint *leaf = CreateFixedConstraint(environment, controllers[1], controllers[2], 2);
  PHY_IConstraint *middle = CreateFixedConstraint(environment, controllers[0], controllers[1], 2);
  PHY_IConstraint *root = CreateFixedConstraint(environment, anchor_controller, controllers[0], 2);
  ASSERT_NE(leaf, nullptr);
  ASSERT_NE(middle, nullptr);
  ASSERT_NE(root, nullptr);
  environment.ProcessPendingConstraintTopologyChanges();

  EXPECT_EQ(static_cast<JoltConstraint *>(root)->GetConstraint()->GetConstraintPriority(), 3u);
  EXPECT_EQ(static_cast<JoltConstraint *>(middle)->GetConstraint()->GetConstraintPriority(), 2u);
  EXPECT_EQ(static_cast<JoltConstraint *>(leaf)->GetConstraint()->GetConstraintPriority(), 1u);

  static_cast<JoltConstraint *>(middle)->SetEnabled(false);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(static_cast<JoltConstraint *>(root)->GetConstraint()->GetConstraintPriority(), 1u);
  EXPECT_EQ(static_cast<JoltConstraint *>(middle)->GetConstraint()->GetConstraintPriority(), 0u);
  EXPECT_EQ(static_cast<JoltConstraint *>(leaf)->GetConstraint()->GetConstraintPriority(), 0u);

  static_cast<JoltConstraint *>(middle)->SetEnabled(true);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(static_cast<JoltConstraint *>(root)->GetConstraint()->GetConstraintPriority(), 3u);
  EXPECT_EQ(static_cast<JoltConstraint *>(middle)->GetConstraint()->GetConstraintPriority(), 2u);
  EXPECT_EQ(static_cast<JoltConstraint *>(leaf)->GetConstraint()->GetConstraintPriority(), 1u);

  EXPECT_TRUE(environment.RemoveConstraintById(middle->GetIdentifier(), true));
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(static_cast<JoltConstraint *>(root)->GetConstraint()->GetConstraintPriority(), 1u);
  EXPECT_EQ(static_cast<JoltConstraint *>(leaf)->GetConstraint()->GetConstraintPriority(), 0u);

  EXPECT_TRUE(environment.RemoveConstraintById(root->GetIdentifier(), true));
  EXPECT_TRUE(environment.RemoveConstraintById(leaf->GetIdentifier(), true));
}

TEST(JoltPhysicsEnvironment, ConstraintPrioritiesStayNeutralForAmbiguousTopology)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  JPH::BodyInterface &body_interface = environment.GetBodyInterface();

  const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  const JPH::BodyID anchors[2] = {
      body_interface.CreateAndAddBody(
          JPH::BodyCreationSettings(shape,
                                    JPH::RVec3(-1.0f, 0.0f, 0.0f),
                                    JPH::Quat::sIdentity(),
                                    JPH::EMotionType::Static,
                                    JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC)),
          JPH::EActivation::DontActivate),
      body_interface.CreateAndAddBody(
          JPH::BodyCreationSettings(shape,
                                    JPH::RVec3(1.0f, 0.0f, 0.0f),
                                    JPH::Quat::sIdentity(),
                                    JPH::EMotionType::Static,
                                    JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC)),
          JPH::EActivation::DontActivate)};
  const JPH::BodyID body = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic,
                                JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC)),
      JPH::EActivation::Activate);
  ASSERT_FALSE(anchors[0].IsInvalid());
  ASSERT_FALSE(anchors[1].IsInvalid());
  ASSERT_FALSE(body.IsInvalid());

  JoltPhysicsController anchor_controllers[2];
  for (int i = 0; i < 2; ++i) {
    anchor_controllers[i].SetEnvironment(&environment);
    anchor_controllers[i].SetBodyID(anchors[i]);
  }
  JoltPhysicsController controller;
  controller.SetEnvironment(&environment);
  controller.SetBodyID(body);

  PHY_IConstraint *constraints[2] = {
      CreateFixedConstraint(environment, anchor_controllers[0], controller, 2),
      CreateFixedConstraint(environment, anchor_controllers[1], controller, 2)};
  ASSERT_NE(constraints[0], nullptr);
  ASSERT_NE(constraints[1], nullptr);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(
      static_cast<JoltConstraint *>(constraints[0])->GetConstraint()->GetConstraintPriority(), 0u);
  EXPECT_EQ(
      static_cast<JoltConstraint *>(constraints[1])->GetConstraint()->GetConstraintPriority(), 0u);

  EXPECT_TRUE(environment.RemoveConstraintById(constraints[0]->GetIdentifier(), true));
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(
      static_cast<JoltConstraint *>(constraints[1])->GetConstraint()->GetConstraintPriority(), 1u);

  const JPH::BodyID cycle_body = body_interface.CreateAndAddBody(
      JPH::BodyCreationSettings(shape,
                                JPH::RVec3(0.0f, 1.0f, 0.0f),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic,
                                JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC)),
      JPH::EActivation::Activate);
  ASSERT_FALSE(cycle_body.IsInvalid());
  JoltPhysicsController cycle_controller;
  cycle_controller.SetEnvironment(&environment);
  cycle_controller.SetBodyID(cycle_body);
  PHY_IConstraint *cycle_constraints[2] = {
      CreateFixedConstraint(environment, controller, cycle_controller, 2),
      CreateFixedConstraint(environment, cycle_controller, anchor_controllers[1], 2)};
  ASSERT_NE(cycle_constraints[0], nullptr);
  ASSERT_NE(cycle_constraints[1], nullptr);
  environment.ProcessPendingConstraintTopologyChanges();
  EXPECT_EQ(
      static_cast<JoltConstraint *>(constraints[1])->GetConstraint()->GetConstraintPriority(), 0u);
  EXPECT_EQ(static_cast<JoltConstraint *>(cycle_constraints[0])
                ->GetConstraint()
                ->GetConstraintPriority(),
            0u);
  EXPECT_EQ(static_cast<JoltConstraint *>(cycle_constraints[1])
                ->GetConstraint()
                ->GetConstraintPriority(),
            0u);

  EXPECT_TRUE(environment.RemoveConstraintById(cycle_constraints[0]->GetIdentifier(), true));
  EXPECT_TRUE(environment.RemoveConstraintById(cycle_constraints[1]->GetIdentifier(), true));
  EXPECT_TRUE(environment.RemoveConstraintById(constraints[1]->GetIdentifier(), true));
}

TEST(JoltPhysicsEnvironment, RayCastIsBoundedSortedAndUniquePerBody)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  ShapeCastTarget target1(environment, JPH::RVec3(2.0f, 0.0f, 0.0f));
  ShapeCastTarget target2(environment, JPH::RVec3(4.0f, 0.0f, 0.0f));
  ShapeCastTarget target3(environment, JPH::RVec3(6.0f, 0.0f, 0.0f));
  AcceptAllRayCastFilter filter;

  PHY_RayQuerySettings settings;
  settings.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
  settings.destination = MT_Vector3(10.0f, 0.0f, 0.0f);
  settings.max_results = 2;
  std::vector<PHY_RayCastResult> results;
  ASSERT_TRUE(environment.RayCast(settings, filter, results));
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].m_controller, &target1.controller);
  EXPECT_EQ(results[1].m_controller, &target2.controller);
  EXPECT_NEAR(results[0].m_fraction, 0.15f, 1.0e-4f);
  EXPECT_NEAR(results[1].m_fraction, 0.35f, 1.0e-4f);
  EXPECT_NEAR(results[0].m_hitPoint.x(), 1.5f, 1.0e-4f);
  EXPECT_NEAR(results[0].m_hitNormal.x(), -1.0f, 1.0e-4f);
}

TEST(JoltPhysicsEnvironment, RayCastBatchAdaptiveMatchesSequentialAndPreservesEverySlot)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  ShapeCastTarget sensor(environment, JPH::RVec3(1.0f, 0.0f, 0.0f), true);
  ShapeCastTarget target1(environment, JPH::RVec3(2.0f, 0.0f, 0.0f));
  ShapeCastTarget target2(environment, JPH::RVec3(4.0f, 0.0f, 0.0f));
  ShapeCastTarget target3(environment, JPH::RVec3(6.0f, 0.0f, 0.0f));
  for (JoltPhysicsController *controller : {
           &sensor.controller, &target1.controller, &target2.controller, &target3.controller})
  {
    environment.AddController(controller);
  }

  std::vector<PHY_RayQuerySettings> settings(32);
  for (size_t index = 0; index < settings.size(); index++) {
    PHY_RayQuerySettings &ray = settings[index];
    ray.max_results = 1;
    switch (index % 4) {
      case 0:
        ray.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
        ray.destination = MT_Vector3(10.0f, 0.0f, 0.0f);
        break;
      case 1:
        ray.origin = MT_Vector3(10.0f, 0.0f, 0.0f);
        ray.destination = MT_Vector3(0.0f, 0.0f, 0.0f);
        break;
      case 2:
        ray.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
        ray.destination = MT_Vector3(10.0f, 0.0f, 0.0f);
        ray.include_sensors = true;
        break;
      case 3:
        ray.origin = MT_Vector3(0.0f, 2.0f, 0.0f);
        ray.destination = MT_Vector3(10.0f, 2.0f, 0.0f);
        break;
    }
  }

  CountingRayCastFilter sequential_filter;
  CountingRayCastFilter adaptive_filter;
  std::vector<std::vector<PHY_RayCastResult>> sequential;
  std::vector<std::vector<PHY_RayCastResult>> adaptive;
  ASSERT_TRUE(environment.RayCastBatch(
      settings, sequential_filter, PHY_RayBatchExecution::Sequential, sequential));
  ASSERT_TRUE(environment.RayCastBatch(
      settings, adaptive_filter, PHY_RayBatchExecution::Adaptive, adaptive));
  ASSERT_EQ(sequential.size(), settings.size());
  ASSERT_EQ(adaptive.size(), settings.size());
  EXPECT_GT(sequential_filter.call_count.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(adaptive_filter.call_count.load(std::memory_order_relaxed), 4u);
  EXPECT_FALSE(sequential_filter.called_from_worker.load(std::memory_order_relaxed));
  EXPECT_FALSE(adaptive_filter.called_from_worker.load(std::memory_order_relaxed));

  for (size_t index = 0; index < settings.size(); index++) {
    SCOPED_TRACE(index);
    ASSERT_EQ(adaptive[index].size(), sequential[index].size());
    if (index % 4 == 3) {
      EXPECT_TRUE(adaptive[index].empty());
      continue;
    }
    ASSERT_EQ(adaptive[index].size(), 1u);
    const PHY_IPhysicsController *expected = index % 4 == 0 ?
                                                 &target1.controller :
                                             index % 4 == 1 ? &target3.controller :
                                                              &sensor.controller;
    EXPECT_EQ(adaptive[index][0].m_controller, expected);
    EXPECT_EQ(adaptive[index][0].m_controller, sequential[index][0].m_controller);
    EXPECT_NEAR(adaptive[index][0].m_fraction, sequential[index][0].m_fraction, 1.0e-6f);
    EXPECT_NEAR(adaptive[index][0].m_hitPoint.x(), sequential[index][0].m_hitPoint.x(), 1.0e-6f);
  }

  for (JoltPhysicsController *controller : {
           &sensor.controller, &target1.controller, &target2.controller, &target3.controller})
  {
    EXPECT_TRUE(environment.RemoveController(controller));
  }
}

TEST(JoltPhysicsEnvironment, RayCastBatchAdaptiveKeepsSmallBatchesSequential)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  ShapeCastTarget target(environment, JPH::RVec3(2.0f, 0.0f, 0.0f));
  environment.AddController(&target.controller);

  std::vector<PHY_RayQuerySettings> settings(15);
  for (PHY_RayQuerySettings &ray : settings) {
    ray.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
    ray.destination = MT_Vector3(10.0f, 0.0f, 0.0f);
    ray.max_results = 1;
  }

  CountingRayCastFilter filter;
  std::vector<std::vector<PHY_RayCastResult>> results;
  EXPECT_TRUE(environment.RayCastBatch(
      settings, filter, PHY_RayBatchExecution::Adaptive, results));
  EXPECT_EQ(results.size(), settings.size());
  EXPECT_GT(filter.call_count.load(std::memory_order_relaxed), 1u);
  EXPECT_FALSE(filter.called_from_worker.load(std::memory_order_relaxed));
  for (const std::vector<PHY_RayCastResult> &ray_results : results) {
    ASSERT_EQ(ray_results.size(), 1u);
    EXPECT_EQ(ray_results.front().m_controller, &target.controller);
  }

  EXPECT_TRUE(environment.RemoveController(&target.controller));
}

TEST(JoltPhysicsEnvironment, RayCastBatchAdaptiveFallsBackWhenBarrierPoolIsExhausted)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  ShapeCastTarget target(environment, JPH::RVec3(2.0f, 0.0f, 0.0f));
  environment.AddController(&target.controller);

  std::vector<JPH::JobSystem::Barrier *> held_barriers;
  bool barrier_pool_exhausted = false;
  for (int index = 0; index <= JPH::cMaxPhysicsBarriers; index++) {
    JPH::JobSystem::Barrier *barrier = environment.AcquireJobBarrierForTest();
    if (!barrier) {
      barrier_pool_exhausted = true;
      break;
    }
    held_barriers.push_back(barrier);
  }

  std::vector<PHY_RayQuerySettings> settings(16);
  for (PHY_RayQuerySettings &ray : settings) {
    ray.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
    ray.destination = MT_Vector3(10.0f, 0.0f, 0.0f);
    ray.max_results = 1;
  }
  CountingRayCastFilter filter;
  std::vector<std::vector<PHY_RayCastResult>> results;
  const bool succeeded = environment.RayCastBatch(
      settings, filter, PHY_RayBatchExecution::Adaptive, results);

  for (JPH::JobSystem::Barrier *barrier : held_barriers) {
    environment.ReleaseJobBarrierForTest(barrier);
  }

  EXPECT_TRUE(barrier_pool_exhausted);
  EXPECT_TRUE(succeeded);
  EXPECT_EQ(results.size(), settings.size());
  EXPECT_GT(filter.call_count.load(std::memory_order_relaxed), 1u);
  EXPECT_FALSE(filter.called_from_worker.load(std::memory_order_relaxed));
  for (const std::vector<PHY_RayCastResult> &ray_results : results) {
    ASSERT_EQ(ray_results.size(), 1u);
    EXPECT_EQ(ray_results.front().m_controller, &target.controller);
  }

  EXPECT_TRUE(environment.RemoveController(&target.controller));
}

TEST(JoltPhysicsEnvironment, RayCastSensorsAreOptIn)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  ShapeCastTarget sensor(environment, JPH::RVec3(2.0f, 0.0f, 0.0f), true);
  ShapeCastTarget solid(environment, JPH::RVec3(4.0f, 0.0f, 0.0f));
  AcceptAllRayCastFilter filter;

  PHY_RayQuerySettings settings;
  settings.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
  settings.destination = MT_Vector3(10.0f, 0.0f, 0.0f);
  settings.max_results = 2;
  std::vector<PHY_RayCastResult> results;
  ASSERT_TRUE(environment.RayCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].m_controller, &solid.controller);

  settings.include_sensors = true;
  ASSERT_TRUE(environment.RayCast(settings, filter, results));
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].m_controller, &sensor.controller);
  EXPECT_EQ(results[1].m_controller, &solid.controller);
}

TEST(JoltPhysicsEnvironment, ShapeCastSupportsAllPrimitiveModesAndInitialOverlap)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  ShapeCastTarget target(environment, JPH::RVec3(3.0f, 0.0f, 0.0f));
  AcceptAllShapeCastFilter filter;

  PHY_ShapeCastSettings settings;
  settings.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
  settings.destination = MT_Vector3(5.0f, 0.0f, 0.0f);
  settings.radius = 0.5f;
  settings.half_extents = MT_Vector3(0.5f, 0.5f, 0.5f);
  settings.height = 2.0f;

  for (const PHY_ShapeCastType type :
       {PHY_ShapeCastType::Sphere, PHY_ShapeCastType::Box, PHY_ShapeCastType::Capsule})
  {
    settings.type = type;
    std::vector<PHY_ShapeCastResult> results;
    ASSERT_TRUE(environment.ShapeCast(settings, filter, results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].controller, &target.controller);
    EXPECT_NEAR(results[0].fraction, 0.4f, 1.0e-4f);
    EXPECT_NEAR(results[0].cast_position.x(), 2.0f, 1.0e-4f);
    EXPECT_NEAR(results[0].point.x(), 2.5f, 1.0e-4f);
    EXPECT_LT(results[0].normal.x(), -0.99f);
    EXPECT_FALSE(results[0].started_overlapping);
  }

  settings.type = PHY_ShapeCastType::Sphere;
  settings.extra_radius = 0.5f;
  std::vector<PHY_ShapeCastResult> results;
  ASSERT_TRUE(environment.ShapeCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_NEAR(results[0].fraction, 0.3f, 1.0e-4f);
  EXPECT_NEAR(results[0].cast_position.x(), 1.5f, 1.0e-4f);

  settings.type = PHY_ShapeCastType::Capsule;
  settings.radius = 1.0f;
  settings.height = 1.0f;
  settings.extra_radius = 0.0f;
  ASSERT_TRUE(environment.ShapeCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_NEAR(results[0].fraction, 0.3f, 1.0e-4f);

  settings.type = PHY_ShapeCastType::Box;
  settings.radius = 0.5f;
  settings.height = 2.0f;
  settings.extra_radius = 0.0f;
  settings.half_extents = MT_Vector3(1.0f, 0.25f, 0.25f);
  settings.orientation = MT_Matrix3x3::Identity();
  ASSERT_TRUE(environment.ShapeCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_NEAR(results[0].fraction, 0.3f, 1.0e-4f);

  settings.orientation = MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, 0.5f * 3.14159265358979323846f));
  ASSERT_TRUE(environment.ShapeCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_NEAR(results[0].fraction, 0.45f, 1.0e-4f);

  settings.type = PHY_ShapeCastType::Sphere;
  settings.radius = 0.25f;
  settings.orientation = MT_Matrix3x3::Identity();
  settings.origin = MT_Vector3(3.0f, 0.0f, 0.0f);
  settings.destination = settings.origin;
  std::vector<PHY_ShapeCastResult> overlap_results;
  ASSERT_TRUE(environment.ShapeCast(settings, filter, overlap_results));
  ASSERT_EQ(overlap_results.size(), 1u);
  EXPECT_TRUE(overlap_results[0].started_overlapping);
  EXPECT_GT(overlap_results[0].penetration_depth, 0.0f);
  EXPECT_FLOAT_EQ(overlap_results[0].fraction, 0.0f);
  EXPECT_EQ(overlap_results[0].cast_position, settings.origin);
}

TEST(JoltPhysicsEnvironment, ShapeCastAllIsBoundedSortedAndUniquePerBody)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  ShapeCastTarget target1(environment, JPH::RVec3(2.0f, 0.0f, 0.0f));
  ShapeCastTarget target2(environment, JPH::RVec3(4.0f, 0.0f, 0.0f));
  ShapeCastTarget target3(environment, JPH::RVec3(6.0f, 0.0f, 0.0f));
  AcceptAllShapeCastFilter filter;

  PHY_ShapeCastSettings settings;
  settings.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
  settings.destination = MT_Vector3(8.0f, 0.0f, 0.0f);
  settings.radius = 0.25f;
  settings.max_results = 2;
  std::vector<PHY_ShapeCastResult> results;
  ASSERT_TRUE(environment.ShapeCast(settings, filter, results));
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].controller, &target1.controller);
  EXPECT_EQ(results[1].controller, &target2.controller);
  EXPECT_LT(results[0].fraction, results[1].fraction);
  EXPECT_NE(results[0].controller, results[1].controller);
}

TEST(JoltPhysicsEnvironment, ShapeCastSensorsAreOptIn)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  ShapeCastTarget sensor(environment, JPH::RVec3(2.0f, 0.0f, 0.0f), true);
  AcceptAllShapeCastFilter filter;

  PHY_ShapeCastSettings settings;
  settings.origin = MT_Vector3(0.0f, 0.0f, 0.0f);
  settings.destination = MT_Vector3(4.0f, 0.0f, 0.0f);
  settings.radius = 0.25f;
  std::vector<PHY_ShapeCastResult> results;
  ASSERT_TRUE(environment.ShapeCast(settings, filter, results));
  EXPECT_TRUE(results.empty());

  settings.include_sensors = true;
  ASSERT_TRUE(environment.ShapeCast(settings, filter, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].controller, &sensor.controller);
}

TEST(JoltPhysicsEnvironment, PhysicsThreadsCountTheGamesOwnThread)
{
  /* Auto (-1 and 0): half the CPU's threads as helpers, at most 8. */
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(-1, 32), 8);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(0, 32), 8);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(-1, 12), 6);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(-1, 8), 4);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(-1, 2), 1);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(-1, 1), 0);
  /* An unknown CPU (0 threads reported) counts as one thread. */
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(-1, 0), 0);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(4, 0), 0);
  /* N threads in all: the game's own and N - 1 helpers, never more than the CPU has. */
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(1, 32), 0);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(4, 32), 3);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(32, 32), 31);
  EXPECT_EQ(JoltPhysicsEnvironment::HelperThreadsForSetting(64, 8), 7);
}

TEST(JoltPhysicsEnvironment, ContactRoomGrowsWithMaxBodies)
{
  EXPECT_EQ(JoltPhysicsEnvironment::ContactCapacityFor(10240, 10240), 65536);
  EXPECT_EQ(JoltPhysicsEnvironment::ContactCapacityFor(65536, 65536), 65536);
  EXPECT_EQ(JoltPhysicsEnvironment::ContactCapacityFor(200000, 65536), 200000);
  /* A scene's own larger value (set from Python) is kept. */
  EXPECT_EQ(JoltPhysicsEnvironment::ContactCapacityFor(65536, 131072), 131072);
}

TEST(JoltPhysicsEnvironment, FullContactCacheIsReportedOncePerKind)
{
  blender::Scene scene{};
  /* Room for 4 body pairs and 4 contacts, and a heap of 24 overlapping boxes. */
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 4, 4, 8, false);
  std::vector<std::unique_ptr<LogicContactTarget>> boxes;
  for (int index = 0; index < 24; index++) {
    boxes.push_back(std::make_unique<LogicContactTarget>(
        environment,
        JPH::EMotionType::Dynamic,
        JPH::RVec3(0.1f * float(index % 4), 0.1f * float(index / 4), 0.0f)));
  }

  testing::internal::CaptureStdout();
  constexpr float timeStep = 1.0f / 60.0f;
  for (int step = 0; step < 6; step++) {
    ASSERT_TRUE(environment.ProceedDeltaTime(step * timeStep, timeStep, timeStep));
  }
  fflush(stdout);
  const std::string output = testing::internal::GetCapturedStdout();

  const uint32_t reported = environment.ReportedUpdateErrors();
  EXPECT_NE(reported, 0u);
  const std::string warning = "Warning: Jolt physics ran out of room for contacts";
  int lines = 0;
  for (size_t at = output.find(warning); at != std::string::npos;
       at = output.find(warning, at + 1))
  {
    lines++;
  }
  /* Every full step dropped contacts, yet each kind is told once: a line names new kinds only. */
  EXPECT_GE(lines, 1) << output;
  EXPECT_LE(lines, std::popcount(reported)) << output;
  EXPECT_NE(output.find("Raise Max Bodies"), std::string::npos);
}

TEST(JoltPhysicsEnvironment, StaticBodySkipsUnmovedStepsButFollowsEveryMove)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget level(environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  SetControllerQueryMotionState(level.controller, MT_Vector3(0.0f, 0.0f, 0.0f));
  PHY_IMotionState *motionState = level.controller.GetMotionState();
  ASSERT_NE(motionState, nullptr);
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  const auto bodyX = [&]() { return float(bodyInterface.GetPosition(level.body_id).GetX()); };

  constexpr float timeStep = 1.0f / 60.0f;
  int steps = 0;
  const auto step = [&]() {
    EXPECT_TRUE(environment.ProceedDeltaTime(steps * timeStep, timeStep, timeStep));
    steps++;
  };

  /* Found in sync at the origin, then saved there. */
  step();
  std::vector<uint8_t> state;
  ASSERT_TRUE(environment.SavePhysicsState(state));

  /* The game moves it: the next step moves the body, later unmoved steps keep it. */
  motionState->SetWorldPosition(MT_Vector3(1.0f, 0.0f, 0.0f));
  step();
  EXPECT_NEAR(bodyX(), 1.0f, 1.0e-5f);
  step();
  step();
  EXPECT_NEAR(bodyX(), 1.0f, 1.0e-5f);

  /* A restored physics state moves the body behind its controller: the scene graph wins. */
  ASSERT_TRUE(environment.RestorePhysicsState(state));
  EXPECT_NEAR(bodyX(), 0.0f, 1.0e-5f);
  step();
  EXPECT_NEAR(bodyX(), 1.0f, 1.0e-5f);

  /* A direct controller write (Set Position writes Jolt before the scene graph follows) is checked
   * again at the next step as well. */
  level.controller.SetPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  EXPECT_NEAR(bodyX(), 2.0f, 1.0e-5f);
  step();
  EXPECT_NEAR(bodyX(), 1.0f, 1.0e-5f);

  /* A turn of the scene graph alone is followed too. */
  motionState->SetWorldOrientation(MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, 0.5f)));
  step();
  const JPH::Quat rotation = bodyInterface.GetRotation(level.body_id);
  EXPECT_GT(std::abs(rotation.GetY()) + std::abs(rotation.GetZ()), 0.2f);
}

}  // namespace

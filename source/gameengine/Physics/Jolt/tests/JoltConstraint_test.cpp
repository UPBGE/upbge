/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <Jolt/Jolt.h>
JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/StateRecorderImpl.h>

#include "DNA_scene_types.h"

#include "JoltConstraint.h"
#include "JoltDefaultMotionState.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "SG_Node.h"

#include <cmath>
#include <memory>
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
  AcceptAllShapeCastFilter() : PHY_IShapeCastFilterCallback(nullptr)
  {
  }
};

class AcceptAllRayCastFilter : public PHY_IRayCastFilterCallback {
 public:
  AcceptAllRayCastFilter() : PHY_IRayCastFilterCallback(nullptr)
  {
  }

  void reportHit(PHY_RayCastResult * /*result*/) override
  {
  }
};

struct ShapeCastTarget {
  ShapeCastTarget(TestJoltPhysicsEnvironment &environment,
                  const JPH::RVec3 &position,
                  const bool sensor = false)
      : client_info(&object,
                    sensor ? KX_ClientObjectInfo::SENSOR : KX_ClientObjectInfo::STATIC)
  {
    const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
    JPH::BodyCreationSettings settings(shape,
                                       position,
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Static,
                                       JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC));
    settings.mIsSensor = sensor;
    body_id = environment.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::DontActivate);
    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetNewClientInfo(&client_info);
    object.SetPhysicsController(&controller);
    environment.GetBodyInterface().SetUserData(
        body_id, reinterpret_cast<JPH::uint64>(&client_info));
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
                     const unsigned short collisionLayer = 1)
      : client_info(&object,
                    motionType == JPH::EMotionType::Static ? KX_ClientObjectInfo::STATIC :
                                                            KX_ClientObjectInfo::ACTOR)
  {
    const JoltBroadPhaseLayer broadPhaseLayer = motionType == JPH::EMotionType::Static ?
                                                   JOLT_BP_STATIC :
                                                   JOLT_BP_DYNAMIC;
    const JPH::RefConst<JPH::Shape> shape =
        new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
    body_id = environment.GetBodyInterface().CreateAndAddBody(
        JPH::BodyCreationSettings(
            shape, position, JPH::Quat::sIdentity(), motionType,
            JoltMakeObjectLayer(collisionLayer, collisionLayer, broadPhaseLayer)),
        motionType == JPH::EMotionType::Static ? JPH::EActivation::DontActivate :
                                                JPH::EActivation::Activate);

    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetDynamic(motionType == JPH::EMotionType::Dynamic);
    controller.SetOriginalMotionType(motionType);
    controller.SetBroadPhaseCategory(broadPhaseLayer);
    controller.SetShape(shape, nullptr);
    controller.SetNewClientInfo(&client_info);
    object.SetPhysicsController(&controller);
    environment.AddController(&controller);
    environment.GetBodyInterface().SetUserData(
        body_id, reinterpret_cast<JPH::uint64>(&client_info));
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

struct LogicObjectSensorTarget {
  LogicObjectSensorTarget(TestJoltPhysicsEnvironment &environment,
                          const JPH::RVec3 &position,
                          const KX_ClientObjectInfo::clienttype sensorType =
                              KX_ClientObjectInfo::OBSENSOR,
                          const bool activeSceneObject = true,
                          const bool includeStatic = false)
      : client_info(&object, sensorType)
  {
    const JPH::RefConst<JPH::Shape> shape =
        new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
    JPH::BodyCreationSettings settings(shape,
                                       position,
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Static,
                                       JoltMakeObjectLayer(1, 1, JOLT_BP_SENSOR));
    settings.mIsSensor = true;
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
    const JPH::RefConst<JPH::Shape> shape =
        new JPH::BoxShape(JPH::Vec3::sReplicate(0.25f));
    JPH::BodyCreationSettings settings(shape,
                                       position,
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Dynamic,
                                       JoltMakeObjectLayer(1, 1, JOLT_BP_DYNAMIC));
    settings.mIsSensor = true;
    body_id = environment.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::Activate);

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
    environment.GetBodyInterface().SetUserData(
        body_id, reinterpret_cast<JPH::uint64>(&client_info));
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

TEST(JoltPhysicsEnvironment, LogicObjectSensorQueriesStaticAndSleepingBodiesWithoutRegistration)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget sensor(environment,
                                 JPH::RVec3::sZero(),
                                 KX_ClientObjectInfo::OBSENSOR,
                                 true,
                                 true);
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
  EXPECT_FALSE(environment.CheckCollision(
      &staticTarget.controller, &sensor.controller, false).collide);

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
  EXPECT_FALSE(environment.CheckCollision(
      &sensor.controller, &layerFilteredTarget.controller, false).collide);

  /* Trigger overlaps are deliberately not injected into the solid targets' collision lists. */
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &staticTarget.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &sleepingTarget.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
}

TEST(JoltPhysicsEnvironment, LogicObjectSensorDetectsGhostButNotTrueSensor)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicObjectSensorTarget sensor(environment,
                                 JPH::RVec3::sZero(),
                                 KX_ClientObjectInfo::OBSENSOR,
                                 true,
                                 true);
  LogicContactTarget ghost(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicObjectSensorTarget trueSensor(environment,
                                     JPH::RVec3(-0.75f, 0.0f, 0.0f),
                                     KX_ClientObjectInfo::OBSENSOR,
                                     false);
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
  EXPECT_TRUE(environment.CheckCollision(
      &sensor.controller, &ghost.controller, false).collide);
  EXPECT_FALSE(environment.CheckCollision(
      &sensor.controller, &trueSensor.controller, false).collide);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  environment.CallbackTriggers();

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &sensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->size(), 1u);
  EXPECT_EQ((*contacts)[0]->object1, &ghost.object);

  /* Sensor query overlaps remain one-sided and never become solid collision data. */
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &ghost.controller, contacts, false));
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
  EXPECT_FALSE(environment.CheckCollision(
      &sensor.controller, &staticTarget.controller, false).collide);
  EXPECT_FALSE(environment.CheckCollision(
      &sensor.controller, &staticGhost.controller, false).collide);
  EXPECT_TRUE(environment.CheckCollision(
      &sensor.controller, &sleepingTarget.controller, false).collide);
  EXPECT_TRUE(environment.CheckCollision(
      &sensor.controller, &kinematicTarget.controller, false).collide);
  EXPECT_TRUE(environment.CheckCollision(
      &sensor.controller, &dynamicGhost.controller, false).collide);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 0u);

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &sensor.controller, contacts, false));
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
  EXPECT_FALSE(environment.GetCachedCollisionContactRefs(
      &sensor.controller, contacts, false));
  EXPECT_TRUE(environment.CheckCollision(
      &sensor.controller, &staticTarget.controller, false).collide);
  EXPECT_TRUE(environment.CheckCollision(
      &sensor.controller, &staticGhost.controller, false).collide);

  environment.CallbackTriggers();
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &sensor.controller, contacts, false));
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
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &actorSensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->size(), 1u);
  EXPECT_EQ((*contacts)[0]->object1, &actorTarget.object);

  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &inactiveSensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
  EXPECT_FALSE(environment.CheckCollision(
      &inactiveSensor.controller, &staticTarget.controller, false).collide);

  actorSensor.controller.SuspendPhysics(false);
  environment.CallbackTriggers();
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &actorSensor.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
}

TEST(JoltPhysicsEnvironment, ParentedRigidChildQueriesCurrentTransformAndRejectsOwnHierarchy)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget parent(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  LogicContactTarget child(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(10.0f, 0.0f, 0.0f));
  LogicContactTarget externalTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  TestObjectHierarchy hierarchy(parent.object, child.object);
  SetControllerQueryMotionState(child.controller, MT_Vector3(0.0f, 0.0f, 0.0f));

  child.controller.SetLogicCollisionQueryActive(true);
  child.controller.SuspendDynamics(false);
  EXPECT_TRUE(child.controller.IsParentedCollisionQuery());
  EXPECT_FALSE(environment.GetBodyInterface().IsAdded(child.body_id));

  /* The removed Jolt body remains at x=10. Only the motion-state/scene-graph position at
   * x=0 overlaps these targets, proving that parent following does not use a stale body pose. */
  EXPECT_TRUE(environment.CheckCollision(
      &child.controller, &externalTarget.controller, false).collide);
  EXPECT_FALSE(environment.CheckCollision(
      &child.controller, &parent.controller, false).collide);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 0u);

  const std::vector<const PHY_CachedCollisionContact *> *contacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(&child.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->size(), 1u);
  EXPECT_EQ((*contacts)[0]->object0, &child.object);
  EXPECT_EQ((*contacts)[0]->object1, &externalTarget.object);
  EXPECT_EQ(environment.LogicObjectSensorQueryCount(), 1u);
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
  EXPECT_TRUE(sensor.controller.IsParentedCollisionQuery());
  EXPECT_TRUE(sensor.controller.IsLogicObjectSensorActive());
  EXPECT_TRUE(environment.CheckCollision(
      &sensor.controller, &staticTarget.controller, false).collide);

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
  LogicContactTarget parent(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3::sZero());
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3(0.75f, 0.0f, 0.0f));
  KX_GameObject child;
  TestObjectHierarchy hierarchy(parent.object, child);

  const JPH::RefConst<JPH::Shape> rootShape =
      new JPH::BoxShape(JPH::Vec3::sReplicate(0.1f));
  const JPH::RefConst<JPH::Shape> childShape =
      new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f));
  const JPH::uint32 childUserData = parent.controller.RegisterCompoundChildBinding(
      &child, JPH::Vec3::sZero(), JPH::Quat::sIdentity());
  ASSERT_NE(childUserData, 0u);

  JPH::StaticCompoundShapeSettings compoundSettings;
  compoundSettings.AddShape(
      JPH::Vec3(-3.0f, 0.0f, 0.0f), JPH::Quat::sIdentity(), rootShape, 0);
  compoundSettings.AddShape(
      JPH::Vec3::sZero(), JPH::Quat::sIdentity(), childShape, childUserData);
  JPH::Shape::ShapeResult compoundResult = compoundSettings.Create();
  ASSERT_FALSE(compoundResult.HasError());
  parent.controller.SetShapePreservingMassPropertiesAndCenterOfMass(
      compoundResult.Get(), nullptr);

  environment.SetLogicCollisionContactCacheEnabled(true, false);
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0f / 60.0f, 1.0f / 60.0f));

  const std::vector<const PHY_CachedCollisionContact *> *childContacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefsForObject(
      &child, childContacts, false));
  ASSERT_NE(childContacts, nullptr);
  ASSERT_FALSE(childContacts->empty());
  for (const PHY_CachedCollisionContact *contact : *childContacts) {
    ASSERT_NE(contact, nullptr);
    EXPECT_TRUE(contact->object0 == &child || contact->object1 == &child);
    EXPECT_TRUE(contact->object0 == &staticTarget.object ||
                contact->object1 == &staticTarget.object);
  }

  const std::vector<const PHY_CachedCollisionContact *> *parentContacts = nullptr;
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &parent.controller, parentContacts, false));
  ASSERT_NE(parentContacts, nullptr);
  EXPECT_FALSE(parentContacts->empty());

  environment.GetBodyInterface().SetPosition(staticTarget.body_id,
                                              JPH::RVec3(10.0f, 0.0f, 0.0f),
                                              JPH::EActivation::Activate);
  ASSERT_TRUE(environment.ProceedDeltaTime(0.0, 1.0f / 60.0f, 1.0f / 60.0f));
  ASSERT_TRUE(environment.GetCachedCollisionContactRefsForObject(
      &child, childContacts, false));
  ASSERT_NE(childContacts, nullptr);
  EXPECT_TRUE(childContacts->empty());
}

TEST(JoltPhysicsEnvironment, LogicContactRemovalRetainsOnlyDormantTouchingPairs)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
  JPH::BodyInterface &bodyInterface = environment.GetBodyInterface();
  environment.SetLogicCollisionContactCacheEnabled(true, false);

  const JPH::SubShapeIDPair key = MakeLogicContactKey(
      dynamicTarget.body_id, staticTarget.body_id, 1, 2);
  ASSERT_TRUE(bodyInterface.IsActive(dynamicTarget.body_id));
  ASSERT_TRUE(environment.CheckCollision(
      &dynamicTarget.controller, &staticTarget.controller, false).collide);

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
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &dynamicTarget.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_EQ(contacts->size(), 1u);

  /* A retained sleep contact is only valid while the pair remains dormant.
   * Waking without a fresh contact update must expose the collision exit. */
  bodyInterface.ActivateBody(dynamicTarget.body_id);
  environment.CallbackTriggers();
  EXPECT_EQ(environment.LogicContactCount(), 0u);
  ASSERT_TRUE(environment.GetCachedCollisionContactRefs(
      &dynamicTarget.controller, contacts, false));
  ASSERT_NE(contacts, nullptr);
  EXPECT_TRUE(contacts->empty());
}

TEST(JoltPhysicsEnvironment, LogicContactRemovalDoesNotRetainDormantSeparation)
{
  blender::Scene scene{};
  TestJoltPhysicsEnvironment environment(&scene, 1, 128, 128, 128, 8, false);
  LogicContactTarget dynamicTarget(
      environment, JPH::EMotionType::Dynamic, JPH::RVec3(0.75f, 0.0f, 0.0f));
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
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
  ASSERT_FALSE(environment.CheckCollision(
      &dynamicTarget.controller, &staticTarget.controller, false).collide);

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
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
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
  LogicContactTarget secondTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
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
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
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
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
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
  LogicContactTarget staticTarget(
      environment, JPH::EMotionType::Static, JPH::RVec3::sZero());
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
  const JPH::RefConst<JPH::Shape> scaledShape = new JPH::ScaledShape(
      meshShape.GetPtr(), JPH::Vec3(-1.0f, 1.0f, 1.0f));
  JPH::StaticCompoundShapeSettings compoundSettings;
  compoundSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), scaledShape);
  const JPH::Shape::ShapeResult compoundResult = compoundSettings.Create();
  ASSERT_FALSE(compoundResult.HasError());
  const JPH::RefConst<JPH::Shape> compoundShape = compoundResult.Get();

  std::shared_ptr<JoltMeshQueryData> meshData = std::make_shared<JoltMeshQueryData>();
  meshData->polygonIndices = {42};
  JoltTriangleUV triangleUV;
  triangleUV.corners = {
      MT_Vector2(0.0f, 0.0f), MT_Vector2(0.0f, 1.0f), MT_Vector2(1.0f, 0.0f)};
  meshData->triangleUVs = {triangleUV};
  std::shared_ptr<JoltShapeQueryData> shapeQueryData =
      std::make_shared<JoltShapeQueryData>();
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
  environment.GetBodyInterface().SetUserData(
      bodyID, reinterpret_cast<JPH::uint64>(&clientInfo));

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

  for (const PHY_ShapeCastType type : {PHY_ShapeCastType::Sphere,
                                       PHY_ShapeCastType::Box,
                                       PHY_ShapeCastType::Capsule})
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

  settings.orientation = MT_Matrix3x3(
      MT_Vector3(0.0f, 0.0f, 0.5f * 3.14159265358979323846f));
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

}  // namespace

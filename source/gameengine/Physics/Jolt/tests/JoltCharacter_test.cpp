/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** The Character physics type on Jolt (2026-11-16): JoltCharacter moves its object, the
 * object's body follows, contacts reach On Collision, copies walk, and many characters moving on
 * Jolt's threads end exactly as one after another. A real Jolt world: a floor, boxes, steps, a
 * ceiling; the environment's own step (ProceedDeltaTime). */

#include "testing/testing.h"

#include <Jolt/Jolt.h>
JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include "DNA_scene_types.h"

#include "JoltCharacter.h"
#include "JoltDefaultMotionState.h"
#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "SG_Node.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr float kStep = 1.0f / 60.0f;

/** As the engine's KX_MotionState: a physics write sets the object's local pose, its world pose
 * follows when the scene updates after the step (KX_Scene::UpdateParents: World::Step calls
 * Update), while a move by logic (Teleport) is seen at once. (JoltDefaultMotionState writes the
 * world pose at once: a body moved back to the object's old pose in the step went unseen.) */
class EngineMotionState final : public PHY_IMotionState {
 public:
  MT_Vector3 GetWorldPosition() const override
  {
    return m_world;
  }
  MT_Vector3 GetWorldScaling() const override
  {
    return MT_Vector3(1.0f, 1.0f, 1.0f);
  }
  MT_Matrix3x3 GetWorldOrientation() const override
  {
    return m_worldOrientation;
  }
  void SetWorldPosition(const MT_Vector3 &pos) override
  {
    m_local = pos;
  }
  void SetWorldOrientation(const MT_Matrix3x3 &ori) override
  {
    m_localOrientation = ori;
  }
  void SetWorldOrientation(const MT_Quaternion &quat) override
  {
    m_localOrientation = MT_Matrix3x3(quat);
  }
  void CalculateWorldTransformations() override {}

  void Update()
  {
    m_world = m_local;
    m_worldOrientation = m_localOrientation;
  }
  void Teleport(const MT_Vector3 &position)
  {
    m_local = m_world = position;
  }

 private:
  MT_Vector3 m_local{0.0f, 0.0f, 0.0f};
  MT_Vector3 m_world{0.0f, 0.0f, 0.0f};
  MT_Matrix3x3 m_localOrientation = MT_Matrix3x3::Identity();
  MT_Matrix3x3 m_worldOrientation = MT_Matrix3x3::Identity();
};

/** One object with a Jolt body and its controller (no Blender object): a floor, a box, a
 * character's body. Positions are Blender's (Z up). */
struct TestObject {
  TestObject(JoltPhysicsEnvironment &environment,
             const JPH::EMotionType motionType,
             JPH::RefConst<JPH::Shape> shape,
             const MT_Vector3 &position,
             const bool character = false)
      : client_info(&object,
                    motionType == JPH::EMotionType::Static ? KX_ClientObjectInfo::STATIC :
                                                             KX_ClientObjectInfo::ACTOR)
  {
    const JoltBroadPhaseLayer broadPhaseLayer = motionType == JPH::EMotionType::Static ?
                                                    JOLT_BP_STATIC :
                                                    JOLT_BP_DYNAMIC;
    JPH::BodyCreationSettings settings(shape,
                                       JoltMath::ToJolt(position),
                                       JPH::Quat::sIdentity(),
                                       motionType,
                                       JoltMakeObjectLayer(1, 1, broadPhaseLayer));
    settings.mAllowSleeping = !character;
    if (motionType == JPH::EMotionType::Dynamic) {
      settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
      settings.mMassPropertiesOverride.mMass = 10.0f;
    }
    body_id = environment.GetBodyInterface().CreateAndAddBody(
        settings,
        motionType == JPH::EMotionType::Static ? JPH::EActivation::DontActivate :
                                                 JPH::EActivation::Activate);

    motion_state = new EngineMotionState();
    motion_state->Teleport(position);
    controller.SetMotionState(motion_state);
    controller.SetEnvironment(&environment);
    controller.SetBodyID(body_id);
    controller.SetDynamic(motionType == JPH::EMotionType::Dynamic);
    controller.SetKinematic(motionType == JPH::EMotionType::Kinematic);
    controller.SetOriginalMotionType(motionType);
    controller.SetBroadPhaseCategory(broadPhaseLayer);
    controller.SetShape(shape, nullptr);
    controller.SetCollisionGroup(1);
    controller.SetCollisionMask(1);
    controller.SetNewClientInfo(&client_info);
    object.SetPhysicsController(&controller);
    environment.AddController(&controller);
    environment.GetBodyInterface().SetUserData(body_id,
                                               reinterpret_cast<JPH::uint64>(&client_info));
  }

  ~TestObject()
  {
    object.SetPhysicsController(nullptr);
    controller.SetNewClientInfo(nullptr);
  }

  MT_Vector3 Position()
  {
    return controller.GetMotionState()->GetWorldPosition();
  }

  KX_GameObject object;
  KX_ClientObjectInfo client_info;
  JoltPhysicsController controller;
  JPH::BodyID body_id;
  /** (The controller owns it.) */
  EngineMotionState *motion_state = nullptr;
};

JPH::RefConst<JPH::Shape> Box(const float x, const float y, const float z)
{
  /* Blender half extents (Z up) as a Jolt box (Y up). */
  return new JPH::BoxShape(JPH::Vec3(x, z, y), 0.0f);
}

/** A capsule 1.8 m tall, 0.3 m round: its origin in the middle (0.9 m above its feet). */
JPH::RefConst<JPH::Shape> CharacterCapsule()
{
  return new JPH::CapsuleShape(0.6f, 0.3f);
}

constexpr float kStand = 0.9f;

/** A world: a 40 x 40 m floor whose top is at z = 0. */
struct World {
  explicit World(const int helperThreads = 0)
      : environment(&scene, helperThreads, 4096, 8192, 8192, 16, false),
        floor(environment, JPH::EMotionType::Static, Box(20.0f, 20.0f, 0.5f), MT_Vector3(0, 0, -0.5f))
  {
    environment.SetGravity(0.0f, 0.0f, -9.81f);
  }

  /** A character standing at (x, y) on the floor (or \a z above it). */
  TestObject &AddCharacter(const float x,
                           const float y,
                           const float z = 0.0f,
                           const float stepHeight = 0.3f)
  {
    objects.push_back(std::make_unique<TestObject>(environment,
                                                   JPH::EMotionType::Kinematic,
                                                   CharacterCapsule(),
                                                   MT_Vector3(x, y, z + kStand),
                                                   true));
    TestObject &object = *objects.back();
    JoltCharacter *character = environment.CreateCharacter(&object.controller, stepHeight);
    EXPECT_NE(character, nullptr);
    return object;
  }

  TestObject &Add(const JPH::EMotionType type,
                  JPH::RefConst<JPH::Shape> shape,
                  const MT_Vector3 &position)
  {
    objects.push_back(std::make_unique<TestObject>(environment, type, shape, position));
    return *objects.back();
  }

  void Step(const int steps = 1)
  {
    for (int i = 0; i < steps; i++) {
      ASSERT_TRUE(environment.ProceedDeltaTime(0.0, kStep, kStep));
      floor.motion_state->Update();
      for (std::unique_ptr<TestObject> &object : objects) {
        object->motion_state->Update();
      }
    }
  }

  static JoltCharacter *CharacterOf(TestObject &object)
  {
    return object.controller.GetCharacter();
  }

  blender::Scene scene{};
  JoltPhysicsEnvironment environment;
  TestObject floor;
  std::vector<std::unique_ptr<TestObject>> objects;
};

}  // namespace

TEST(JoltCharacter, LandsOnTheFloorAndStandsThere)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f, 2.0f);
  world.Step(120);
  JoltCharacter *character = World::CharacterOf(hero);
  ASSERT_NE(character, nullptr);
  /* Its object came down (the old character never moved it) and stands on the floor. */
  EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);
  EXPECT_TRUE(character->OnGround());
  /* Standing: no speed. */
  EXPECT_NEAR(character->GetLinearVelocity().length(), 0.0f, 1.0e-3f);
}

TEST(JoltCharacter, WalksAtItsWalkingVelocityAndItsBodyFollows)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  world.Step(10);
  character->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
  world.Step(60);
  /* 2 m/s for one second. */
  EXPECT_NEAR(hero.Position().x(), 2.0f, 0.05f);
  EXPECT_NEAR(hero.Position().y(), 0.0f, 1.0e-4f);
  EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);
  EXPECT_NEAR(character->GetLinearVelocity().x(), 2.0f, 0.01f);
  /* The body (rays, sensors, other bodies meet the character there) is where the object is. */
  const JPH::RVec3 body = world.environment.GetBodyInterface().GetPosition(hero.body_id);
  EXPECT_NEAR(float(body.GetX()), hero.Position().x(), 1.0e-4f);
  EXPECT_NEAR(float(body.GetZ()), hero.Position().y(), 1.0e-4f);
  EXPECT_NEAR(float(body.GetY()), hero.Position().z(), 1.0e-4f);
  /* (0, 0, 0) stops it. */
  character->SetWalkDirection(MT_Vector3(0.0f, 0.0f, 0.0f));
  world.Step(2);
  const float stopped = hero.Position().x();
  world.Step(30);
  EXPECT_NEAR(hero.Position().x(), stopped, 1.0e-4f);
}

TEST(JoltCharacter, AMoveOfItsObjectIsATeleport)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  world.Step(10);
  /* Logic moves the object (Set Position): the character goes there and walks on from there. */
  hero.motion_state->Teleport(MT_Vector3(5.0f, 3.0f, kStand));
  World::CharacterOf(hero)->SetWalkDirection(MT_Vector3(0.0f, 1.0f, 0.0f));
  world.Step(30);
  EXPECT_NEAR(hero.Position().x(), 5.0f, 1.0e-4f);
  EXPECT_NEAR(hero.Position().y(), 3.5f, 0.05f);
  EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);
}

TEST(JoltCharacter, JumpsOnceThenOnlyAsOftenAsItsMaxJumps)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  character->SetJumpSpeed(5.0f);
  world.Step(10);
  character->Jump();
  world.Step(15);
  /* Up at 5 m/s: about 1 m high after a quarter second. */
  EXPECT_GT(hero.Position().z(), kStand + 0.7f);
  EXPECT_FALSE(character->OnGround());
  EXPECT_EQ(character->GetJumpCount(), 1);
  /* Max Jumps 1: no jump in the air. */
  const float before = hero.Position().z();
  character->Jump();
  world.Step(1);
  EXPECT_LT(character->GetLinearVelocity().z(), 5.0f - 2.0f);
  EXPECT_EQ(character->GetJumpCount(), 1);
  (void)before;
  /* Lands again and may jump again. */
  world.Step(120);
  EXPECT_TRUE(character->OnGround());
  EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);
  /* Max Jumps 2: a second jump in the air. */
  character->SetMaxJumps(2);
  character->Jump();
  world.Step(10);
  character->Jump();
  world.Step(1);
  EXPECT_EQ(character->GetJumpCount(), 2);
  EXPECT_GT(character->GetLinearVelocity().z(), 4.0f);
}

TEST(JoltCharacter, FallSpeedIsTheFastestItFalls)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f, 40.0f);
  World::CharacterOf(hero)->SetFallSpeed(5.0f);
  world.Step(60);
  /* After a second it falls at 5 m/s, not 9.8. */
  EXPECT_NEAR(World::CharacterOf(hero)->GetLinearVelocity().z(), -5.0f, 1.0e-3f);
  EXPECT_GT(hero.Position().z(), 40.0f - 5.0f);
}

TEST(JoltCharacter, ACeilingStopsItsRise)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  /* A ceiling 0.5 m above its head. */
  world.Add(JPH::EMotionType::Static, Box(3.0f, 3.0f, 0.1f), MT_Vector3(0.0f, 0.0f, 2.4f));
  JoltCharacter *character = World::CharacterOf(hero);
  character->SetJumpSpeed(8.0f);
  world.Step(10);
  character->Jump();
  world.Step(12);
  /* It went no higher than the ceiling lets it, and falls again at once (it did not hang there
   * until gravity used its jump up). */
  EXPECT_LT(hero.Position().z(), 2.3f - kStand + 0.05f);
  world.Step(30);
  EXPECT_TRUE(character->OnGround());
}

/** Where a character walking into a box \a height tall ends (Step Height \a step). */
static MT_Vector3 WalkIntoBox(const float height, const float step)
{
  World world;
  world.Add(JPH::EMotionType::Static,
            Box(2.0f, 2.0f, height * 0.5f),
            MT_Vector3(3.0f, 0.0f, height * 0.5f));
  TestObject &hero = world.AddCharacter(0.0f, 0.0f, 0.0f, step);
  world.Step(10);
  World::CharacterOf(hero)->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
  world.Step(120);
  return hero.Position();
}

TEST(JoltCharacter, WalksUpAStepUpToItsStepHeightOnly)
{
  /* A box: x 1..5. Climbed: on it (x > 2) at its height; blocked: before it, on the floor (not
   * hanging on its edge). Step Height is "the maximum height of steps the character can run
   * over" (the panel), 2 cm more at most (kStairTolerance); with no stairs the round bottom
   * takes what a walkable slope reaches. */
  const auto climbs = [](const float height, const float step) {
    const MT_Vector3 p = WalkIntoBox(height, step);
    const bool on = p.x() > 2.0f && std::abs(p.z() - (height + kStand)) < 0.03f;
    const bool before = p.x() < 1.0f && std::abs(p.z() - kStand) < 0.03f;
    EXPECT_TRUE(on || before) << "box " << height << " step " << step << ": ends at " << p.x()
                              << ", " << p.z();
    return on;
  };
  EXPECT_TRUE(climbs(0.3f, 0.3f));
  /* (Climbed before the stair landing check: hung on the edge, walked on over it.) */
  EXPECT_FALSE(climbs(0.35f, 0.3f));
  EXPECT_FALSE(climbs(0.35f, 0.1f));
  EXPECT_FALSE(climbs(0.45f, 0.25f));
  EXPECT_TRUE(climbs(0.2f, 0.25f));
  /* Jolt calls the edge of this box flat (its top face): the stair step never started and the
   * box stopped the character (before the edge marking in Simulate). */
  EXPECT_TRUE(climbs(0.25f, 0.25f));
  EXPECT_TRUE(climbs(0.25f, 0.4f));
  EXPECT_TRUE(climbs(0.4f, 0.4f));
  EXPECT_FALSE(climbs(0.45f, 0.4f));
  /* No stairs: only what the round bottom takes. */
  EXPECT_TRUE(climbs(0.05f, 0.0f));
  EXPECT_FALSE(climbs(0.2f, 0.0f));
  /* (The round bottom rode up this edge as up a ramp before the OnContactSolve rule.) */
  EXPECT_FALSE(climbs(0.25f, 0.0f));
}

TEST(JoltCharacter, PushesABoxItWalksInto)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  TestObject &box = world.Add(
      JPH::EMotionType::Dynamic, Box(0.4f, 0.4f, 0.4f), MT_Vector3(1.5f, 0.0f, 0.4f));
  world.Step(10);
  const float boxStart = box.Position().x();
  World::CharacterOf(hero)->SetWalkDirection(MT_Vector3(1.5f, 0.0f, 0.0f));
  world.Step(120);
  /* The box moved ahead of it (a 10 kg box: Jolt's character strength moves it). */
  const MT_Vector3 boxNow = JoltMath::ToMT(
      JPH::Vec3(world.environment.GetBodyInterface().GetPosition(box.body_id)));
  EXPECT_GT(boxNow.x(), boxStart + 0.5f);
  EXPECT_GT(boxNow.x(), hero.Position().x());
}

TEST(JoltCharacter, ItsContactsReachOnCollision)
{
  World world;
  world.environment.SetLogicCollisionContactCacheEnabled(true, true);
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  TestObject &wall = world.Add(
      JPH::EMotionType::Static, Box(0.1f, 2.0f, 1.0f), MT_Vector3(1.0f, 0.0f, 1.0f));
  world.Step(10);

  const auto touches = [&](TestObject &other) {
    std::vector<PHY_CachedCollisionContact> contacts;
    world.environment.GetCachedCollisionContacts(&hero.controller, contacts, true);
    for (const PHY_CachedCollisionContact &contact : contacts) {
      if (contact.object0 == &other.object || contact.object1 == &other.object) {
        return true;
      }
    }
    return false;
  };
  /* Standing on the floor: the floor is a contact (its body meets static bodies in no Jolt
   * contact: the character reports it). */
  EXPECT_TRUE(touches(world.floor));
  EXPECT_FALSE(touches(wall));
  /* Walks against the wall: the wall too. */
  World::CharacterOf(hero)->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
  world.Step(40);
  EXPECT_TRUE(touches(wall));
  EXPECT_TRUE(touches(world.floor));
  /* The wall sees the character too. */
  std::vector<PHY_CachedCollisionContact> wallContacts;
  world.environment.GetCachedCollisionContacts(&wall.controller, wallContacts, true);
  EXPECT_FALSE(wallContacts.empty());
  /* Jumps away: the floor contact ends. */
  World::CharacterOf(hero)->SetWalkDirection(MT_Vector3(-2.0f, 0.0f, 0.0f));
  World::CharacterOf(hero)->Jump();
  world.Step(10);
  EXPECT_FALSE(touches(world.floor));
  EXPECT_FALSE(touches(wall));
}

TEST(JoltCharacter, SetVelocityLaunchesAndReplacesWalkingForItsTime)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  world.Step(10);
  character->SetWalkDirection(MT_Vector3(1.0f, 0.0f, 0.0f));
  /* Knocked back along -Y and up for half a second. */
  character->SetVelocity(MT_Vector3(0.0f, -4.0f, 3.0f), 0.5f, false);
  world.Step(30);
  EXPECT_NEAR(hero.Position().y(), -2.0f, 0.1f);
  EXPECT_NEAR(hero.Position().x(), 0.0f, 1.0e-3f);
  /* Then it walks again. */
  world.Step(60);
  EXPECT_GT(hero.Position().x(), 0.8f);
  EXPECT_TRUE(character->OnGround());
}

TEST(JoltCharacter, AParentedCharacterFollowsItsObject)
{
  World world;
  KX_GameObject parent;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f, 3.0f);
  SG_Callbacks callbacks;
  SG_Node parentNode(&parent, nullptr, callbacks);
  SG_Node childNode(&hero.object, nullptr, callbacks);
  parent.SetSGNode(&parentNode);
  hero.object.SetSGNode(&childNode);
  parentNode.AddChild(&childNode);
  world.Step(30);
  /* Its parent holds it 3 m up: it does not fall. */
  EXPECT_NEAR(hero.Position().z(), 3.0f + kStand, 1.0e-4f);
  childNode.DisconnectFromParent();
  hero.object.SetSGNode(nullptr);
  parent.SetSGNode(nullptr);
  /* Unparented: it falls. */
  world.Step(90);
  EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);
}

TEST(JoltCharacter, ACopyGetsACharacterWithTheSourcesSettings)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  character->SetJumpSpeed(6.5f);
  character->SetMaxJumps(3);
  character->SetWalkDirection(MT_Vector3(1.0f, 0.0f, 0.0f));
  world.Step(5);

  KX_GameObject copyObject;
  KX_ClientObjectInfo copyInfo(&copyObject, KX_ClientObjectInfo::ACTOR);
  std::unique_ptr<JoltPhysicsController> copy(
      static_cast<JoltPhysicsController *>(hero.controller.GetReplica()));
  copy->SetNewClientInfo(&copyInfo);
  copyObject.SetPhysicsController(copy.get());
  /* Unlinked before the controller goes, also when an assertion ends the test early (the object
   * would delete the controller a second time). */
  struct Unlink {
    KX_GameObject &object;
    ~Unlink()
    {
      object.SetPhysicsController(nullptr);
    }
  } unlink{copyObject};
  JoltDefaultMotionState *motionState = new JoltDefaultMotionState();
  motionState->SetWorldPosition(MT_Vector3(0.0f, 4.0f, kStand));
  motionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  copy->PostProcessReplica(motionState, nullptr);

  JoltCharacter *copyCharacter = copy->GetCharacter();
  ASSERT_NE(copyCharacter, nullptr);
  EXPECT_NE(copyCharacter, character);
  EXPECT_EQ(world.environment.GetCharacterController(&copyObject), copyCharacter);
  EXPECT_FLOAT_EQ(copyCharacter->GetJumpSpeed(), 6.5f);
  EXPECT_EQ(copyCharacter->GetMaxJumps(), 3);
  /* None of its motion. */
  EXPECT_NEAR(copyCharacter->GetWalkDirection().length(), 0.0f, 0.0f);
  copyCharacter->SetWalkDirection(MT_Vector3(0.0f, 1.0f, 0.0f));
  world.Step(60);
  EXPECT_NEAR(motionState->GetWorldPosition().y(), 5.0f, 0.05f);

  /* Removing the copy's controller removes its character. */
  world.environment.RemoveController(copy.get());
  EXPECT_EQ(world.environment.GetCharacterController(&copyObject), nullptr);
  copy->SetNewClientInfo(nullptr);
}

/** A crowd walking into each other and into boxes: on Jolt's threads (16+ characters move at
 * once) exactly as one after another. */
/* -------------------------------------------------------------------- */
/** \name Pushes (2026-11-17): forces and impulses on a character
 * \{ */

/** Standing still, an impulse knocks it away: 350 / 70 kg = 5 m/s, which slides it about 0.54 m
 * and stops it in under half a second, on the ground all along. */
TEST(JoltCharacter, AnImpulseKnocksAStandingCharacterAwayAndItSlidesToAStop)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  world.Step(10);
  const MT_Vector3 start = hero.Position();
  hero.controller.ApplyWorldImpulse(MT_Vector3(350.0f, 0.0f, 0.0f), nullptr, false);
  world.Step();
  EXPECT_NEAR(character->GetLinearVelocity().x(), 5.0f, 0.05f);
  EXPECT_NEAR(hero.Position().x() - start.x(), 5.0f * kStep, 1.0e-3f);
  world.Step(40);
  const MT_Vector3 stopped = hero.Position();
  EXPECT_NEAR(stopped.x() - start.x(), 0.54f, 0.04f);
  EXPECT_NEAR(stopped.y(), start.y(), 1.0e-4f);
  EXPECT_NEAR(stopped.z(), start.z(), 1.0e-3f);
  EXPECT_LT(character->GetKnockback().length(), 1.0e-6f);
  EXPECT_TRUE(character->OnGround());
  world.Step(20);
  EXPECT_NEAR(hero.Position().x(), stopped.x(), 1.0e-5f);
}

/** An upward push launches it as a jump would (5 m/s: about 1.27 m up), and it lands. */
TEST(JoltCharacter, AnUpwardImpulseLaunchesItAsAJump)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  world.Step(10);
  const float ground = hero.Position().z();
  hero.controller.ApplyImpulse(MT_Vector3(0.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 350.0f), false);
  float peak = ground;
  for (int i = 0; i < 120; i++) {
    world.Step();
    peak = std::max(peak, float(hero.Position().z()));
  }
  EXPECT_NEAR(peak - ground, 5.0f * 5.0f / (2.0f * 9.81f), 0.08f);
  EXPECT_NEAR(hero.Position().z(), ground, 1.0e-3f);
  EXPECT_TRUE(character->OnGround());
  EXPECT_NEAR(hero.Position().x(), 0.0f, 1.0e-4f);
}

/** In the air it flies on (slow drag), and a wall stops it: the knock-back into the wall ends
 * there, where without the wall it would still be moving. */
TEST(JoltCharacter, KnockBackFliesOnInTheAirAndAWallStopsIt)
{
  {
    World world;
    TestObject &hero = world.AddCharacter(0.0f, 0.0f);
    JoltCharacter *character = World::CharacterOf(hero);
    world.Step(10);
    hero.controller.ApplyWorldImpulse(MT_Vector3(350.0f, 0.0f, 350.0f), nullptr, false);
    world.Step(18);
    EXPECT_FALSE(character->OnGround());
    EXPECT_NEAR(character->GetKnockback().x(),
                5.0f * std::pow(1.0f - JoltCharacter::kKnockbackAirDrag * kStep, 18.0f),
                0.02f);
  }
  for (const bool wall : {false, true}) {
    SCOPED_TRACE(wall ? "a wall at 0.9 m" : "no wall");
    World world;
    TestObject &hero = world.AddCharacter(0.0f, 0.0f);
    JoltCharacter *character = World::CharacterOf(hero);
    if (wall) {
      world.Add(JPH::EMotionType::Static, Box(0.1f, 2.0f, 1.0f), MT_Vector3(1.0f, 0.0f, 1.0f));
    }
    world.Step(10);
    hero.controller.ApplyWorldImpulse(MT_Vector3(700.0f, 0.0f, 0.0f), nullptr, false);
    world.Step(8);
    if (wall) {
      EXPECT_LT(character->GetKnockback().length(), 1.0e-6f);
      EXPECT_LT(hero.Position().x(), 0.61f);
      EXPECT_GT(hero.Position().x(), 0.55f);
    }
    else {
      EXPECT_GT(character->GetKnockback().x(), 2.5f);
      EXPECT_GT(hero.Position().x(), 0.8f);
    }
  }
}

/** A force pushes while it lasts (force / 70 kg each second; on the ground against its friction)
 * and in the object's own axes with Local; afterwards it slides to a stop. */
TEST(JoltCharacter, AForcePushesItWhileItLasts)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  TestObject &turned = world.AddCharacter(4.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  /* Turned 90 degrees around Z: its local X is the world's Y. */
  MT_Matrix3x3 quarter;
  quarter.setEuler(MT_Vector3(0.0f, 0.0f, float(M_PI_2)));
  turned.motion_state->SetWorldOrientation(quarter);
  turned.motion_state->Update();
  world.Step(10);
  const MT_Vector3 start = hero.Position();
  const MT_Vector3 turned_start = turned.Position();
  for (int i = 0; i < 30; i++) {
    hero.controller.ApplyForce(MT_Vector3(70.0f * 20.0f, 0.0f, 0.0f), false);
    turned.controller.ApplyForce(MT_Vector3(70.0f * 20.0f, 0.0f, 0.0f), true);
    world.Step();
  }
  /* 20 m/s2 against the ground's friction: it settles near (20 - 2) / 8 = 2.25 m/s. */
  EXPECT_NEAR(character->GetLinearVelocity().x(), 2.25f, 0.15f);
  EXPECT_GT(hero.Position().x() - start.x(), 0.6f);
  EXPECT_NEAR(turned.Position().x(), turned_start.x(), 1.0e-3f);
  EXPECT_NEAR(turned.Position().y() - turned_start.y(), hero.Position().x() - start.x(), 1.0e-3f);
  world.Step(40);
  EXPECT_LT(character->GetKnockback().length(), 1.0e-6f);
  const float stopped = hero.Position().x();
  world.Step(10);
  EXPECT_NEAR(hero.Position().x(), stopped, 1.0e-5f);
}

/** Ignore Mass: the impulse is the velocity change. A set velocity ends the knock-back. */
TEST(JoltCharacter, AVelocityChangeIgnoresItsMassAndASetVelocityEndsTheKnockBack)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  world.Step(10);
  hero.controller.ApplyWorldImpulse(MT_Vector3(0.0f, 3.0f, 0.0f), nullptr, true);
  world.Step();
  EXPECT_NEAR(character->GetLinearVelocity().y(), 3.0f, 0.03f);
  world.Step(2);
  EXPECT_GT(character->GetKnockback().y(), 1.0f);
  /* (Set Character Velocity, or Set Object Attribute's Linear Velocity.) */
  hero.controller.SetLinearVelocity(MT_Vector3(0.0f, 0.0f, 0.0f), false);
  EXPECT_LT(character->GetKnockback().length(), 1.0e-6f);
  world.Step();
  const float held = hero.Position().y();
  world.Step(20);
  EXPECT_NEAR(hero.Position().y(), held, 1.0e-5f);
  /* An impulse before a set velocity in the same step is replaced by it. */
  hero.controller.ApplyWorldImpulse(MT_Vector3(0.0f, 5.0f, 0.0f), nullptr, true);
  hero.controller.SetLinearVelocity(MT_Vector3(0.0f, 0.0f, 0.0f), false);
  world.Step(10);
  EXPECT_NEAR(hero.Position().y(), held, 1.0e-5f);
}

/** A character turns from nothing physical (torques, angular impulses, a set spin); its mass is
 * the 70 kg pushes move it with; pushes on a suspended character are dropped (none waits). */
TEST(JoltCharacter, TurnsMoveItNotAndItsMassIsWhatPushesMove)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  world.Step(10);
  const MT_Vector3 start = hero.Position();
  EXPECT_FLOAT_EQ(float(hero.controller.GetMass()), JoltCharacter::kMass);
  EXPECT_TRUE(hero.controller.IsCharacter());
  hero.controller.ApplyTorque(MT_Vector3(0.0f, 0.0f, 500.0f), false);
  hero.controller.ApplyAngularImpulse(MT_Vector3(0.0f, 0.0f, 50.0f), false);
  hero.controller.ApplyAngularImpulse(MT_Vector3(0.0f, 0.0f, 5.0f), true);
  hero.controller.SetAngularVelocity(MT_Vector3(0.0f, 0.0f, 5.0f), false);
  world.Step(20);
  const MT_Matrix3x3 orientation = hero.controller.GetOrientation();
  EXPECT_NEAR(orientation[0][0], 1.0f, 1.0e-5f);
  EXPECT_NEAR(orientation[1][1], 1.0f, 1.0e-5f);
  EXPECT_NEAR((hero.Position() - start).length(), 0.0f, 1.0e-4f);
  EXPECT_EQ(hero.controller.PushIgnoredReason(false), nullptr);
  ASSERT_NE(hero.controller.PushIgnoredReason(true), nullptr);
  EXPECT_NE(std::string(hero.controller.PushIgnoredReason(true)).find("Character"),
            std::string::npos);

  /* Suspended: a push meets nothing that moves and is dropped, not kept for later. */
  hero.controller.SuspendDynamics(false);
  hero.controller.ApplyWorldImpulse(MT_Vector3(350.0f, 0.0f, 0.0f), nullptr, false);
  world.Step(2);
  hero.controller.RestoreDynamics();
  world.Step(20);
  EXPECT_NEAR(hero.Position().x(), start.x(), 1.0e-3f);
  EXPECT_LT(character->GetKnockback().length(), 1.0e-6f);
}

/** Every body says why a push moves nothing (the Logic Nodes warning), and nothing when it
 * moves (or is suspended on purpose). */
TEST(JoltCharacter, EveryBodySaysWhyAPushMovesNothing)
{
  World world;
  TestObject &moving = world.Add(
      JPH::EMotionType::Dynamic, Box(0.3f, 0.3f, 0.3f), MT_Vector3(3.0f, 0.0f, 0.3f));
  TestObject &platform = world.Add(
      JPH::EMotionType::Kinematic, Box(1.0f, 1.0f, 0.1f), MT_Vector3(-3.0f, 0.0f, 2.0f));
  TestObject &zone = world.Add(
      JPH::EMotionType::Static, Box(1.0f, 1.0f, 1.0f), MT_Vector3(0.0f, 5.0f, 1.0f));
  zone.controller.SetSensorPhysicsType(true);
  const auto reason = [](TestObject &object, const bool angular) {
    const char *why = object.controller.PushIgnoredReason(angular);
    return std::string(why != nullptr ? why : "");
  };
  EXPECT_EQ(reason(world.floor, false), "it is Static");
  EXPECT_EQ(reason(platform, false), "it is Kinematic");
  EXPECT_EQ(reason(platform, true), "it is Kinematic");
  EXPECT_EQ(reason(zone, false), "it is a Sensor");
  /* Dynamic moves, but does not turn (Rigid Body does). */
  EXPECT_EQ(reason(moving, false), "");
  EXPECT_NE(reason(moving, true).find("Dynamic"), std::string::npos);
  moving.controller.SetRigidBodyFlag(true);
  EXPECT_EQ(reason(moving, true), "");
  /* Suspended on purpose: no warning. */
  moving.controller.SuspendDynamics(false);
  EXPECT_EQ(reason(moving, false), "");
  moving.controller.RestoreDynamics();
}

/** \} */

static std::vector<float> CrowdRun(const int helperThreads)
{
  World world(helperThreads);
  std::vector<TestObject *> crowd;
  for (int i = 0; i < 24; i++) {
    const float angle = float(i) * (2.0f * 3.14159265f / 24.0f);
    crowd.push_back(&world.AddCharacter(6.0f * std::cos(angle), 6.0f * std::sin(angle)));
  }
  std::vector<TestObject *> boxes;
  for (int i = 0; i < 6; i++) {
    boxes.push_back(&world.Add(JPH::EMotionType::Dynamic,
                               Box(0.3f, 0.3f, 0.3f),
                               MT_Vector3(-1.5f + 0.6f * float(i), 0.2f * float(i % 2), 0.3f)));
  }
  world.Step(5);
  for (int i = 0; i < 24; i++) {
    /* Everyone toward the middle. */
    const MT_Vector3 p = crowd[i]->Position();
    World::CharacterOf(*crowd[i])->SetWalkDirection(MT_Vector3(-p.x(), -p.y(), 0.0f) * 0.4f);
    if (i % 5 == 0) {
      World::CharacterOf(*crowd[i])->Jump();
    }
    /* (2026-11-17) Some knocked sideways and up, some pushed by a force. */
    if (i % 3 == 0) {
      crowd[i]->controller.ApplyWorldImpulse(
          MT_Vector3(p.y(), -p.x(), 2.0f) * 30.0f, nullptr, false);
    }
  }
  for (int step = 0; step < 150; step++) {
    if (step < 40) {
      for (int i = 1; i < 24; i += 4) {
        crowd[i]->controller.ApplyForce(MT_Vector3(400.0f, -200.0f, 0.0f), false);
      }
    }
    world.Step();
  }
  std::vector<float> result;
  for (TestObject *object : crowd) {
    const MT_Vector3 p = object->Position();
    result.insert(result.end(), {float(p.x()), float(p.y()), float(p.z())});
  }
  for (TestObject *box : boxes) {
    const JPH::RVec3 p = world.environment.GetBodyInterface().GetPosition(box->body_id);
    result.insert(result.end(), {float(p.GetX()), float(p.GetY()), float(p.GetZ())});
  }
  return result;
}

TEST(JoltCharacter, ACrowdOnJoltsThreadsEndsAsOneAfterAnother)
{
  const std::vector<float> alone = CrowdRun(0);
  const std::vector<float> threaded = CrowdRun(4);
  ASSERT_EQ(alone.size(), threaded.size());
  int differ = 0;
  for (size_t i = 0; i < alone.size(); i++) {
    if (std::bit_cast<uint32_t>(alone[i]) != std::bit_cast<uint32_t>(threaded[i])) {
      differ++;
    }
  }
  EXPECT_EQ(differ, 0);
  /* And they did move (toward the middle). */
  EXPECT_LT(std::abs(alone[0]), 5.0f);
}

/** Which box heights a character climbs, by Step Height (a 0.3 m round, 1.8 m capsule walking
 * at 2 m/s for 3 s into a box): run with --gtest_also_run_disabled_tests. */
TEST(JoltCharacter, DISABLED_StepHeightScan)
{
  const float steps[] = {0.0f, 0.1f, 0.25f, 0.4f};
  const float heights[] = {0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.35f, 0.4f, 0.45f, 0.55f, 0.9f};
  printf("SCAN step\\box");
  for (const float h : heights) {
    printf(" %5.2f", h);
  }
  printf("\n");
  for (const float step : steps) {
    printf("SCAN %9.2f", step);
    for (const float h : heights) {
      World world;
      world.Add(JPH::EMotionType::Static, Box(2.0f, 2.0f, h * 0.5f), MT_Vector3(3.0f, 0.0f, h * 0.5f));
      TestObject &hero = world.AddCharacter(0.0f, 0.0f, 0.0f, step);
      world.Step(10);
      World::CharacterOf(hero)->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
      world.Step(180);
      printf("     %s", hero.Position().x() > 1.5f ? "Y" : ".");
    }
    printf("\n");
  }
}

/** What characters cost a step: \a count walking in a grid (each its own lane, a wall of boxes
 * in their way), 120 steps, the whole step timed; 0 and 8 helper threads. Run with
 * --gtest_also_run_disabled_tests (UPBGE_CHARACTER_BENCH_COUNT to change 1000). */
TEST(JoltCharacter, DISABLED_CrowdCost)
{
  const char *countText = std::getenv("UPBGE_CHARACTER_BENCH_COUNT");
  const int count = countText ? std::atoi(countText) : 1000;
  for (const int helpers : {0, 8}) {
    World world(helpers);
    const int side = int(std::ceil(std::sqrt(float(count))));
    std::vector<TestObject *> crowd;
    for (int i = 0; i < count; i++) {
      crowd.push_back(&world.AddCharacter(-15.0f + 30.0f * float(i % side) / float(side),
                                          -15.0f + 30.0f * float(i / side) / float(side)));
    }
    for (int i = 0; i < 20; i++) {
      world.Add(JPH::EMotionType::Static,
                Box(0.3f, 0.3f, 0.5f),
                MT_Vector3(-15.0f + 1.5f * float(i), 16.0f, 0.5f));
    }
    world.Step(5);
    for (TestObject *object : crowd) {
      World::CharacterOf(*object)->SetWalkDirection(MT_Vector3(0.0f, 1.5f, 0.0f));
    }
    const auto start = std::chrono::steady_clock::now();
    world.Step(120);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count() /
                      120.0;
    double walked = 0.0;
    int moving = 0;
    for (TestObject *object : crowd) {
      walked += object->Position().y();
      moving += World::CharacterOf(*object)->SimulatedThisStep() ? 1 : 0;
    }
    printf("CROWD %d characters, %d helpers: %.3f ms a step (%.2f us a character); %d moved "
           "themselves, mean y %.2f\n",
           count,
           helpers,
           ms,
           ms * 1000.0 / double(count),
           moving,
           walked / double(count));
  }
}

/* -------------------------------------------------------------------- */
/** \name What it touches, its settings, crouching (2026-11-17: Get Character Info's On Wall, On
 * Ceiling, Ground Normal / Velocity / Object, Wall Normal; Set Character Attribute's Max Slope,
 * Fall Speed, Step Height, Crouch)
 * \{ */

namespace {

/** The bottom and top (Blender Z, Jolt's Y) of \a shape in its own space (Jolt's local bounds
 * are around the center of mass). */
void ShapeHeight(const JPH::Shape &shape, float &r_bottom, float &r_top)
{
  const JPH::AABox bounds = shape.GetLocalBounds();
  const float center = shape.GetCenterOfMass().GetY();
  r_bottom = bounds.mMin.GetY() + center;
  r_top = bounds.mMax.GetY() + center;
}

float BodyShapeHeight(World &world, TestObject &object)
{
  float bottom, top;
  ShapeHeight(*world.environment.GetBodyInterface().GetShape(object.body_id), bottom, top);
  return top - bottom;
}

float CharacterShapeHeight(TestObject &object)
{
  float bottom, top;
  ShapeHeight(*World::CharacterOf(object)->GetCharacterVirtual()->GetShape(), bottom, top);
  return top - bottom;
}

/** A static box turned \a angle (radians) around Blender's Y: its top faces up and \a angle
 * toward +X. Not a TestObject (no controller): only its body. */
JPH::BodyID AddSlope(World &world, const MT_Vector3 &position, const float angle)
{
  MT_Matrix3x3 turn;
  turn.setEuler(MT_Vector3(0.0f, angle, 0.0f));
  JPH::BodyCreationSettings settings(Box(12.0f, 6.0f, 0.1f),
                                     JoltMath::ToJolt(position),
                                     JoltMath::ToJolt(turn.getRotation()),
                                     JPH::EMotionType::Static,
                                     JoltMakeObjectLayer(1, 1, JOLT_BP_STATIC));
  return world.environment.GetBodyInterface().CreateAndAddBody(settings,
                                                               JPH::EActivation::DontActivate);
}

}  // namespace

/** Away from walls it touches nothing but the floor; walked into a wall it is On Wall, its Wall
 * Normal pointing away from it; stepping back ends it. The floor is its ground all along. */
TEST(JoltCharacter, AWallItTouchesIsOnWallWithItsNormal)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  /* A wall whose face is at x = 1.5. */
  world.Add(JPH::EMotionType::Static, Box(0.2f, 3.0f, 1.5f), MT_Vector3(1.7f, 0.0f, 1.5f));
  world.Step(10);
  EXPECT_TRUE(character->OnGround());
  EXPECT_FALSE(character->OnWall());
  EXPECT_FALSE(character->OnCeiling());
  EXPECT_EQ(character->GetWallNormal(), MT_Vector3(0.0f, 0.0f, 0.0f));

  character->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
  world.Step(60);
  EXPECT_NEAR(hero.Position().x(), 1.5f - 0.3f, 0.03f);
  EXPECT_TRUE(character->OnWall());
  EXPECT_TRUE(character->OnGround());
  EXPECT_FALSE(character->OnCeiling());
  const MT_Vector3 away = character->GetWallNormal();
  EXPECT_NEAR(away.x(), -1.0f, 1.0e-4f);
  EXPECT_NEAR(away.y(), 0.0f, 1.0e-4f);
  EXPECT_NEAR(away.z(), 0.0f, 1.0e-4f);
  /* Standing still against it: still touching. */
  character->SetWalkDirection(MT_Vector3(0.0f, 0.0f, 0.0f));
  world.Step(10);
  EXPECT_TRUE(character->OnWall());

  /* A corner (a second wall at y = 0.8 facing -Y): the two normals' average. */
  world.Add(JPH::EMotionType::Static, Box(3.0f, 0.2f, 1.5f), MT_Vector3(0.0f, 1.0f, 1.5f));
  character->SetWalkDirection(MT_Vector3(1.0f, 1.0f, 0.0f));
  world.Step(60);
  const MT_Vector3 corner = character->GetWallNormal();
  EXPECT_NEAR(corner.x(), -std::sqrt(0.5f), 1.0e-3f);
  EXPECT_NEAR(corner.y(), -std::sqrt(0.5f), 1.0e-3f);

  character->SetWalkDirection(MT_Vector3(-2.0f, -2.0f, 0.0f));
  world.Step(30);
  EXPECT_FALSE(character->OnWall());
  EXPECT_EQ(character->GetWallNormal(), MT_Vector3(0.0f, 0.0f, 0.0f));
}

/** The Physics panel's Max Slope of 90 degrees (every new object's) and Jolt's "no limit" of 0
 * call a vertical wall walkable: it is still a wall (kWallFlatness). */
TEST(JoltCharacter, AVerticalWallIsAWallWhateverMaxSlopeSays)
{
  for (const float max_slope : {float(M_PI_2), 0.0f}) {
    SCOPED_TRACE(max_slope);
    World world;
    TestObject &hero = world.AddCharacter(0.0f, 0.0f);
    JoltCharacter *character = World::CharacterOf(hero);
    character->SetMaxSlope(max_slope);
    world.Add(JPH::EMotionType::Static, Box(0.2f, 3.0f, 1.5f), MT_Vector3(1.7f, 0.0f, 1.5f));
    world.Step(10);
    character->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
    world.Step(60);
    EXPECT_TRUE(character->OnWall());
    EXPECT_FALSE(character->OnCeiling());
    EXPECT_NEAR(character->GetWallNormal().x(), -1.0f, 1.0e-4f);
    EXPECT_TRUE(character->OnGround());
  }
}

/** A jump into a ceiling: On Ceiling while it touches it, not a wall; not once landed. */
TEST(JoltCharacter, ACeilingItBumpsIsOnCeiling)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  world.Add(JPH::EMotionType::Static, Box(3.0f, 3.0f, 0.1f), MT_Vector3(0.0f, 0.0f, 2.4f));
  JoltCharacter *character = World::CharacterOf(hero);
  character->SetJumpSpeed(8.0f);
  world.Step(10);
  EXPECT_FALSE(character->OnCeiling());
  character->Jump();
  int ceiling_steps = 0;
  for (int step = 0; step < 12; step++) {
    world.Step(1);
    ceiling_steps += character->OnCeiling() ? 1 : 0;
    EXPECT_FALSE(character->OnWall());
  }
  EXPECT_GE(ceiling_steps, 1);
  world.Step(40);
  EXPECT_TRUE(character->OnGround());
  EXPECT_FALSE(character->OnCeiling());
}

/** On the floor its ground is the floor (up, still); on a moving platform the platform carries it
 * (Ground Velocity, Ground Object); in the air nothing. */
TEST(JoltCharacter, ItsGroundIsWhatItStandsOn)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  world.Step(10);
  EXPECT_EQ(character->GetGroundObject(), &world.floor.object);
  EXPECT_NEAR(character->GetGroundNormal().z(), 1.0f, 1.0e-5f);
  EXPECT_NEAR(character->GetGroundVelocity().length(), 0.0f, 1.0e-5f);

  /* A Kinematic platform at (5, 0), its top at z = 0.2, moving +Y at 1.5 m/s. */
  TestObject &platform = world.Add(
      JPH::EMotionType::Kinematic, Box(1.5f, 1.5f, 0.1f), MT_Vector3(5.0f, 0.0f, 0.1f));
  hero.motion_state->Teleport(MT_Vector3(5.0f, 0.0f, 0.2f + kStand + 0.05f));
  for (int step = 0; step < 30; step++) {
    const MT_Vector3 at = platform.Position();
    platform.motion_state->Teleport(at + MT_Vector3(0.0f, 1.5f * kStep, 0.0f));
    world.Step(1);
  }
  EXPECT_TRUE(character->OnGround());
  EXPECT_EQ(character->GetGroundObject(), &platform.object);
  EXPECT_NEAR(character->GetGroundVelocity().y(), 1.5f, 0.02f);
  EXPECT_NEAR(character->GetGroundNormal().z(), 1.0f, 1.0e-4f);
  /* It rode along. */
  EXPECT_GT(hero.Position().y(), 0.5f);

  /* In the air: none. */
  character->SetJumpSpeed(6.0f);
  character->Jump();
  world.Step(5);
  EXPECT_FALSE(character->OnGround());
  EXPECT_EQ(character->GetGroundObject(), nullptr);
  EXPECT_EQ(character->GetGroundNormal(), MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(character->GetGroundVelocity(), MT_Vector3(0.0f, 0.0f, 0.0f));
}

/** A 60 degree slope: a wall for Max Slope 45 (it slides down it, On Wall, not On Ground), ground
 * for Max Slope 70 (it stands on it). */
TEST(JoltCharacter, ASlopeSteeperThanMaxSlopeIsAWall)
{
  for (const float max_slope_degrees : {45.0f, 70.0f}) {
    SCOPED_TRACE(max_slope_degrees);
    World world;
    AddSlope(world, MT_Vector3(0.0f, 0.0f, 10.0f), JPH::DegreesToRadians(60.0f));
    TestObject &hero = world.AddCharacter(0.0f, 0.0f, 12.0f);
    JoltCharacter *character = World::CharacterOf(hero);
    character->SetMaxSlope(JPH::DegreesToRadians(max_slope_degrees));
    EXPECT_NEAR(character->GetMaxSlope(), JPH::DegreesToRadians(max_slope_degrees), 1.0e-6f);
    /* Falls onto it. */
    int touching = 0;
    for (int step = 0; step < 90 && touching < 10; step++) {
      world.Step(1);
      touching += character->OnWall() || character->OnGround() ? 1 : 0;
    }
    ASSERT_GE(touching, 10);
    if (max_slope_degrees < 60.0f) {
      EXPECT_TRUE(character->OnWall());
      EXPECT_FALSE(character->OnGround());
      /* Away from the slope: its face's normal, 60 degrees from up. */
      const MT_Vector3 away = character->GetWallNormal();
      EXPECT_NEAR(std::abs(away.x()), std::sin(JPH::DegreesToRadians(60.0f)), 0.02f);
      EXPECT_NEAR(away.y(), 0.0f, 0.02f);
      EXPECT_NEAR(away.z(), std::cos(JPH::DegreesToRadians(60.0f)), 0.02f);
      /* No ground: the slope it slides down is no ground (Jolt's steep ground). */
      EXPECT_EQ(character->GetGroundObject(), nullptr);
      EXPECT_EQ(character->GetGroundNormal(), MT_Vector3(0.0f, 0.0f, 0.0f));
      EXPECT_EQ(character->GetGroundVelocity(), MT_Vector3(0.0f, 0.0f, 0.0f));
    }
    else {
      EXPECT_FALSE(character->OnWall());
      EXPECT_TRUE(character->OnGround());
      EXPECT_NEAR(character->GetGroundNormal().z(), 0.5f, 0.02f);
    }
  }
}

/** Step Height set while playing: 0 stops it at a 0.2 m step, 0.3 walks up it. */
TEST(JoltCharacter, StepHeightIsTheHighestStepItWalksUp)
{
  for (const float step_height : {0.0f, 0.3f}) {
    SCOPED_TRACE(step_height);
    World world;
    world.Add(JPH::EMotionType::Static, Box(2.0f, 2.0f, 0.1f), MT_Vector3(3.0f, 0.0f, 0.1f));
    TestObject &hero = world.AddCharacter(0.0f, 0.0f, 0.0f, 0.15f);
    JoltCharacter *character = World::CharacterOf(hero);
    character->SetStepHeight(step_height);
    EXPECT_FLOAT_EQ(character->GetStepHeight(), step_height);
    world.Step(10);
    character->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
    world.Step(120);
    if (step_height > 0.0f) {
      EXPECT_GT(hero.Position().x(), 2.0f);
      EXPECT_NEAR(hero.Position().z(), 0.2f + kStand, 0.03f);
    }
    else {
      EXPECT_LT(hero.Position().x(), 1.0f);
      EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);
    }
  }
  /* (A negative one is none; a copy keeps it.) */
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  World::CharacterOf(hero)->SetStepHeight(-1.0f);
  EXPECT_FLOAT_EQ(World::CharacterOf(hero)->GetStepHeight(), 0.0f);
}

/** Crouched: its shape is half as tall, its feet where they were, its body the same shape; it
 * walks under a bar it stood too tall for; standing up waits for room under the bar and comes as
 * soon as it walked out. */
TEST(JoltCharacter, CrouchesUnderABarAndStandsUpOnlyWhereThereIsRoom)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  /* A bar from x = 2 to 4 whose bottom is 1.2 m above the floor. */
  world.Add(JPH::EMotionType::Static, Box(1.0f, 3.0f, 0.2f), MT_Vector3(3.0f, 0.0f, 1.4f));
  world.Step(10);
  EXPECT_FALSE(character->IsCrouched());
  EXPECT_NEAR(CharacterShapeHeight(hero), 1.8f, 1.0e-4f);

  /* Standing, the bar stops it. */
  character->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
  world.Step(90);
  EXPECT_LT(hero.Position().x(), 2.0f - 0.25f);

  /* Crouched: under it. */
  character->SetCrouch(true, 0.5f);
  world.Step(1);
  EXPECT_TRUE(character->IsCrouched());
  EXPECT_NEAR(CharacterShapeHeight(hero), 0.9f, 1.0e-4f);
  EXPECT_NEAR(BodyShapeHeight(world, hero), 0.9f, 1.0e-4f);
  float bottom, top;
  ShapeHeight(*character->GetCharacterVirtual()->GetShape(), bottom, top);
  EXPECT_NEAR(bottom, -kStand, 1.0e-4f);
  world.Step(30);
  EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);
  EXPECT_TRUE(character->OnGround());
  EXPECT_GT(hero.Position().x(), 2.2f);

  /* Under the bar: standing up is asked, but there is no room. */
  character->SetWalkDirection(MT_Vector3(0.0f, 0.0f, 0.0f));
  world.Step(5);
  ASSERT_LT(hero.Position().x(), 3.5f);
  character->SetCrouch(false, 0.5f);
  world.Step(10);
  EXPECT_TRUE(character->IsCrouched());
  EXPECT_NEAR(CharacterShapeHeight(hero), 0.9f, 1.0e-4f);
  EXPECT_NEAR(BodyShapeHeight(world, hero), 0.9f, 1.0e-4f);
  /* (It did not climb onto or into the bar.) */
  EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);

  /* Walked out: it stands at once. */
  character->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
  world.Step(90);
  EXPECT_GT(hero.Position().x(), 4.5f);
  EXPECT_FALSE(character->IsCrouched());
  EXPECT_NEAR(CharacterShapeHeight(hero), 1.8f, 1.0e-4f);
  EXPECT_NEAR(BodyShapeHeight(world, hero), 1.8f, 1.0e-4f);
  EXPECT_EQ(world.environment.GetBodyInterface().GetShape(hero.body_id),
            hero.controller.GetShape());
  EXPECT_NEAR(hero.Position().z(), kStand, 0.03f);

  /* Height 1: crouched without a shorter shape. */
  character->SetCrouch(true, 1.0f);
  world.Step(1);
  EXPECT_TRUE(character->IsCrouched());
  EXPECT_NEAR(CharacterShapeHeight(hero), 1.8f, 1.0e-4f);
  EXPECT_FLOAT_EQ(character->GetCrouchHeight(), 1.0f);
  character->SetCrouch(false, 0.0f);
  world.Step(1);
  EXPECT_FALSE(character->IsCrouched());
  /* (The height is held to kMinCrouchHeight..1.) */
  EXPECT_FLOAT_EQ(character->GetCrouchHeight(), JoltCharacter::kMinCrouchHeight);
}

/** Sweep Object of a Character casts the shape it has now: standing it meets a bar 1.2 m up,
 * crouched it passes under (SweepBody takes the CharacterVirtual's shape, not the controller's
 * standing one). */
TEST(JoltCharacter, ASweepOfACrouchedCharacterCastsItsShorterShape)
{
  class AcceptAll : public PHY_IShapeCastFilterCallback {
   public:
    AcceptAll() : PHY_IShapeCastFilterCallback(nullptr) {}
  };
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  TestObject &bar = world.Add(
      JPH::EMotionType::Static, Box(1.0f, 3.0f, 0.2f), MT_Vector3(3.0f, 0.0f, 1.4f));
  JoltCharacter *character = World::CharacterOf(hero);
  world.Step(10);
  const auto meets_bar = [&]() {
    AcceptAll filter;
    std::vector<PHY_ShapeCastResult> results;
    PHY_BodySweepSettings settings;
    settings.controller = &hero.controller;
    settings.position = hero.Position();
    settings.destination = hero.Position() + MT_Vector3(5.0f, 0.0f, 0.0f);
    settings.max_results = 8;
    EXPECT_EQ(world.environment.SweepBody(settings, filter, results), PHY_BodySweepStatus::Ok);
    return std::any_of(results.begin(), results.end(), [&](const PHY_ShapeCastResult &result) {
      return result.controller == &bar.controller;
    });
  };
  EXPECT_TRUE(meets_bar());
  character->SetCrouch(true, 0.5f);
  world.Step(1);
  ASSERT_TRUE(character->IsCrouched());
  EXPECT_FALSE(meets_bar());
  character->SetCrouch(false, 0.5f);
  world.Step(1);
  EXPECT_TRUE(meets_bar());
}

/** A copy of a crouched character stands, and its body (made from its source body's settings,
 * which held the crouched shape) gets the standing shape at its first step. */
TEST(JoltCharacter, ACopyOfACrouchedCharacterStands)
{
  World world;
  TestObject &hero = world.AddCharacter(0.0f, 0.0f);
  JoltCharacter *character = World::CharacterOf(hero);
  character->SetCrouch(true, 0.5f);
  world.Step(5);
  ASSERT_TRUE(character->IsCrouched());

  KX_GameObject copyObject;
  KX_ClientObjectInfo copyInfo(&copyObject, KX_ClientObjectInfo::ACTOR);
  std::unique_ptr<JoltPhysicsController> copy(
      static_cast<JoltPhysicsController *>(hero.controller.GetReplica()));
  copy->SetNewClientInfo(&copyInfo);
  copyObject.SetPhysicsController(copy.get());
  struct Unlink {
    KX_GameObject &object;
    ~Unlink()
    {
      object.SetPhysicsController(nullptr);
    }
  } unlink{copyObject};
  JoltDefaultMotionState *motionState = new JoltDefaultMotionState();
  motionState->SetWorldPosition(MT_Vector3(0.0f, 4.0f, kStand));
  motionState->SetWorldOrientation(MT_Matrix3x3::Identity());
  copy->PostProcessReplica(motionState, nullptr);
  JoltCharacter *copyCharacter = copy->GetCharacter();
  ASSERT_NE(copyCharacter, nullptr);
  EXPECT_FLOAT_EQ(copyCharacter->GetCrouchHeight(), 0.5f);
  world.Step(1);
  EXPECT_FALSE(copyCharacter->IsCrouched());
  float bottom, top;
  ShapeHeight(*world.environment.GetBodyInterface().GetShape(copy->GetBodyID()), bottom, top);
  EXPECT_NEAR(top - bottom, 1.8f, 1.0e-4f);
  EXPECT_TRUE(character->IsCrouched());

  world.environment.RemoveController(copy.get());
  copy->SetNewClientInfo(nullptr);
}

/** The crouched shape of each kind of shape: a capsule keeps its radius (at least a sphere tall),
 * a sphere becomes a smaller one, a box is scaled along up; the bottom stays. */
TEST(JoltCharacter, CrouchedShapesKeepTheirBottom)
{
  const auto check = [](const JPH::Shape &standing,
                        const float height,
                        const float expected_top,
                        const float expected_half_width) {
    const JPH::RefConst<JPH::Shape> crouched = JoltCharacter::CrouchedShapeOf(standing, height);
    float bottom, top, standing_bottom, standing_top;
    ShapeHeight(standing, standing_bottom, standing_top);
    ShapeHeight(*crouched, bottom, top);
    EXPECT_NEAR(bottom, standing_bottom, 1.0e-5f);
    EXPECT_NEAR(top, expected_top, 1.0e-4f);
    EXPECT_NEAR(crouched->GetLocalBounds().mMax.GetX(), expected_half_width, 1.0e-5f);
  };
  const JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(0.6f, 0.3f);
  check(*capsule, 0.5f, 0.0f, 0.3f);
  /* Lower than its width: a sphere of its radius (and a hair). */
  check(*capsule, 0.2f, -0.9f + 0.6f + 0.002f, 0.3f);
  const JPH::RefConst<JPH::Shape> scaled = new JPH::ScaledShape(capsule, JPH::Vec3::sReplicate(2.0f));
  check(*scaled, 0.5f, 0.0f, 0.6f);
  const JPH::RefConst<JPH::Shape> sphere = new JPH::SphereShape(0.5f);
  check(*sphere, 0.5f, 0.0f, 0.25f);
  const JPH::RefConst<JPH::Shape> box = Box(0.4f, 0.4f, 1.0f);
  check(*box, 0.25f, -0.5f, 0.4f);
  /* Height 1 (and more): the standing shape itself. */
  EXPECT_EQ(JoltCharacter::CrouchedShapeOf(*capsule, 1.0f).GetPtr(), capsule.GetPtr());
  EXPECT_EQ(JoltCharacter::CrouchedShapeOf(*capsule, 7.0f).GetPtr(), capsule.GetPtr());
}

/** \} */

/* Navigation made easy, item 12 (2026-11-20): a Character moved by the navigation passes
 * through the bodies of the other navigating NPCs physics cannot push (SetNavigatingBodies);
 * whatever is not navigating (a player steered by keys, an NPC standing) is met as before.
 * A walks at 2 m/s for 2 s from x -2 into B standing at x 0 (or a Kinematic box there). */
TEST(JoltCharacter, NavigatingNpcsPassThroughEachOther)
{
  struct Case {
    const char *name;
    bool a_navigates, b_navigates, b_box;
    bool passes;
  };
  for (const Case &c : {Case{"both navigate", true, true, false, true},
                        Case{"B stands", true, false, false, false},
                        Case{"A steered by keys", false, true, false, false},
                        Case{"neither", false, false, false, false},
                        Case{"through a navigating Kinematic NPC", true, true, true, true},
                        Case{"a Kinematic NPC standing", true, false, true, false}})
  {
    SCOPED_TRACE(c.name);
    World world;
    TestObject &a = world.AddCharacter(-2.0f, 0.0f);
    TestObject &b = c.b_box ? world.Add(JPH::EMotionType::Kinematic,
                                        Box(0.3f, 0.3f, 0.9f),
                                        MT_Vector3(0.0f, 0.0f, kStand)) :
                              world.AddCharacter(0.0f, 0.0f);
    std::vector<PHY_IPhysicsController *> navigating;
    if (c.a_navigates) {
      navigating.push_back(&a.controller);
    }
    if (c.b_navigates) {
      navigating.push_back(&b.controller);
    }
    world.environment.SetNavigatingBodies(navigating);
    world.Step(10);
    World::CharacterOf(a)->SetWalkDirection(MT_Vector3(2.0f, 0.0f, 0.0f));
    world.Step(120);
    if (c.passes) {
      EXPECT_GT(a.Position().x(), 1.5f);
    }
    else {
      EXPECT_LT(a.Position().x(), -0.5f);
    }
    /* B is where it stood (a standing Character pressed on gives a centimetre or so). */
    EXPECT_NEAR(b.Position().x(), 0.0f, 0.05f);
  }
}

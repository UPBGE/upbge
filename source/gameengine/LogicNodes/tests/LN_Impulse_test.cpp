/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Apply Impulse's Point Mode / Local / Ignore Mass, Apply Angular Impulse and Explosion Impulse
 * (2026-11-11): what the flush hands the physics body, in world terms, seen through
 * LN_SetImpulseObserverForTests (the rig has no physics: the pose is the object's, its center of
 * mass its position). The overlap of Explosion Impulse needs Jolt: the game probe
 * ~/Desktop/CPP_LN_TESTS/impulse_nodes/ carries it.
 */

#include "testing/testing.h"

#include <cmath>
#include <string>
#include <vector>

#include "LN_CommandBuffer.h"
#include "LN_SemRig.h"
#include "LN_StrictDump.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

struct ImpulseRecord {
  std::string object;
  MT_Vector3 vector;
  MT_Vector3 point;
  bool at_point = false;
  bool angular = false;
  bool velocity_change = false;
};

class ImpulseObserver {
 public:
  ImpulseObserver()
  {
    LN_SetImpulseObserverForTests(&ImpulseObserver::Note, this);
  }
  ~ImpulseObserver()
  {
    LN_SetImpulseObserverForTests(nullptr, nullptr);
  }
  std::vector<ImpulseRecord> records;

  std::vector<ImpulseRecord> Of(const std::string &name) const
  {
    std::vector<ImpulseRecord> found;
    for (const ImpulseRecord &record : records) {
      if (record.object == name) {
        found.push_back(record);
      }
    }
    return found;
  }

 private:
  static void Note(void *user,
                   const KX_GameObject *object,
                   const MT_Vector3 &vector,
                   const MT_Vector3 &point,
                   const bool at_point,
                   const bool angular,
                   const bool velocity_change)
  {
    ImpulseObserver &self = *static_cast<ImpulseObserver *>(user);
    ImpulseRecord record;
    record.object = object != nullptr ? const_cast<KX_GameObject *>(object)->GetName() : "";
    record.vector = vector;
    record.point = point;
    record.at_point = at_point;
    record.angular = angular;
    record.velocity_change = velocity_change;
    self.records.push_back(record);
  }
};

void ExpectVector(const MT_Vector3 &actual, const MT_Vector3 &expected, const char *what)
{
  SCOPED_TRACE(what);
  EXPECT_NEAR(actual.x(), expected.x(), 1e-5f);
  EXPECT_NEAR(actual.y(), expected.y(), 1e-5f);
  EXPECT_NEAR(actual.z(), expected.z(), 1e-5f);
}

/** A quarter turn about Z: X becomes Y. */
const MT_Matrix3x3 kQuarterTurn(0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);

}  // namespace

TEST(LN_Impulse, EachPointModeGivesItsWorldPush)
{
  ImpulseObserver observer;
  PositionedGameObject car(MT_Vector3(1.0f, 2.0f, 3.0f));
  car.object.NodeSetGlobalOrientation(kQuarterTurn);
  car.object.NodeUpdateGS(0.0);

  const MT_Vector3 back(-1.5f, 0.0f, 0.0f);
  const MT_Vector3 forward(3.0f, 0.0f, 0.0f);
  LN_CommandBuffer buffer;
  buffer.BeginRecording();
  /* The old command (nodes made before 2026-11-11): the offset along the world axes. */
  buffer.AppendApplyImpulse(&car.object, back, forward, 10, 0);
  buffer.AppendApplyImpulse(
      &car.object, back, forward, LN_ImpulseMode::WorldOffset, true, false, false, 20, 0);
  buffer.AppendApplyImpulse(
      &car.object, back, forward, LN_ImpulseMode::LocalPoint, false, false, false, 30, 0);
  buffer.AppendApplyImpulse(&car.object,
                            MT_Vector3(5.0f, 6.0f, 7.0f),
                            forward,
                            LN_ImpulseMode::WorldPoint,
                            false,
                            true,
                            false,
                            40,
                            0);
  buffer.AppendApplyImpulse(&car.object,
                            MT_Vector3(9.0f, 9.0f, 9.0f),
                            MT_Vector3(1.0f, 0.0f, 2.0f),
                            LN_ImpulseMode::Angular,
                            true,
                            true,
                            false,
                            50,
                            0);
  buffer.EndRecording();
  buffer.Flush();

  ASSERT_EQ(observer.records.size(), 5u);
  const std::vector<ImpulseRecord> &r = observer.records;
  /* World Offset: the back of the car is still the world's -X side after its quarter turn. */
  EXPECT_TRUE(r[0].at_point);
  ExpectVector(r[0].point, MT_Vector3(-0.5f, 2.0f, 3.0f), "old command's point");
  ExpectVector(r[0].vector, forward, "old command's impulse");
  EXPECT_FALSE(r[0].velocity_change);
  /* Local turns the impulse with the car, not the World Offset point. */
  ExpectVector(r[1].point, MT_Vector3(-0.5f, 2.0f, 3.0f), "world offset point");
  ExpectVector(r[1].vector, MT_Vector3(0.0f, 3.0f, 0.0f), "local impulse");
  /* Local Point: the back of the car turns with it. */
  EXPECT_TRUE(r[2].at_point);
  ExpectVector(r[2].point, MT_Vector3(1.0f, 0.5f, 3.0f), "local point");
  ExpectVector(r[2].vector, forward, "world impulse");
  /* World Point: where it says; Ignore Mass is a velocity change. */
  ExpectVector(r[3].point, MT_Vector3(5.0f, 6.0f, 7.0f), "world point");
  EXPECT_TRUE(r[3].velocity_change);
  /* Angular: no point, the vector turned by Local. */
  EXPECT_TRUE(r[4].angular);
  EXPECT_FALSE(r[4].at_point);
  ExpectVector(r[4].vector, MT_Vector3(0.0f, 1.0f, 2.0f), "local angular impulse");
  EXPECT_TRUE(r[4].velocity_change);
}

TEST(LN_Impulse, ExplosionPushesAwayOrPullsInWithItsFalloff)
{
  ImpulseObserver observer;
  PositionedGameObject near(MT_Vector3(3.0f, 0.0f, 0.0f));
  PositionedGameObject side(MT_Vector3(0.0f, 4.0f, 0.0f));
  PositionedGameObject center(MT_Vector3(0.0f, 0.0f, 0.0f));
  PositionedGameObject beyond(MT_Vector3(6.0f, 0.0f, 0.0f));
  PositionedGameObject lifted(MT_Vector3(4.0f, 0.0f, 0.0f));
  near.object.SetName("near");
  side.object.SetName("side");
  center.object.SetName("center");
  beyond.object.SetName("beyond");
  lifted.object.SetName("lifted");

  const MT_Vector3 origin(0.0f, 0.0f, 0.0f);
  LN_CommandBuffer buffer;
  buffer.BeginRecording();
  uint64_t key = 10;
  /* (object, strength, lift, linear, implode, ignore mass) */
  const auto blast = [&](PositionedGameObject &object,
                         const float strength,
                         const float lift,
                         const bool linear,
                         const bool implode,
                         const bool ignore_mass) {
    buffer.AppendExplosionImpulse(&object.object,
                                  origin,
                                  5.0f,
                                  strength,
                                  lift,
                                  linear,
                                  implode,
                                  ignore_mass,
                                  false,
                                  key++,
                                  0);
  };
  for (PositionedGameObject *object : {&near, &side, &center, &beyond}) {
    /* Linear: 1 at the origin, 0 at the radius. */
    blast(*object, 10.0f, 0.0f, true, false, false);
  }
  /* Constant: full strength, also for an object reached whose center is past the radius. */
  blast(near, 10.0f, 0.0f, false, false, true);
  blast(beyond, 10.0f, 0.0f, false, false, false);
  /* Upward Lift 3: aimed from (0, 0, -3), so (4, 0, 0) goes along (4, 0, 3) / 5. */
  blast(lifted, 10.0f, 3.0f, true, false, false);
  /* A negative strength reverses: pulls in. */
  blast(near, -10.0f, 0.0f, false, false, false);
  /* Implode: toward the center; Upward Lift 3 pulls toward (0, 0, 3), so (4, 0, 0) goes along
   * (-4, 0, 3) / 5; nothing at the very center; a negative strength pushes out. */
  blast(near, 10.0f, 0.0f, true, true, false);
  blast(lifted, 10.0f, 3.0f, true, true, false);
  blast(center, 10.0f, 0.0f, false, true, false);
  blast(near, -10.0f, 0.0f, false, true, false);
  buffer.EndRecording();
  buffer.Flush();

  ASSERT_EQ(observer.records.size(), 12u);
  const std::vector<ImpulseRecord> &r = observer.records;
  for (const ImpulseRecord &record : r) {
    EXPECT_FALSE(record.at_point) << record.object;
    EXPECT_FALSE(record.angular) << record.object;
  }
  ExpectVector(r[0].vector, MT_Vector3(4.0f, 0.0f, 0.0f), "near, linear");
  ExpectVector(r[0].point, MT_Vector3(3.0f, 0.0f, 0.0f), "near's center of mass");
  ExpectVector(r[1].vector, MT_Vector3(0.0f, 2.0f, 0.0f), "side, linear");
  ExpectVector(r[2].vector, MT_Vector3(0.0f, 0.0f, 10.0f), "at the origin: straight up");
  ExpectVector(r[3].vector, MT_Vector3(0.0f, 0.0f, 0.0f), "past the radius, linear");
  ExpectVector(r[4].vector, MT_Vector3(10.0f, 0.0f, 0.0f), "near, constant");
  EXPECT_TRUE(r[4].velocity_change);
  EXPECT_FALSE(r[0].velocity_change);
  ExpectVector(r[5].vector, MT_Vector3(10.0f, 0.0f, 0.0f), "past the radius, constant");
  ExpectVector(r[6].vector, MT_Vector3(1.6f, 0.0f, 1.2f), "lifted");
  ExpectVector(r[7].vector, MT_Vector3(-10.0f, 0.0f, 0.0f), "pulled in");
  ExpectVector(r[8].vector, MT_Vector3(-4.0f, 0.0f, 0.0f), "imploded, linear");
  ExpectVector(r[9].vector, MT_Vector3(-1.6f, 0.0f, 1.2f), "imploded, lifted");
  ExpectVector(r[10].vector, MT_Vector3(0.0f, 0.0f, 0.0f), "imploded at the very center");
  ExpectVector(r[11].vector, MT_Vector3(10.0f, 0.0f, 0.0f), "imploded with a negative strength");
}

TEST(LN_Impulse, TheNodesRecordWhatTheirSettingsSay)
{
  TreeBuilder b;
  b.SetRealSockets(true); /* Explosion Impulse's Mask is a Collision Layers socket */
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &push = b.Add("LogicNativeApplyImpulse", 2 /* Local Point */);
  b.Vector(push, "Attach", -1.5f, 0.0f, 0.0f);
  b.Vector(push, "Impulse", 3.0f, 0.0f, 0.0f);
  b.Bool(push, "Local", true);
  GenNode &heavy = b.Add("LogicNativeGetGamePropertyBool");
  b.String(heavy, "Property", "pb0");
  b.Link(heavy, "Value", push, "Ignore Mass");
  b.Link(step, "Out", push, "Flow");
  GenNode &spin = b.Add("LogicNativeApplyAngularImpulse");
  b.Vector(spin, "Angular Impulse", 1.0f, 0.0f, 0.0f);
  b.Bool(spin, "Local", true);
  b.Link(push, "Done", spin, "Flow");
  GenNode &blast = b.Add("LogicNativeExplosionImpulse");
  b.Link(spin, "Done", blast, "Flow");
  GenNode &after = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(after, "Property", "pf2");
  b.Float(after, "Value", 4.0f);
  b.Link(blast, "Done", after, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;

  SemRig rig(program, kMemoOn, 2);
  rig.Object(0).NodeSetWorldPosition(MT_Vector3(1.0f, 2.0f, 3.0f));
  rig.Object(0).NodeSetGlobalOrientation(kQuarterTurn);
  rig.Object(0).NodeUpdateGS(0.0);
  rig.Object(1).NodeSetWorldPosition(MT_Vector3(-4.0f, 0.0f, 0.0f));
  rig.Object(1).NodeUpdateGS(0.0);
  rig.SetBool(rig.Object(0), "pb0", true);
  rig.SetBool(rig.Object(1), "pb0", false);

  ImpulseObserver observer;
  rig.Tick(LN_Event::OnFixedUpdate);

  const std::vector<ImpulseRecord> turned = observer.Of("Obj0");
  ASSERT_EQ(turned.size(), 2u);
  ExpectVector(turned[0].point, MT_Vector3(1.0f, 0.5f, 3.0f), "turned owner's local point");
  ExpectVector(turned[0].vector, MT_Vector3(0.0f, 3.0f, 0.0f), "turned owner's local impulse");
  EXPECT_TRUE(turned[0].velocity_change);
  EXPECT_TRUE(turned[1].angular);
  ExpectVector(turned[1].vector, MT_Vector3(0.0f, 1.0f, 0.0f), "turned owner's spin");

  const std::vector<ImpulseRecord> straight = observer.Of("Obj1");
  ASSERT_EQ(straight.size(), 2u);
  ExpectVector(straight[0].point, MT_Vector3(-5.5f, 0.0f, 0.0f), "local point");
  ExpectVector(straight[0].vector, MT_Vector3(3.0f, 0.0f, 0.0f), "impulse");
  EXPECT_FALSE(straight[0].velocity_change);
  ExpectVector(straight[1].vector, MT_Vector3(1.0f, 0.0f, 0.0f), "spin");

  /* The blast found nothing (no physics world) and still ran on: Done fired. */
  EXPECT_FLOAT_EQ(rig.Float(rig.Object(0), "pf2"), 4.0f);
  EXPECT_FLOAT_EQ(rig.Float(rig.Object(1), "pf2"), 4.0f);
}

TEST(LN_Impulse, AnApplyImpulseMadeBeforeItsSettingsCompilesAsBefore)
{
  /* Unchecked Local / Ignore Mass / Visualize add no expression and World Offset is mode 0: the
   * instruction (and so the program) of a node made before 2026-11-11 is the old one. */
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &push = b.Add("LogicNativeApplyImpulse", 0 /* World Offset */);
  b.Vector(push, "Attach", 0.0f, 0.1f, 0.0f);
  b.Vector(push, "Impulse", 0.0f, 0.0f, 0.1f);
  b.Link(step, "Out", push, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  size_t found = 0;
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
    if (instruction.opcode != LN_OpCode::ApplyImpulse) {
      continue;
    }
    found++;
    EXPECT_EQ(instruction.int_value, 0);
    EXPECT_EQ(instruction.bool_expr_index, LN_INVALID_INDEX);
    EXPECT_EQ(instruction.secondary_bool_expr_index, LN_INVALID_INDEX);
    EXPECT_EQ(instruction.tertiary_bool_expr_index, LN_INVALID_INDEX);
  }
  EXPECT_EQ(found, 1u);

  /* And it records the old command. */
  SemRig rig(program, kMemoOn, 1);
  rig.SetFlush(false);
  rig.Tick(LN_Event::OnFixedUpdate);
  EXPECT_NE(rig.LastRecorded().find("type=ApplyImpulse"), std::string::npos) << rig.LastRecorded();
}

TEST(LN_Impulse, ExplosionImpulseCompilesItsSettingsAndItsObjectsQuery)
{
  TreeBuilder b;
  b.SetRealSockets(true); /* Explosion Impulse's Mask is a Collision Layers socket */
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &blast = b.Add("LogicNativeExplosionImpulse", 0 /* Constant */);
  b.Vector(blast, "Origin", 1.0f, 2.0f, 3.0f);
  b.Float(blast, "Radius", 7.0f);
  b.Float(blast, "Strength", 12.0f);
  b.Float(blast, "Upward Lift", 1.5f);
  b.Bool(blast, "Ignore Mass", true);
  b.Link(step, "Out", blast, "Flow");
  GenNode &squad = b.Add("LogicNativeEmptyList");
  b.Link(squad, "List", blast, "Ignore Objects");
  GenNode &count = b.Add("LogicNativeListLength");
  b.Link(blast, "Objects", count, "List");
  GenNode &store = b.Add("LogicNativeSetGamePropertyInt");
  b.String(store, "Property", "pi0");
  b.Link(count, "Length", store, "Value");
  b.Link(blast, "Done", store, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;

  const LN_Instruction *explosion = nullptr;
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
    if (instruction.opcode == LN_OpCode::ExplosionImpulse) {
      explosion = &instruction;
    }
  }
  ASSERT_NE(explosion, nullptr) << LN_DumpProgramText(*program);
  EXPECT_EQ(explosion->int_value, LN_EXPLOSION_FALLOFF_CONSTANT);
  EXPECT_NE(explosion->float_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(explosion->secondary_float_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(explosion->tertiary_float_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(explosion->int_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(explosion->quaternary_value_expr_index, LN_INVALID_INDEX) << "Ignore Objects";

  /* Objects is a Shape Cast All of the same sphere, with X-Ray and room for 256. */
  size_t queries = 0;
  for (const LN_QueryExpression &query : program->GetQueryExpressions()) {
    if (query.kind != LN_QueryExpressionKind::ShapeCastAll) {
      continue;
    }
    queries++;
    EXPECT_EQ(query.input1, query.input2);
    EXPECT_EQ(query.shape_cast_type, LN_ShapeCastType::Sphere);
    EXPECT_EQ(query.secondary_int_value, int32_t(LN_EXPLOSION_MAX_OBJECTS));
    EXPECT_FLOAT_EQ(query.float_value, 7.0f);
    EXPECT_EQ(query.tertiary_bool_expr_index, LN_INVALID_INDEX) << "the query never draws";
    EXPECT_NE(query.ignore_list_expr_index, LN_INVALID_INDEX) << "Ignore Objects";
  }
  EXPECT_EQ(queries, 1u);

  /* Without a physics world nothing is found: an empty list, and the node still runs on. */
  SemRig rig(program, kMemoOn, 1);
  rig.SetInt(rig.Object(0), "pi0", -1);
  rig.Tick(LN_Event::OnFixedUpdate);
  EXPECT_FLOAT_EQ(rig.Float(rig.Object(0), "pi0"), 0.0f);
}

TEST(LN_Impulse, AnExplosionAtAnObjectBlastsAroundItAndLeavesItAlone)
{
  for (const bool named : {false, true}) {
    SCOPED_TRACE(named ? "a named object" : "the tree's object");
    TreeBuilder b;
    b.SetRealSockets(true);
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &blast = b.Add(
        "LogicNativeExplosionImpulse", 3 /* Linear + Implode */, 1 /* Object */);
    b.Vector(blast, "Origin", 7.0f, 7.0f, 7.0f);
    b.Link(step, "Out", blast, "Flow");
    if (named) {
      GenNode &barrel = b.Add("LogicNativeObjectByName");
      b.String(barrel, "Name", "Barrel");
      b.Link(barrel, "Object", blast, "Object");
    }
    GenNode &count = b.Add("LogicNativeListLength");
    b.Link(blast, "Objects", count, "List");
    GenNode &store = b.Add("LogicNativeSetGamePropertyInt");
    b.String(store, "Property", "pi0");
    b.Link(count, "Length", store, "Value");
    b.Link(blast, "Done", store, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;

    const LN_Instruction *explosion = nullptr;
    for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
      if (instruction.opcode == LN_OpCode::ExplosionImpulse) {
        explosion = &instruction;
      }
    }
    ASSERT_NE(explosion, nullptr) << LN_DumpProgramText(*program);
    EXPECT_EQ(explosion->secondary_int_value, LN_EXPLOSION_CENTER_OBJECT);
    EXPECT_EQ(explosion->int_value, LN_EXPLOSION_FALLOFF_LINEAR | LN_EXPLOSION_IMPLODE);
    EXPECT_EQ(explosion->vector_expr_index, LN_INVALID_INDEX) << "the Origin is not read";
    EXPECT_EQ(explosion->tertiary_value_expr_index != LN_INVALID_INDEX, named);

    /* Objects: a Local cast around the object at (0, 0, 0) - the object is its caster, left
     * alone, the owner when empty - that runs only while a named object exists. */
    size_t queries = 0;
    for (const LN_QueryExpression &query : program->GetQueryExpressions()) {
      if (query.kind != LN_QueryExpressionKind::ShapeCastAll) {
        continue;
      }
      queries++;
      EXPECT_TRUE(query.bool_value);
      ASSERT_NE(query.bool_expr_index, LN_INVALID_INDEX);
      EXPECT_EQ(query.input0 != LN_INVALID_INDEX, named);
      EXPECT_EQ(query.input1, query.input2);
      EXPECT_FLOAT_EQ(query.vector_value.x(), 0.0f) << "never the Origin";
      if (named) {
        ASSERT_NE(query.condition_bool_expr_index, LN_INVALID_INDEX);
        const LN_BoolExpression &gate =
            program->GetBoolExpressions()[query.condition_bool_expr_index];
        const bool gated_by_existence =
            gate.kind == LN_BoolExpressionKind::FromGenericValue ||
            (gate.kind == LN_BoolExpressionKind::And &&
             program->GetBoolExpressions()[gate.input1].kind ==
                 LN_BoolExpressionKind::FromGenericValue);
        EXPECT_TRUE(gated_by_existence);
      }
    }
    EXPECT_EQ(queries, 1u);

    /* No physics world: nothing found, the node still runs on (a missing Barrel too). */
    SemRig rig(program, kMemoOn, 1);
    rig.SetInt(rig.Object(0), "pi0", -1);
    rig.Tick(LN_Event::OnFixedUpdate);
    EXPECT_FLOAT_EQ(rig.Float(rig.Object(0), "pi0"), 0.0f);
  }
}

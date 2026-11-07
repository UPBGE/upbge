/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * What phase B of the lean runtime plan (tree blocks) kept: the wider batch allow lists (B0),
 * parallel snapshot capture and trees sharing their object in batches. The block executor itself
 * (B1-B2, op by op across trees) measured slower than tree batches and was removed on 2026-11-03.
 * Every test compares a run of the production path with the same run as a tree batch on helper
 * threads, by the whole text of the differential world (recorded commands, applied world, every
 * tree's state). */

#include "testing/testing.h"

#include <cstring>
#include <string>

#include <deque>

#include "LN_JobPool.h"
#include "LN_LeanProgram.h"
#include "LN_SemRig.h"
#include "LN_TreeBatch.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

struct WideScope {
  explicit WideScope(const int wide)
  {
    LN_SetBatchWideForTests(wide);
  }
  ~WideScope()
  {
    LN_SetBatchWideForTests(-1);
  }
};

/** On Logic + Physics Step -> a Get Object Attribute of `attribute` (custom1) read into a property.
 * `object_named`: the object input names the world's Target (a constant object), else it is empty
 * (the owner). `pin` is the attribute's output; `then` adds the node that stores it. */
std::shared_ptr<LN_Program> AttributeReader(const int16_t attribute,
                                            const bool object_named,
                                            const char *pin,
                                            const char *store_kind,
                                            const char *store_pin)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &read = b.Add("LogicNativeGetObjectAttribute", attribute);
  if (object_named) {
    b.ObjectNamed(read, "Object", &diff::TargetBlenderObject());
  }
  GenNode *source = &read;
  const char *source_pin = pin;
  if (std::strcmp(store_kind, "LogicNativeSetGamePropertyFloat") == 0 &&
      std::strcmp(pin, "Vector") == 0)
  {
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(read, pin, split, "Vector");
    source = &split;
    source_pin = "X";
  }
  GenNode &store = b.Add(store_kind);
  b.String(store, "Property", "got");
  b.Link(*source, source_pin, store, store_pin);
  b.Link(event, "Out", store, "Flow");
  std::string report;
  return b.Compile(&report);
}

/** Whether a tree batch of the program ends as the serial recording ends it. */
void ExpectBatchEqualsSerial(const std::shared_ptr<LN_Program> &program, const char *what)
{
  diff::RunOptions serial;
  serial.objects = 17;
  diff::RunOptions batch = serial;
  batch.chunks = 2;
  batch.helpers = 3;
  const diff::RunResult a = diff::RunWorld(program, serial);
  const diff::RunResult b = diff::RunWorld(program, batch);
  EXPECT_TRUE(b.batched) << what << ": the run was not a batch";
  EXPECT_EQ(a.text, b.text) << what << ": " << diff::FirstDifference(a.text, b.text);
  EXPECT_TRUE(b.failure.empty()) << what << ": " << b.failure;
}

}  // namespace

/* B0: Get Object Attribute of the owner or of a constant object (position, visibility, name, scale)
 * joins a tree batch, and the batch ends exactly as the serial recording ends. */
TEST(LN_TreeBlock, GetObjectAttributeOfTheOwnerAndAConstantObjectBatchesAndEndsAsSerial)
{
  struct Case {
    int16_t attribute;
    const char *pin;
    const char *store_kind;
    const char *store_pin;
    const char *name;
  };
  const Case cases[] = {
      {0, "Vector", "LogicNativeSetGamePropertyFloat", "Value", "world position"},
      {1, "Vector", "LogicNativeSetGamePropertyFloat", "Value", "local position"},
      {2, "Visible", "LogicNativeSetGamePropertyBool", "Value", "visible"},
      {3, "Name", "LogicNativeSetGamePropertyString", "Value", "name"},
      {5, "Vector", "LogicNativeSetGamePropertyFloat", "Value", "world scale"},
  };
  WideScope wide(1);
  for (const Case &c : cases) {
    for (const bool named : {false, true}) {
      const std::shared_ptr<LN_Program> program = AttributeReader(
          c.attribute, named, c.pin, c.store_kind, c.store_pin);
      ASSERT_NE(program, nullptr) << c.name;
      const LN_TreeBatchProfile &profile = program->GetTreeBatchProfile();
      EXPECT_TRUE(profile.eligible) << c.name << (named ? " (named)" : " (owner)") << ": "
                                    << profile.reason;
      if (profile.eligible) {
        ExpectBatchEqualsSerial(program, c.name);
      }
    }
  }
}

/* A velocity is read through the physics body: it stays out of batches. With the switch at 0 the
 * allow lists are those of step 7 and the position reads stay out too. */
TEST(LN_TreeBlock, VelocitiesStayOutOfBatchesAndTheSwitchTurnsTheWideListsOff)
{
  {
    WideScope wide(1);
    const std::shared_ptr<LN_Program> velocity = AttributeReader(
        9, false, "Vector", "LogicNativeSetGamePropertyFloat", "Value");
    ASSERT_NE(velocity, nullptr);
    EXPECT_FALSE(velocity->GetTreeBatchProfile().eligible);
  }
  {
    WideScope narrow(0);
    const std::shared_ptr<LN_Program> position = AttributeReader(
        0, false, "Vector", "LogicNativeSetGamePropertyFloat", "Value");
    ASSERT_NE(position, nullptr);
    EXPECT_FALSE(position->GetTreeBatchProfile().eligible);
  }
}

/* The native number writes, Modify Property and vector commands of the LeanNative sweeps, in a
 * tree batch, end as serial. */
TEST(LN_TreeBlock, NativeWritesInBatchesEndAsSerial)
{
  /* B0's allow lists (an UPBGE_LN_BATCH_WIDE=0 run would batch fewer of these programs). */
  WideScope wide(1);
  uint32_t compiled = 0;
  for (int16_t custom1 = 0; custom1 < 40; custom1++) {
    for (const bool linked : {false, true}) {
      TreeBuilder b;
      GenNode &event = b.Add("LogicNativeOnFixedUpdate");
      GenNode &node = b.Add("LogicNativeSetObjectAttribute", custom1, 0);
      b.Link(event, "Out", node, "Flow");
      if (linked) {
        GenNode &x = b.Add("LogicNativeGetGamePropertyFloat");
        b.String(x, "Property", "pf0");
        GenNode &vector = b.Add("LogicNativeCombineXYZ");
        b.Link(x, "Value", vector, "X");
        b.Float(vector, "Y", -0.4f);
        b.Float(vector, "Z", 0.9f);
        for (const char *pin : {"Value", "Position", "Scale"}) {
          b.Link(vector, "Vector", node, pin);
        }
      }
      else {
        b.Vector(node, "Value", 0.3f, -0.2f, 0.5f);
        b.Vector(node, "Position", 0.4f, 0.1f, -0.3f);
        b.Vector(node, "Scale", 1.2f, 1.3f, 1.4f);
        b.Rotation(node, "Rotation", 0.2f, 0.1f, -0.4f);
      }
      std::string report;
      const std::shared_ptr<LN_Program> program = b.Compile(&report);
      if (program == nullptr || !program->GetTreeBatchProfile().eligible) {
        continue;
      }
      compiled++;
      ExpectBatchEqualsSerial(program, "set object attribute");
    }
  }
  EXPECT_GE(compiled, 10u);
}

/* Parallel snapshot capture: capturing the snapshots of many trees on helper threads gives the
 * same snapshots as capturing them one after the other, step after step, whatever the properties
 * of the objects are. The programs read game properties, positions and visibility. */
TEST(LN_TreeBlock, SnapshotsCapturedOnHelperThreadsEqualThoseCapturedInTurn)
{
  BrainOfOptions options;
  options.step_every = 1;
  options.measure_distance = false;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  constexpr uint32_t kOwners = 600;
  SemRig serial(program, kMemoOn, kOwners);
  SemRig parallel(program, kMemoOn, kOwners);
  parallel.SetParallelCapture(3);
  for (int step = 0; step < 5; step++) {
    for (uint32_t index = 0; index < kOwners; index++) {
      const float dist = float((index * 7 + step * 13) % 29) - 3.0f;
      serial.SetFloat(serial.Object(index), "dist", dist);
      parallel.SetFloat(parallel.Object(index), "dist", dist);
      if (step % 2 == 1 && index % 5 == 0) {
        serial.SetFloat(serial.Object(index), "t0", float(index) * 0.5f);
        parallel.SetFloat(parallel.Object(index), "t0", float(index) * 0.5f);
      }
    }
    serial.Step();
    parallel.Step();
    EXPECT_EQ(serial.LastRecorded(), parallel.LastRecorded()) << "step " << step;
  }
  EXPECT_GT(parallel.ParallelCaptured(), size_t(kOwners)) << "no snapshot was captured in parallel";
  EXPECT_EQ(serial.ParallelCaptured(), 0u);
}

/* D16 (2026-11-02): an object with several trees can still be recorded in a batch when its tree
 * measures no Get Distance from the object itself (that read is the one that applies another
 * tree's pending move), or when no other tree of the object moves it. NPCs with a mover and a
 * reader tree, or with the brain and a reader tree: the batch records exactly what the trees
 * record one after the other, whatever the helpers and chunks, and the programs' flags say who
 * may share. */
TEST(LN_TreeBlock, TreesSharingAnObjectBatchWhenNoneMeasuresADistanceFromIt)
{
  /* A: walks the object and counts. B: reads the object's position (the old one: the walk is
   * only recorded) into a property. */
  TreeBuilder mover_builder;
  {
    GenNode &event = mover_builder.Add("LogicNativeOnUpdate");
    GenNode &walk = mover_builder.Add("LogicNativeApplyMovement");
    mover_builder.Vector(walk, "Vector", 0.05f, 0.02f, 0.0f);
    mover_builder.Bool(walk, "Local", false);
    mover_builder.Link(event, "Out", walk, "Flow");
    GenNode &mark = mover_builder.Add("LogicNativeSetGamePropertyFloat");
    mover_builder.String(mark, "Property", "walked");
    mover_builder.Float(mark, "Value", 1.0f);
    mover_builder.Link(walk, "Done", mark, "Flow");
  }
  std::string report;
  const std::shared_ptr<LN_Program> mover = mover_builder.Compile(&report);
  ASSERT_NE(mover, nullptr) << report;
  TreeBuilder reader_builder;
  {
    GenNode &event = reader_builder.Add("LogicNativeOnUpdate");
    GenNode &read = reader_builder.Add("LogicNativeGetObjectAttribute", 0);
    GenNode &split = reader_builder.Add("LogicNativeSeparateXYZ");
    reader_builder.Link(read, "Vector", split, "Vector");
    GenNode &store = reader_builder.Add("LogicNativeSetGamePropertyFloat");
    reader_builder.String(store, "Property", "seen_x");
    reader_builder.Link(split, "X", store, "Value");
    reader_builder.Link(event, "Out", store, "Flow");
  }
  const std::shared_ptr<LN_Program> reader = reader_builder.Compile(&report);
  ASSERT_NE(reader, nullptr) << report;
  const std::shared_ptr<LN_Program> brain = CompileBrain(BrainOptions());
  ASSERT_NE(brain, nullptr);

  WideScope wide(1);
  EXPECT_TRUE(mover->GetTreeBatchProfile().eligible) << mover->GetTreeBatchProfile().reason;
  EXPECT_TRUE(reader->GetTreeBatchProfile().eligible) << reader->GetTreeBatchProfile().reason;
  ASSERT_TRUE(brain->GetTreeBatchProfile().eligible) << brain->GetTreeBatchProfile().reason;
  EXPECT_FALSE(mover->GetTreeBatchProfile().reads_owner_distance);
  EXPECT_FALSE(reader->GetTreeBatchProfile().reads_owner_distance);
  EXPECT_TRUE(brain->GetTreeBatchProfile().reads_owner_distance);
  EXPECT_TRUE(mover->GetTreeBatchProfile().moves_owner);
  EXPECT_FALSE(reader->GetTreeBatchProfile().moves_owner);
  EXPECT_TRUE(brain->GetTreeBatchProfile().moves_owner);
  /* object_movers counts the tree itself when it moves its object. */
  EXPECT_TRUE(LN_TreeMayShareItsObject(mover->GetTreeBatchProfile(), 2));
  EXPECT_TRUE(LN_TreeMayShareItsObject(reader->GetTreeBatchProfile(), 2));
  /* The brain beside a tree that moves its NPC: no; beside trees that do not: yes. */
  EXPECT_FALSE(LN_TreeMayShareItsObject(brain->GetTreeBatchProfile(), 2));
  EXPECT_TRUE(LN_TreeMayShareItsObject(brain->GetTreeBatchProfile(), 1));

  constexpr size_t kObjects = 150;
  struct World {
    std::deque<BatchObject> objects;
    std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
    std::vector<LN_RuntimeTree *> pointers;
    World(const std::shared_ptr<LN_Program> &first, const std::shared_ptr<LN_Program> &second)
    {
      for (size_t index = 0; index < kObjects; index++) {
        objects.emplace_back(MT_Vector3(float(index) * 0.5f, 1.0f, 0.0f));
        for (uint32_t which = 0; which < 2; which++) {
          trees.push_back(std::make_unique<LN_RuntimeTree>(
              which == 0 ? first : second, &objects.back().object, uint32_t(index), which));
          pointers.push_back(trees.back().get());
        }
      }
    }
  };
  const auto same_world = [](World &a, World &b) {
    for (size_t index = 0; index < kObjects; index++) {
      const MT_Vector3 pa = a.objects[index].object.NodeGetWorldPosition();
      const MT_Vector3 pb = b.objects[index].object.NodeGetWorldPosition();
      if (pa.x() != pb.x() || pa.y() != pb.y() || pa.z() != pb.z()) {
        return false;
      }
      for (const char *name : {"walked", "seen_x"}) {
        EXP_Value *va = a.objects[index].object.GetProperty(name);
        EXP_Value *vb = b.objects[index].object.GetProperty(name);
        if ((va == nullptr) != (vb == nullptr) ||
            (va != nullptr && va->GetNumber() != vb->GetNumber()))
        {
          return false;
        }
      }
    }
    return true;
  };

  size_t compared = 0;
  /* Mover + reader in both orders, and the brain (Get Distance from its NPC, moves it) beside the
   * reader (which does not move it), in both orders. */
  struct Pair {
    const std::shared_ptr<LN_Program> *first;
    const std::shared_ptr<LN_Program> *second;
    const char *name;
  };
  const Pair pairs[] = {{&mover, &reader, "mover first"},
                        {&reader, &mover, "reader first"},
                        {&brain, &reader, "brain first"},
                        {&reader, &brain, "brain second"}};
  for (const Pair &pair : pairs) {
    for (const uint32_t helpers : {0u, 3u}) {
      for (const size_t chunks : {size_t(1), size_t(2), size_t(7)}) {
        SCOPED_TRACE(std::string(pair.name) + ", helpers " + std::to_string(helpers) +
                     ", chunks " + std::to_string(chunks));
        World one(*pair.first, *pair.second);
        World batched(*pair.first, *pair.second);
        LN_JobPool pool;
        pool.SetHelperCount(helpers);
        LN_TreeBatchRecorder recorder;
        for (uint64_t frame = 1; frame <= 6; frame++) {
          LN_TickContext context;
          context.event = LN_Event::OnUpdate;
          context.tick_index = frame;
          context.delta_time = 1.0 / 60.0;
          LN_CommandBuffer one_buffer;
          one_buffer.BeginRecording();
          for (LN_RuntimeTree *tree : one.pointers) {
            tree->ExecuteReady(one_buffer, context);
          }
          one_buffer.EndRecording();
          LN_CommandBuffer batched_buffer;
          batched_buffer.BeginRecording();
          EXPECT_EQ(recorder.Record(pool,
                                    batched_buffer,
                                    batched.pointers.data(),
                                    batched.pointers.size(),
                                    context,
                                    nullptr,
                                    nullptr,
                                    chunks),
                    0u);
          batched_buffer.EndRecording();
          ASSERT_EQ(batched_buffer.Size(), one_buffer.Size()) << "frame " << frame;
          ASSERT_EQ(batched_buffer.FlushDecisionForTests(), one_buffer.FlushDecisionForTests())
              << "frame " << frame;
          for (const bool coalesce : {false, true}) {
            ASSERT_EQ(batched_buffer.DirectFlushOrderForTests(coalesce),
                      one_buffer.DirectFlushOrderForTests(coalesce))
                << "frame " << frame;
          }
          one_buffer.Flush();
          batched_buffer.Flush();
          ASSERT_TRUE(same_world(one, batched)) << "frame " << frame;
          compared++;
        }
      }
    }
  }
  EXPECT_EQ(compared, size_t(4 * 2 * 3 * 6));
}

/* Why a tree that measures a distance from its own object stays out of a batch when its object has
 * other trees: one tree walks the object, the brain's Get Distance after it would apply that walk
 * (as it does one tree after the other), and a batch cannot give it. The recorder's safety net
 * names the chunk, which is what the manager's rule keeps from ever happening. */
TEST(LN_TreeBlock, ABrainAfterAMoverOfItsObjectCannotBeBatchedAndTheNetSaysSo)
{
  TreeBuilder mover_builder;
  GenNode &event = mover_builder.Add("LogicNativeOnUpdate");
  GenNode &walk = mover_builder.Add("LogicNativeApplyMovement");
  mover_builder.Vector(walk, "Vector", 0.05f, 0.02f, 0.0f);
  mover_builder.Bool(walk, "Local", false);
  mover_builder.Link(event, "Out", walk, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> mover = mover_builder.Compile(&report);
  ASSERT_NE(mover, nullptr) << report;
  const std::shared_ptr<LN_Program> brain = CompileBrain(BrainOptions());
  ASSERT_NE(brain, nullptr);

  std::deque<BatchObject> objects;
  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
  std::vector<LN_RuntimeTree *> pointers;
  for (size_t index = 0; index < 6; index++) {
    objects.emplace_back(MT_Vector3(1.0f + float(index), 0.0f, 0.0f));
    trees.push_back(
        std::make_unique<LN_RuntimeTree>(mover, &objects.back().object, uint32_t(index), 0));
    pointers.push_back(trees.back().get());
    trees.push_back(
        std::make_unique<LN_RuntimeTree>(brain, &objects.back().object, uint32_t(index), 1));
    pointers.push_back(trees.back().get());
  }
  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.tick_index = 1;
  context.delta_time = 1.0 / 60.0;
  LN_JobPool pool;
  pool.SetHelperCount(0);
  LN_TreeBatchRecorder recorder;
  LN_CommandBuffer buffer;
  buffer.BeginRecording();
  EXPECT_GT(recorder.Record(pool, buffer, pointers.data(), pointers.size(), context, nullptr, nullptr, 1),
            0u)
      << "a brain after a mover of its own object read the walk in a batch without being noted";
  buffer.EndRecording();
}

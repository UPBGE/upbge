/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The block phases of the lean runtime plan (B0 and on): the wider batch allow lists (B0) and the
 * block executor (B1 and on). Every test compares a run of the production path with the same run
 * in the shape under test (a tree batch on helper threads; later a block), by the whole text of
 * the differential world (recorded commands, applied world, every tree's state). */

#include "testing/testing.h"

#include <cstring>
#include <string>

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

/** The program as a tree batch in blocks (Force: any two trees of it run together), against the
 * serial recording: the whole text must be the same. Returns the lanes that ran in blocks. */
uint64_t ExpectBlocksEqualSerial(const std::shared_ptr<LN_Program> &program,
                                 const char *what,
                                 const uint32_t objects = 17)
{
  diff::RunOptions serial;
  serial.objects = objects;
  serial.lean = 2;
  serial.blocks = 0;
  diff::RunOptions blocks = serial;
  blocks.chunks = 2;
  blocks.helpers = 3;
  blocks.blocks = 2;
  const uint64_t lanes_before = LN_BlockLaneCount();
  const diff::RunResult a = diff::RunWorld(program, serial);
  const diff::RunResult b = diff::RunWorld(program, blocks);
  EXPECT_EQ(a.text, b.text) << what << ": " << diff::FirstDifference(a.text, b.text);
  EXPECT_TRUE(b.failure.empty()) << what << ": " << b.failure;
  return LN_BlockLaneCount() - lanes_before;
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

/* B1: the brain (flow, number writes, Get Distance, Apply Movement / Rotation) in blocks ends
 * exactly as the serial recording ends it, commands and record sequences included. */
TEST(LN_TreeBlock, TheBrainInBlocksEndsAsTheSerialRecordingEndsIt)
{
  for (const bool measure : {true, false}) {
    BrainOfOptions options;
    options.step_every = 1;
    options.measure_distance = measure;
    const std::shared_ptr<LN_Program> program = BrainOf(options);
    ASSERT_NE(program, nullptr);
    EXPECT_GT(ExpectBlocksEqualSerial(program, measure ? "brain" : "brain, stored distance"), 0u)
        << "no lane ran in a block";
  }
}

/* The native number writes, Modify Property and vector commands of the LeanNative sweeps, in
 * blocks, end as serial. */
TEST(LN_TreeBlock, NativeWritesInBlocksEndAsSerial)
{
  uint32_t compiled = 0;
  uint64_t block_lanes = 0;
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
      block_lanes += ExpectBlocksEqualSerial(program, "set object attribute", 9);
    }
  }
  EXPECT_GE(compiled, 10u);
  EXPECT_GT(block_lanes, 0u);
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

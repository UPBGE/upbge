/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Tests of the strict dumps (lean runtime plan, phase A2): the command buffer's dump, the runtime
 * tree's state dump, the scene-access seam. Their job is to make "bit for bit the same" checkable,
 * so each is also tried with a planted difference, which it must report. */

#include "testing/testing.h"

#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "EXP_FloatValue.h"
#include "EXP_StringValue.h"
#include "LN_CommandBuffer.h"
#include "LN_JobPool.h"
#include "LN_Manager.h"
#include "LN_RuntimeTree.h"
#include "LN_StrictDump.h"
#include "LN_TestTrees.h"
#include "LN_TreeBatch.h"
#include "LN_TreeCompiler.h"

namespace {

using namespace ln_test;

LN_TickContext FrameContext(const uint64_t tick)
{
  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.tick_index = tick;
  context.delta_time = 1.0 / 60.0;
  return context;
}

/** The three dumps of a recording, in one string. */
std::string AllOrders(LN_CommandBuffer &buffer)
{
  using Order = LN_CommandBuffer::DumpOrder;
  return "== record\n" + buffer.DumpRecordedForTests(Order::Record) + "== flush\n" +
         buffer.DumpRecordedForTests(Order::Flush) + "== flush, coalesced\n" +
         buffer.DumpRecordedForTests(Order::FlushCoalesced);
}

/** Objects of a brain test, spread over near and far, and a tree each. */
struct BrainWorld {
  std::deque<BatchObject> objects;
  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
  std::vector<LN_RuntimeTree *> pointers;

  BrainWorld(const std::shared_ptr<LN_Program> &program, const size_t count)
  {
    for (size_t index = 0; index < count; index++) {
      const float angle = float(index) * 0.37f;
      const float radius = 2.0f + float(index % 9);
      objects.emplace_back(MT_Vector3(radius * std::cos(angle), radius * std::sin(angle), 0.0f));
      trees.push_back(std::make_unique<LN_RuntimeTree>(
          program, &objects.back().object, uint32_t(index), 0));
      pointers.push_back(trees.back().get());
    }
  }
};

/* The dump lists every field and says what was recorded. */
TEST(LN_StrictDump, TheRecordingDumpSaysWhatWasRecorded)
{
  BrainOfOptions options;
  options.chains = 3;
  options.look = true;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);

  std::string dump;
  {
    BrainWorld world(program, 6);
    LN_CommandBuffer buffer;
    buffer.BeginRecording();
    const LN_TickContext context = FrameContext(1);
    for (LN_RuntimeTree *tree : world.pointers) {
      tree->ExecuteReady(buffer, context);
    }
    buffer.EndRecording();
    dump = buffer.DumpRecordedForTests(LN_CommandBuffer::DumpOrder::Record);
  }
  EXPECT_FALSE(dump.empty());
  if (std::getenv("LN_PRINT_DUMP") != nullptr) {
    std::printf("%s\n", dump.substr(0, 6000).c_str());
  }
  /* It says what the brains recorded: moves, a turn, the stored distance and a chain. */
  EXPECT_NE(dump.find("type=ApplyMovement"), std::string::npos);
  EXPECT_NE(dump.find("type=ApplyRotation"), std::string::npos);
  EXPECT_NE(dump.find("name_ptr=\"dist\""), std::string::npos);
  EXPECT_NE(dump.find("name_ptr=\"t2\""), std::string::npos);
  EXPECT_NE(dump.find("name_ptr=\"seen\""), std::string::npos);
}

/* A recording that the batch recorder makes is, field by field, the one the trees make one after
 * the other: three orders of the dump (as recorded, as flushed, as flushed with merging), and the
 * state of every tree afterwards (both parts). */
TEST(LN_StrictDump, ABatchAndTheSerialRecordingDumpAlike)
{
  BrainOfOptions options;
  options.chains = 2;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  ASSERT_TRUE(program->GetTreeBatchProfile().eligible) << program->GetTreeBatchProfile().reason;
  constexpr size_t count = 40;
  BrainWorld serial(program, count);
  BrainWorld batched(program, count);
  LN_JobPool pool;
  pool.SetHelperCount(2);
  LN_TreeBatchRecorder recorder;
  for (uint64_t frame = 1; frame <= 3; frame++) {
    SCOPED_TRACE("frame " + std::to_string(frame));
    const LN_TickContext context = FrameContext(frame);
    LN_CommandBuffer one;
    one.BeginRecording();
    for (LN_RuntimeTree *tree : serial.pointers) {
      tree->ExecuteReady(one, context);
    }
    one.EndRecording();
    LN_CommandBuffer many;
    many.BeginRecording();
    EXPECT_EQ(recorder.Record(
                  pool, many, batched.pointers.data(), count, context, nullptr, nullptr, 5),
              0u);
    many.EndRecording();
    const std::string a = AllOrders(one);
    const std::string b = AllOrders(many);
    EXPECT_TRUE(a == b) << "the batch recorded differently from the trees one after the other";
    EXPECT_FALSE(a.empty());
    for (size_t index = 0; index < count; index++) {
      const auto state_a = serial.pointers[index]->DumpStateForTests(frame);
      const auto state_b = batched.pointers[index]->DumpStateForTests(frame);
      EXPECT_EQ(state_a.meaning, state_b.meaning) << "tree " << index;
      EXPECT_EQ(state_a.bookkeeping, state_b.bookkeeping) << "tree " << index;
    }
    one.Flush();
    many.Flush();
  }
}

/* The dumps tell small differences apart: one ulp in a float, a swapped record order, a changed
 * element of a list in a value, the Euler hint of a rotation, an object. */
TEST(LN_StrictDump, PlantedDifferencesShowInTheDumps)
{
  BatchObject first(MT_Vector3(1.0f, 0.0f, 0.0f));
  BatchObject second(MT_Vector3(2.0f, 0.0f, 0.0f));
  const auto make = [&](const float x, const bool swapped, KX_GameObject *object,
                        const std::function<void(LN_CommandBuffer::Command &)> &change) {
    std::vector<LN_CommandBuffer::Command> commands(2);
    commands[0].type = LN_CommandBuffer::CommandType::SetWorldPosition;
    commands[0].object = object;
    commands[0].vector_value = MT_Vector3(x, 2.0f, 3.0f);
    commands[0].sort_key = 10;
    commands[1].type = LN_CommandBuffer::CommandType::SetGameProperty;
    commands[1].object = &second.object;
    commands[1].property_name = "dist";
    commands[1].property_value.exists = true;
    commands[1].property_value.type = LN_ValueType::List;
    commands[1].property_value.list_value.resize(2);
    commands[1].property_value.list_value[1].float_value = 0.5f;
    commands[1].sort_key = 20;
    change(commands[0]);
    change(commands[1]);
    if (swapped) { /* the buffer numbers the records in the order given */
      std::swap(commands[0], commands[1]);
    }
    LN_CommandBuffer buffer;
    buffer.BeginRecording();
    buffer.AppendLegacyCommandsForTests(std::move(commands));
    buffer.EndRecording();
    return AllOrders(buffer);
  };
  const auto nothing = [](LN_CommandBuffer::Command &) {};
  const std::string base = make(1.0f, false, &first.object, nothing);
  EXPECT_TRUE(base == make(1.0f, false, &first.object, nothing));
  EXPECT_NE(base.find("0x3f800000"), std::string::npos); /* the bits of 1.0f */

  /* One ulp. */
  EXPECT_TRUE(base != make(std::nextafter(1.0f, 2.0f), false, &first.object, nothing)) << "1 ulp";
  /* -0.0 against 0.0. */
  EXPECT_TRUE(make(0.0f, false, &first.object, nothing) !=
              make(-0.0f, false, &first.object, nothing))
      << "-0.0";
  /* The two commands in the other record order. */
  EXPECT_TRUE(base != make(1.0f, true, &first.object, nothing)) << "swapped record order";
  /* Another object. */
  EXPECT_TRUE(base != make(1.0f, false, &second.object, nothing)) << "another object";
  /* A change deep in a value, and in a field the command only holds sometimes. */
  EXPECT_TRUE(base != make(1.0f, false, &first.object, [](LN_CommandBuffer::Command &command) {
    if (command.type == LN_CommandBuffer::CommandType::SetGameProperty) {
      command.property_value.list_value[1].float_value = std::nextafter(0.5f, 1.0f);
    }
  })) << "a float deep in a value";
  EXPECT_TRUE(base != make(1.0f, false, &first.object, [](LN_CommandBuffer::Command &command) {
    if (command.type == LN_CommandBuffer::CommandType::SetGameProperty) {
      command.property_value.rotation_euler_hint_valid = true;
    }
  })) << "the Euler hint";
  EXPECT_TRUE(base != make(1.0f, false, &first.object, [](LN_CommandBuffer::Command &command) {
    command.quaternary_property_name = "x";
  })) << "a rarely used field";
}

/* The state dump keeps what a tree's behaviour depends on apart from its counters: trees that
 * slept through the steps of an Every N Steps tree have the meaning of trees that ran every step,
 * and a tree that did something more has a meaning of its own. */
TEST(LN_StrictDump, SleepingBetweenStepsChangesNoMeaningButOneMoreRunDoes)
{
  BrainOfOptions options;
  options.chains = 2;
  options.step_every = 4;
  options.measure_distance = false; /* SteppedTrees' objects have no scene-graph node */
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  constexpr uint32_t count = 8;
  constexpr uint64_t steps = 24;
  SteppedTrees awake(program, count);
  SteppedTrees asleep(program, count);
  awake.Run(steps, 1.0 / 60.0, false);
  asleep.Run(steps, 1.0 / 60.0, true);
  const uint64_t last_tick = steps; /* SteppedTrees::Context: tick_index = step + 1 */
  bool bookkeeping_differs = false;
  for (uint32_t index = 0; index < count; index++) {
    const auto a = awake.trees[index]->DumpStateForTests(last_tick);
    const auto b = asleep.trees[index]->DumpStateForTests(last_tick);
    EXPECT_EQ(a.meaning, b.meaning) << "tree " << index;
    EXPECT_FALSE(a.meaning.empty());
    bookkeeping_differs = bookkeeping_differs || a.bookkeeping != b.bookkeeping;
  }
  /* Whether sleeping shows in the counters is not the point; the two parts are apart. */
  (void)bookkeeping_differs;

  /* One tree runs one more step: its meaning is no longer the same. */
  LN_CommandBuffer buffer;
  buffer.BeginRecording();
  awake.trees[0]->PrepareStepGates(steps, 1.0 / 60.0, 1.0 / 60.0);
  awake.trees[0]->ExecuteReady(buffer, SteppedTrees::Context(steps, 1.0 / 60.0));
  buffer.EndRecording();
  EXPECT_NE(awake.trees[0]->DumpStateForTests(last_tick + 1).meaning,
            asleep.trees[0]->DumpStateForTests(last_tick).meaning);
}

/* The scene seam: trees that look objects up by name run without a scene. A constant object of
 * Get Distance is found by name in the lists the test gives, and not found without them. */
TEST(LN_StrictDump, TheSceneSeamLetsTreesFindObjectsByNameWithoutAScene)
{
  blender::Object target_object = {};
  SetCString(target_object.id.name, "OBTarget");

  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode event(tree, "LogicNativeOnUpdate", "Event", 1);
  blender::bNodeSocket &tick = event.flow(false, "Out");
  TestNode distance(tree, "LogicNativeGetDistance", "Distance Between", 2);
  distance.node.custom1 = 0; /* End A: the owner */
  distance.node.custom2 = 0; /* End B: an object */
  distance.in_object("Object A");
  distance.in_vector("Position A", MT_Vector3(0.0f, 0.0f, 0.0f));
  blender::bNodeSocket &end_b = distance.in_object("Object B");
  static_cast<blender::bNodeSocketValueObject *>(end_b.default_value)->value = &target_object;
  distance.in_vector("Position B", MT_Vector3(0.0f, 0.0f, 0.0f));
  blender::bNodeSocket &distance_out = distance.add(false, "Distance", blender::SOCK_FLOAT, nullptr);
  distance.add(false, "Squared Distance", blender::SOCK_FLOAT, nullptr);
  distance.add(false, "Valid", blender::SOCK_BOOLEAN, nullptr);
  TestNode store(tree, "LogicNativeSetGamePropertyFloat", "Set Game Property", 3);
  blender::bNodeSocket &store_flow = store.flow(true, "Flow");
  store.in_object("Object");
  store.in_string("Property", "d");
  blender::bNodeSocket &store_value = store.in_float("Value", -1.0f);
  store.flow(false, "Done");
  std::deque<blender::bNodeLink> links;
  const auto link = [&](TestNode &from, blender::bNodeSocket &out, TestNode &to,
                        blender::bNodeSocket &in) {
    InitLink(links.emplace_back(), from.node, out, to.node, in);
    Append(tree.links, links.back());
  };
  link(event, tick, store, store_flow);
  link(distance, distance_out, store, store_value);
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  ASSERT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  BatchObject owner(MT_Vector3(0.0f, 0.0f, 0.0f));
  BatchObject target(MT_Vector3(3.0f, 4.0f, 0.0f));
  target.object.SetName("Target");
  const auto stored_distance = [&]() {
    LN_RuntimeTree runtime_tree(program, &owner.object, 0, 0);
    LN_CommandBuffer buffer;
    buffer.BeginRecording();
    runtime_tree.ExecuteReady(buffer, FrameContext(1));
    buffer.EndRecording();
    for (const LN_CommandBuffer::Command &command : buffer.GetCommandsForTests()) {
      if (command.type == LN_CommandBuffer::CommandType::SetGameProperty &&
          ((command.property_name_ptr != nullptr && *command.property_name_ptr == "d") ||
           command.property_name == "d"))
      {
        return command.property_value.float_value;
      }
    }
    return -2.0f;
  };

  const float without = stored_distance(); /* no scene, no seam: nothing to find */
  EXPECT_NE(without, 5.0f);

  LN_SceneAccessForTests access;
  access.objects = {&owner.object, &target.object};
  LN_SetSceneAccessForTests(&access);
  const float with = stored_distance();
  LN_SetSceneAccessForTests(nullptr);
  EXPECT_FLOAT_EQ(with, 5.0f);
  EXPECT_NE(stored_distance(), 5.0f); /* the seam is off again */
}

/* A number write goes into the property in place when the property holds a number, and a whole
 * value (text) is converted to the property's type: today's flush never replaces a property by
 * one of another type (text into a float property gives -1.0). The order of two writes to one
 * property matters (the world dump says so). */
TEST(LN_StrictDump, ANumberWriteAndATextWriteToOnePropertyEndByTheirOrder)
{
  BrainOfOptions options;
  options.chains = 1;
  options.measure_distance = false;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  uint32_t ref = LN_INVALID_INDEX;
  for (uint32_t index = 0; index < program->GetGamePropertyRefs().size(); index++) {
    if (program->GetGamePropertyRefs()[index].name == "t0") {
      ref = index;
    }
  }
  ASSERT_NE(ref, LN_INVALID_INDEX);

  std::string results[2][2]; /* [string property][number last] */
  for (const bool string_property : {false, true}) {
    for (const bool number_last : {true, false}) {
      BatchObject owner(MT_Vector3(0.0f, 0.0f, 0.0f));
      owner.object.SetName("Owner");
      if (string_property) {
        owner.object.SetProperty("t0", new EXP_StringValue("start", "t0"));
      }
      else {
        owner.object.SetProperty("t0", new EXP_FloatValue(1.0f));
      }
      LN_RuntimeTree runtime_tree(program, &owner.object, 0, 0);
      LN_Value text;
      text.exists = true;
      text.type = LN_ValueType::String;
      text.string_value = "text";
      LN_PropertyNumber number;
      number.type = LN_ValueType::Float;
      number.float_value = 7.0f;
      LN_CommandBuffer buffer;
      buffer.BeginRecording();
      buffer.AppendSetGamePropertyRef(
          &runtime_tree, &owner.object, ref, text, number_last ? 1 : 2, 0);
      buffer.AppendSetGamePropertyNumberRef(
          &runtime_tree, &owner.object, ref, number, number_last ? 2 : 1, 1);
      buffer.EndRecording();
      const std::string world = FlushAndDumpWorld(buffer, {&owner.object});
      results[string_property][number_last] = world;
      if (!string_property && number_last) {
        /* The number written last: a float 7 (value type 2). */
        EXPECT_NE(world.find("property[t0]=2/"), std::string::npos) << world;
        EXPECT_NE(world.find(LN_DumpDoubleBits(7.0)), std::string::npos) << world;
      }
    }
    EXPECT_TRUE(results[string_property][0] != results[string_property][1])
        << "the order of the two writes did not matter";
  }
}

/* The program dumper writes a program the same way every time, and the programs of the brains
 * match their goldens (tests/fixtures/lean_golden): a compiler change that moves a node, a guard
 * or a constant shows here as a diff to read. */
TEST(LN_StrictDump, TheProgramDumpIsStableAndTheBrainsMatchTheirGoldens)
{
  struct Case {
    const char *golden;
    BrainOfOptions options;
  };
  BrainOfOptions plain;
  BrainOfOptions stepped;
  stepped.chains = 2;
  stepped.step_every = 4;
  stepped.measure_distance = false;
  stepped.look = true;
  for (const Case &item : {Case{"brain_of_program", plain}, Case{"brain_of_stepped_program", stepped}}) {
    SCOPED_TRACE(item.golden);
    const std::shared_ptr<LN_Program> a = BrainOf(item.options);
    const std::shared_ptr<LN_Program> b = BrainOf(item.options);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const std::string text = LN_DumpProgramText(*a);
    EXPECT_TRUE(text == LN_DumpProgramText(*b)) << "two compiles dump differently";
    EXPECT_NE(text.find("Op::ApplyTransformVector"), std::string::npos);
    EXPECT_NE(text.find("Float::"), std::string::npos);
    const std::string problem = CheckGolden(item.golden, text);
    EXPECT_TRUE(problem.empty()) << problem;
  }
  /* A different program dumps differently. */
  EXPECT_TRUE(LN_DumpProgramText(*BrainOf(plain)) != LN_DumpProgramText(*BrainOf(stepped)));
}

/* What the brains record is kept as a golden too: one tree in one frame in full, and for four trees
 * over two frames the hash of each frame's three dumps and of every tree's state. */
TEST(LN_StrictDump, WhatTheBrainsRecordMatchesItsGolden)
{
  BrainOfOptions options;
  options.chains = 2;
  const std::shared_ptr<LN_Program> program = BrainOf(options);
  ASSERT_NE(program, nullptr);
  BrainWorld world(program, 4);
  std::string summary;
  std::string one_tree_one_frame;
  for (uint64_t frame = 1; frame <= 2; frame++) {
    LN_CommandBuffer buffer;
    buffer.BeginRecording();
    for (LN_RuntimeTree *tree : world.pointers) {
      tree->ExecuteReady(buffer, FrameContext(frame));
    }
    buffer.EndRecording();
    using Order = LN_CommandBuffer::DumpOrder;
    const std::string record = buffer.DumpRecordedForTests(Order::Record);
    if (frame == 1) {
      one_tree_one_frame = record;
    }
    summary += "frame " + std::to_string(frame) + " commands " + std::to_string(buffer.Size()) +
               " record " + GoldenHash(record) + " flush " +
               GoldenHash(buffer.DumpRecordedForTests(Order::Flush)) + " coalesced " +
               GoldenHash(buffer.DumpRecordedForTests(Order::FlushCoalesced)) + "\n";
    for (size_t index = 0; index < world.pointers.size(); index++) {
      const auto state = world.pointers[index]->DumpStateForTests(frame);
      summary += " tree " + std::to_string(index) + " meaning " + GoldenHash(state.meaning) +
                 " bookkeeping " + GoldenHash(state.bookkeeping) + "\n";
    }
    buffer.Flush();
  }
  EXPECT_TRUE(CheckGolden("brain_of_records_hashes", summary).empty())
      << CheckGolden("brain_of_records_hashes", summary);
  /* The first tree's commands of frame 1 in full (its lines are the first of the dump). */
  size_t end = 0;
  for (int line = 0; line < 10 && end != std::string::npos; line++) {
    end = one_tree_one_frame.find('\n', end == 0 ? 0 : end + 1);
  }
  const std::string first_lines = one_tree_one_frame.substr(
      0, end == std::string::npos ? one_tree_one_frame.size() : end + 1);
  EXPECT_TRUE(CheckGolden("brain_of_first_commands", first_lines).empty())
      << CheckGolden("brain_of_first_commands", first_lines);
}

/* An object removed between recording and flush loses its commands, the others keep theirs (the
 * dump of what is left and the world after the flush say so). Removing an object during a flush
 * (the Remove Object command) needs a scene and is covered by the acceptance suite. */
TEST(LN_StrictDump, ARemovedObjectLosesItsRecordedCommands)
{
  NamedPropertyWriter writer({"x"});
  BatchObject removed(MT_Vector3(0.0f, 0.0f, 0.0f));
  BatchObject kept(MT_Vector3(0.0f, 0.0f, 0.0f));
  removed.object.SetName("Removed");
  kept.object.SetName("Kept");
  LN_CommandBuffer buffer;
  buffer.BeginRecording();
  buffer.AppendSetWorldPosition(&removed.object, MT_Vector3(1.0f, 2.0f, 3.0f), 1, 0);
  buffer.AppendSetWorldPosition(&kept.object, MT_Vector3(4.0f, 5.0f, 6.0f), 2, 1);
  LN_Value value;
  value.exists = true;
  value.type = LN_ValueType::Float;
  value.float_value = 9.0f;
  writer.Write(buffer, &removed.object, "x", value, 3, 2);
  buffer.AppendSetWorldPosition(&removed.object, MT_Vector3(7.0f, 8.0f, 9.0f), 4, 3);
  buffer.EndRecording();
  EXPECT_EQ(buffer.Size(), 4u);
  buffer.RemoveCommandsForGameObject(&removed.object);
  EXPECT_EQ(buffer.Size(), 1u);
  const std::string left_over = buffer.DumpRecordedForTests(LN_CommandBuffer::DumpOrder::Record);
  EXPECT_NE(left_over.find("obj=Kept#0"), std::string::npos);
  EXPECT_EQ(left_over.find("Removed"), std::string::npos);
  FlushAndDumpWorld(buffer, {&removed.object, &kept.object});
  EXPECT_EQ(removed.object.NodeGetWorldPosition(), MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(kept.object.NodeGetWorldPosition(), MT_Vector3(4.0f, 5.0f, 6.0f));
}

}  // namespace

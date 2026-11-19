/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Insert, List Operation and Get Children (2026-11-17, LN_ListOps.h): the rules against answers
 * written here, then the nodes reading game properties in every mode and in a wide run. Insert's
 * Game Property mode is held to its Output Only output by LN_NodeCoverage's
 * ListAndDictionaryChangesEndAsTheirOutputs (its row in kCollectionNodes). */

#include "testing/testing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "LN_CommandBuffer.h"
#include "LN_ListOps.h"
#include "LN_SemRig.h"
#include "LN_StrictDump.h"
#include "LN_TextOps.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

LN_Value Number(const LN_ValueType type, const double number)
{
  LN_Value value;
  value.type = type;
  value.exists = true;
  if (type == LN_ValueType::Int) {
    value.int_value = int32_t(number);
  }
  else if (type == LN_ValueType::Float) {
    value.float_value = float(number);
  }
  else if (type == LN_ValueType::Bool) {
    value.bool_value = number != 0.0;
  }
  return value;
}

LN_Value Text(const char *text)
{
  LN_Value value;
  value.type = LN_ValueType::String;
  value.exists = true;
  value.string_value = text;
  return value;
}

LN_Value ListOf(std::vector<LN_Value> items)
{
  LN_Value list;
  list.type = LN_ValueType::List;
  list.exists = true;
  list.list_value = std::move(items);
  return list;
}

}  // namespace

TEST(LN_ListOps, InsertPlaceIsPythons)
{
  EXPECT_EQ(LN_ListInsertPlace(0, 3), 0u);
  EXPECT_EQ(LN_ListInsertPlace(2, 3), 2u);
  EXPECT_EQ(LN_ListInsertPlace(3, 3), 3u) << "at the end";
  EXPECT_EQ(LN_ListInsertPlace(99, 3), 3u) << "past the end: at the end";
  EXPECT_EQ(LN_ListInsertPlace(-1, 3), 2u) << "-1: before the last item";
  EXPECT_EQ(LN_ListInsertPlace(-3, 3), 0u);
  EXPECT_EQ(LN_ListInsertPlace(-99, 3), 0u) << "before the start: at the start";
  EXPECT_EQ(LN_ListInsertPlace(0, 0), 0u);
  EXPECT_EQ(LN_ListInsertPlace(-1, 0), 0u);
  EXPECT_EQ(LN_ListInsertPlace(std::numeric_limits<int32_t>::min(), 5), 0u);
  EXPECT_EQ(LN_ListInsertPlace(std::numeric_limits<int32_t>::max(), 5), 5u);
}

TEST(LN_ListOps, SliceClampsAsSubstring)
{
  const auto slice = [](const int64_t count, const int32_t start, const int32_t length) {
    int64_t first = -7, last = -7;
    LN_ClampSlice(count, start, length, first, last);
    return std::to_string(first) + ".." + std::to_string(last);
  };
  EXPECT_EQ(slice(5, 0, 3), "0..3");
  EXPECT_EQ(slice(5, 1, -1), "1..5") << "a negative Length takes the rest";
  EXPECT_EQ(slice(5, -2, -1), "3..5") << "the last two";
  EXPECT_EQ(slice(5, -9, 2), "0..2");
  EXPECT_EQ(slice(5, 7, 2), "5..5") << "past the end: nothing";
  EXPECT_EQ(slice(5, 2, 0), "2..2");
  EXPECT_EQ(slice(0, 0, 3), "0..0");
  EXPECT_EQ(slice(5, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()),
            "0..5");
}

TEST(LN_ListOps, StatisticsCountOnlyNumbers)
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  int32_t index = 99;
  const std::vector<LN_Value> mixed = {Text("7"),
                                       Number(LN_ValueType::Int, 4),
                                       Number(LN_ValueType::Float, -1.5),
                                       Number(LN_ValueType::Bool, 1),
                                       Number(LN_ValueType::Float, nan),
                                       Number(LN_ValueType::Int, 9),
                                       Number(LN_ValueType::Float, -1.5),
                                       LN_Value()};
  EXPECT_EQ(LN_ListStatisticOf(mixed, LN_ListStatistic::Min, index), -1.5f);
  EXPECT_EQ(index, 2) << "the first smallest; text, truth, NaN and nothing are no numbers";
  EXPECT_EQ(LN_ListStatisticOf(mixed, LN_ListStatistic::Max, index), 9.0f);
  EXPECT_EQ(index, 5);
  EXPECT_EQ(LN_ListStatisticOf(mixed, LN_ListStatistic::Sum, index), 10.0f);
  EXPECT_EQ(index, -1) << "Sum has no place";
  EXPECT_EQ(LN_ListStatisticOf(mixed, LN_ListStatistic::Average, index), 2.5f);
  EXPECT_EQ(index, -1);

  for (const LN_ListStatistic statistic : {LN_ListStatistic::Min,
                                           LN_ListStatistic::Max,
                                           LN_ListStatistic::Sum,
                                           LN_ListStatistic::Average})
  {
    EXPECT_EQ(LN_ListStatisticOf({}, statistic, index), 0.0f);
    EXPECT_EQ(index, -1);
    EXPECT_EQ(LN_ListStatisticOf({Text("a"), Number(LN_ValueType::Float, nan)}, statistic, index),
              0.0f);
    EXPECT_EQ(index, -1) << "no numbers";
  }
  /* Ties: the first. Large whole numbers add up in double precision. */
  const std::vector<LN_Value> ties = {Number(LN_ValueType::Int, 3),
                                      Number(LN_ValueType::Float, 3.0),
                                      Number(LN_ValueType::Int, 1),
                                      Number(LN_ValueType::Int, 1)};
  EXPECT_EQ(LN_ListStatisticOf(ties, LN_ListStatistic::Max, index), 3.0f);
  EXPECT_EQ(index, 0);
  EXPECT_EQ(LN_ListStatisticOf(ties, LN_ListStatistic::Min, index), 1.0f);
  EXPECT_EQ(index, 2);
  const std::vector<LN_Value> big = {Number(LN_ValueType::Int, 16777217),
                                     Number(LN_ValueType::Int, 1),
                                     Number(LN_ValueType::Int, -16777216)};
  EXPECT_EQ(LN_ListStatisticOf(big, LN_ListStatistic::Sum, index), 2.0f)
      << "(in float 16777217 + 1 would be 16777216 + 1 = 16777216)";
  const std::vector<LN_Value> infinite = {Number(LN_ValueType::Float, 1.0),
                                          Number(LN_ValueType::Float, -INFINITY)};
  EXPECT_EQ(LN_ListStatisticOf(infinite, LN_ListStatistic::Min, index), -INFINITY);
  EXPECT_EQ(index, 1);
}

namespace {

/** The game property as Logic Nodes read it (every field). */
std::string Stored(KX_GameObject &object, const char *name)
{
  EXP_Value *property = object.GetProperty(name);
  if (property == nullptr) {
    return "<none>";
  }
  LN_DumpNames names;
  return LN_DumpValue(LN_GamePropertyValue(property, nullptr), names);
}

void Put(KX_GameObject &object, const char *name, const LN_Value &value)
{
  std::string reason;
  object.RemoveProperty(name);
  ASSERT_TRUE(LN_CommandBuffer::ApplyGamePropertyValue(&object, name, value, nullptr, reason))
      << reason;
}

/** `value` as Stored gives it once written into a property. */
std::string AsStored(KX_GameObject &object, const LN_Value &value)
{
  Put(object, "__expected", value);
  return Stored(object, "__expected");
}

constexpr int16_t kReverse = 0, kShuffle = 1, kSlice = 2, kMin = 3, kMax = 4, kSum = 5,
                  kAverage = 6;

/** Every operation over the owner's game property "src", into game properties; Insert's Output
 * Only output too. Shuffle three times: Per Object, Same for All, and Per Object with Seed. */
std::shared_ptr<LN_Program> ListOpsProgram()
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  const auto read = [&](const char *idname, const char *property) -> GenNode & {
    GenNode &node = b.Add(idname);
    b.String(node, "Property", property);
    return node;
  };
  GenNode &source = read("LogicNativeGetGameProperty", "src");
  GenNode &start = read("LogicNativeGetGamePropertyInt", "start");
  GenNode &length = read("LogicNativeGetGamePropertyInt", "length");
  GenNode &seed = read("LogicNativeGetGamePropertyInt", "seed");
  GenNode &index = read("LogicNativeGetGamePropertyInt", "index");
  GenNode &item = read("LogicNativeGetGameProperty", "item");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  const auto write = [&](const char *idname, const char *property, GenNode &from, const char *pin) {
    GenNode &set = b.Add(idname);
    b.String(set, "Property", property);
    b.Link(from, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  };
  const auto operation = [&](const int16_t op, const int16_t variation = 0) -> GenNode & {
    GenNode &node = b.Add("LogicNativeListOperation", op, variation);
    b.Link(source, "Value", node, "List");
    return node;
  };
  write("LogicNativeSetGameProperty", "reversed", operation(kReverse), "List");
  write("LogicNativeSetGameProperty", "shuffled", operation(kShuffle), "List");
  write("LogicNativeSetGameProperty", "shuffled_all", operation(kShuffle, 1), "List");
  GenNode &seeded = operation(kShuffle);
  b.Link(seed, "Value", seeded, "Seed");
  write("LogicNativeSetGameProperty", "shuffled_seed", seeded, "List");
  GenNode &slice = operation(kSlice);
  b.Link(start, "Value", slice, "Start");
  b.Link(length, "Value", slice, "Length");
  write("LogicNativeSetGameProperty", "sliced", slice, "List");
  const char *names[] = {"min", "max", "sum", "average"};
  for (int16_t op = kMin; op <= kAverage; op++) {
    GenNode &node = operation(op);
    write("LogicNativeSetGamePropertyFloat", names[op - kMin], node, "Value");
    if (op == kMin || op == kMax) {
      write("LogicNativeSetGamePropertyInt",
            (std::string(names[op - kMin]) + "_at").c_str(),
            node,
            "Index");
    }
  }
  GenNode &insert = b.Add("LogicNativeListInsert", LN_COLLECTION_STORE_OUTPUT_ONLY);
  b.Link(step, "Out", insert, "Flow");
  b.Link(source, "Value", insert, "List");
  b.Link(index, "Value", insert, "Index");
  b.Link(item, "Value", insert, "Value");
  write("LogicNativeSetGameProperty", "inserted", insert, "List");
  /* Outputs an operation hides give their defaults (a link left after a change still runs). */
  GenNode &hidden = operation(kReverse);
  write("LogicNativeSetGamePropertyFloat", "hidden_value", hidden, "Value");
  write("LogicNativeSetGamePropertyInt", "hidden_index", hidden, "Index");
  write("LogicNativeSetGameProperty", "hidden_list", operation(kSum), "List");
  write("LogicNativeSetGamePropertyInt", "hidden_sum_index", operation(kSum), "Index");
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

struct ListCase {
  std::vector<LN_Value> items;
  int32_t start, length, seed, index;
};

std::vector<ListCase> ListCases()
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<ListCase> cases;
  std::vector<LN_Value> ten;
  for (int k = 0; k < 10; k++) {
    ten.push_back(Number(k % 2 == 0 ? LN_ValueType::Int : LN_ValueType::Float, k * 1.5 - 4.0));
  }
  cases.push_back({ten, 2, 3, 0, 4});
  cases.push_back({ten, -3, -1, 7, -1});
  cases.push_back({{Text("sword"), Number(LN_ValueType::Int, 5), Text("potion"),
                    Number(LN_ValueType::Float, nan), Number(LN_ValueType::Float, 2.25)},
                   1,
                   9,
                   1,
                   99});
  cases.push_back({{}, 0, 2, 0, 0});
  cases.push_back({{Number(LN_ValueType::Int, -3)}, -9, 1, 3, -9});
  cases.push_back({ten, 0, 0, 2, 10});
  return cases;
}

std::vector<std::string> SortedDumps(const std::vector<LN_Value> &items)
{
  LN_DumpNames names;
  std::vector<std::string> dumps;
  for (const LN_Value &item : items) {
    dumps.push_back(LN_DumpValue(item, names));
  }
  std::sort(dumps.begin(), dumps.end());
  return dumps;
}

const char *const kListOutputs[] = {"reversed",
                                    "shuffled",
                                    "shuffled_all",
                                    "shuffled_seed",
                                    "sliced",
                                    "inserted",
                                    "hidden_list",
                                    "min",
                                    "max",
                                    "sum",
                                    "average",
                                    "min_at",
                                    "max_at",
                                    "hidden_value",
                                    "hidden_index",
                                    "hidden_sum_index"};

}  // namespace

/* The nodes give the rules' answers in every mode, a wide run the column passes'; Shuffle gives
 * a new order every step, the same in every mode, a permutation of the list. */
TEST(LN_ListOps, ListOperationNodesGiveTheRulesAnswers)
{
  const std::shared_ptr<LN_Program> program = ListOpsProgram();
  ASSERT_NE(program, nullptr);
  const std::vector<ListCase> cases = ListCases();
  const uint32_t owners = uint32_t(cases.size());
  const auto run = [&](SemRig &rig, const int steps) {
    for (uint32_t index = 0; index < owners; index++) {
      const ListCase &c = cases[index];
      KX_GameObject &object = rig.Object(index);
      Put(object, "src", ListOf(c.items));
      Put(object, "item", Text("new"));
      rig.SetInt(object, "start", c.start);
      rig.SetInt(object, "length", c.length);
      rig.SetInt(object, "seed", c.seed);
      rig.SetInt(object, "index", c.index);
      for (const char *name : kListOutputs) {
        /* (Made by the writes: a write never changes a property's type.) */
        object.RemoveProperty(name);
      }
    }
    std::vector<std::string> values;
    for (int step = 0; step < steps; step++) {
      rig.Step();
      for (uint32_t index = 0; index < owners; index++) {
        for (const char *name : kListOutputs) {
          values.push_back(Stored(rig.Object(index), name));
        }
      }
    }
    return values;
  };
  const size_t per_owner = std::size(kListOutputs);
  std::vector<std::string> reference;
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, owners);
    const std::vector<std::string> got = run(rig, 3);
    KX_GameObject &scratch = rig.AddObject("Scratch", MT_Vector3(0.0f, 0.0f, 0.0f));
    for (int step = 0; step < 3; step++) {
      for (uint32_t index = 0; index < owners; index++) {
        const ListCase &c = cases[index];
        SCOPED_TRACE("case " + std::to_string(index) + " step " + std::to_string(step));
        const std::string *v = &got[(size_t(step) * owners + index) * per_owner];
        std::vector<LN_Value> reversed = c.items;
        std::reverse(reversed.begin(), reversed.end());
        EXPECT_EQ(v[0], AsStored(scratch, ListOf(reversed)));
        int64_t first = 0, last = 0;
        LN_ClampSlice(int64_t(c.items.size()), c.start, c.length, first, last);
        EXPECT_EQ(v[4],
                  AsStored(scratch,
                           ListOf(std::vector<LN_Value>(c.items.begin() + first,
                                                        c.items.begin() + last))));
        std::vector<LN_Value> inserted = c.items;
        inserted.insert(inserted.begin() + LN_ListInsertPlace(c.index, inserted.size()),
                        Text("new"));
        EXPECT_EQ(v[5], AsStored(scratch, ListOf(inserted)));
        EXPECT_EQ(v[6], AsStored(scratch, ListOf({}))) << "Sum's hidden List: empty";
        const LN_ListStatistic statistics[] = {LN_ListStatistic::Min,
                                               LN_ListStatistic::Max,
                                               LN_ListStatistic::Sum,
                                               LN_ListStatistic::Average};
        int32_t places[4] = {};
        for (int k = 0; k < 4; k++) {
          const float expected = LN_ListStatisticOf(c.items, statistics[k], places[k]);
          rig.SetFloat(scratch, "__number", expected);
          EXPECT_EQ(v[7 + k], Stored(scratch, "__number")) << "statistic " << k;
        }
        rig.SetInt(scratch, "__number", places[0]);
        EXPECT_EQ(v[11], Stored(scratch, "__number")) << "Min's place";
        rig.SetInt(scratch, "__number", places[1]);
        EXPECT_EQ(v[12], Stored(scratch, "__number")) << "Max's place";
        rig.SetFloat(scratch, "__number", 0.0f);
        EXPECT_EQ(v[13], Stored(scratch, "__number")) << "Reverse's hidden Value: 0";
        rig.SetInt(scratch, "__number", -1);
        EXPECT_EQ(v[14], Stored(scratch, "__number")) << "Reverse's hidden Index: -1";
        EXPECT_EQ(v[15], Stored(scratch, "__number")) << "Sum's hidden Index: -1";
      }
    }
    /* Shuffles are permutations (read back as values). */
    for (uint32_t index = 0; index < owners; index++) {
      for (int k = 1; k <= 3; k++) {
        EXP_Value *property = rig.Object(index).GetProperty(kListOutputs[k]);
        ASSERT_NE(property, nullptr);
        const LN_Value shuffled = LN_GamePropertyValue(property, nullptr);
        ASSERT_EQ(shuffled.type, LN_ValueType::List);
        EXPECT_EQ(SortedDumps(shuffled.list_value), SortedDumps(cases[index].items))
            << kListOutputs[k] << " of case " << index;
      }
    }
    if (reference.empty()) {
      reference = got;
    }
    EXPECT_EQ(got, reference);
  }
  /* The ten-item cases (0, 1 and 5) tell the shuffles apart. */
  const auto at = [&](const int step, const uint32_t owner, const int output) {
    return reference[(size_t(step) * owners + owner) * per_owner + size_t(output)];
  };
  EXPECT_NE(at(0, 0, 1), at(1, 0, 1)) << "a new order every step";
  EXPECT_NE(at(1, 0, 1), at(2, 0, 1));
  EXPECT_NE(at(0, 0, 1), at(0, 5, 1)) << "Per Object: each object its own order";
  EXPECT_EQ(at(0, 0, 2), at(0, 5, 2)) << "Same for All: one order for every object";
  EXPECT_EQ(at(2, 0, 2), at(2, 1, 2));
  EXPECT_NE(at(0, 0, 2), at(1, 0, 2));
  EXPECT_NE(at(0, 0, 1), at(0, 0, 3)) << "the Seed (0 against 0 linked) on the same object: "
                                         "another node, another order";
  EXPECT_NE(at(0, 1, 3), at(0, 5, 3)) << "seeds 7 and 2";
  SemRig wide(program, kProduction, owners);
  wide.SetColumns(true, 2, 1);
  EXPECT_EQ(run(wide, 3), reference) << "a wide run";
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(owners))
        << "the objects ran wide: " << wide.LastColumnRefusal();
  }
}

/* String Operation's newer outputs (Length, Index, List) compile when the operation hides them
 * (a link left after the operation changed, no longer drawn): the tree keeps running. The older
 * String, Result and Count keep their old rule (the input they feed takes its own value). */
TEST(LN_ListOps, HiddenStringOperationOutputsStillCompile)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &join = b.Add("LogicNativeStringOperation", 0);
  b.String(join, "String", "ab");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  for (const char *output : {"Length", "Index"}) {
    GenNode &set = b.Add("LogicNativeSetGamePropertyInt");
    b.String(set, "Property", output);
    b.Link(join, output, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  }
  GenNode &list = b.Add("LogicNativeListLength");
  b.Link(join, "List", list, "List");
  GenNode &parts = b.Add("LogicNativeSetGamePropertyInt");
  b.String(parts, "Property", "parts");
  b.Link(list, "Length", parts, "Value");
  b.Link(*flow, flow_pin, parts, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;
  SemRig rig(program, kProduction, 1);
  rig.Step();
  /* Their empty values: no length, no place, no count, no parts. */
  EXPECT_EQ(rig.Float(rig.Object(0), "Length"), 0.0f);
  EXPECT_EQ(rig.Float(rig.Object(0), "Index"), -1.0f);
  EXPECT_EQ(rig.Float(rig.Object(0), "parts"), 0.0f);
}

/* Get Children lists the scene graph's children in Get Child's order, All Descendants depth
 * first; an empty Parent is the owner, a linked one another object; Count is the list's length.
 * In every mode and in a wide run. */
TEST(LN_ListOps, GetChildrenListsTheHierarchy)
{
  constexpr int kItems = 5;
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  const auto write = [&](const char *idname, const std::string &property, GenNode &from,
                         const char *pin) {
    GenNode &set = b.Add(idname);
    b.String(set, "Property", property.c_str());
    b.Link(from, pin, set, "Value");
    b.Link(*flow, flow_pin, set, "Flow");
    flow = &set;
    flow_pin = "Done";
  };
  GenNode &named = b.Add("LogicNativeObjectByName");
  b.String(named, "Name", "A");
  GenNode &deep = b.Add("LogicNativeGetGamePropertyBool");
  b.String(deep, "Property", "deep");
  struct Source {
    const char *prefix;
    GenNode *node;
  };
  GenNode &own = b.Add("LogicNativeGetChildren");
  GenNode &own_all = b.Add("LogicNativeGetChildren");
  b.Bool(own_all, "All Descendants", true);
  GenNode &of_a = b.Add("LogicNativeGetChildren");
  b.Link(named, "Object", of_a, "Parent");
  b.Link(deep, "Value", of_a, "All Descendants");
  for (const Source &source : {Source{"own", &own}, Source{"all", &own_all}, Source{"a", &of_a}})
  {
    write("LogicNativeSetGamePropertyInt", std::string(source.prefix) + "_count", *source.node,
          "Count");
    for (int k = 0; k < kItems; k++) {
      GenNode &item = b.Add("LogicNativeListGetItem");
      b.Link(*source.node, "Children", item, "List");
      b.Int(item, "Index", k);
      GenNode &name = b.Add("LogicNativeTypecast", 2 /* to String */);
      b.Link(item, "Value", name, "Value");
      write("LogicNativeSetGamePropertyString",
            std::string(source.prefix) + std::to_string(k),
            name,
            "String");
    }
  }
  /* Get Child's Index counts the same children. */
  for (int k = 0; k < 2; k++) {
    GenNode &child = b.Add("LogicNativeGetChild");
    b.Int(child, "Index", k);
    GenNode &name = b.Add("LogicNativeTypecast", 2);
    b.Link(child, "Child Object", name, "Value");
    write("LogicNativeSetGamePropertyString", "child" + std::to_string(k), name, "String");
  }
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  ASSERT_NE(program, nullptr) << report;

  const auto build = [](SemRig &rig) {
    /* Obj0: A (with C, which has D) and B; Obj1: none; Obj2: E. */
    const size_t owners = rig.ObjectCount();
    for (const char *name : {"A", "B", "C", "D", "E"}) {
      rig.AddObject(name, MT_Vector3(0.0f, 0.0f, 0.0f));
    }
    rig.Parent(owners + 0, 0);
    rig.Parent(owners + 1, 0);
    rig.Parent(owners + 2, owners + 0);
    rig.Parent(owners + 3, owners + 2);
    rig.Parent(owners + 4, 2);
    rig.SetBool(rig.Object(0), "deep", true);
    rig.SetBool(rig.Object(1), "deep", false);
    rig.SetBool(rig.Object(2), "deep", true);
    rig.NoteObjectListChanged();
  };
  const auto read = [](SemRig &rig, const size_t owner) {
    std::string text;
    KX_GameObject &object = rig.Object(owner);
    for (const char *prefix : {"own", "all", "a"}) {
      text += std::string(prefix) + ":" + std::to_string(int(rig.Float(object,
                                                                       (std::string(prefix) +
                                                                        "_count")
                                                                           .c_str())));
      for (int k = 0; k < kItems; k++) {
        text += " " + rig.Text(object, (std::string(prefix) + std::to_string(k)).c_str());
      }
      text += "; ";
    }
    text += "child: " + rig.Text(object, "child0") + " " + rig.Text(object, "child1");
    return text;
  };
  const std::string expected[] = {
      "own:2 A B   ; all:4 A C D B ; a:2 C D   ; child: A B",
      "own:0     ; all:0     ; a:1 C    ; child:  ",
      "own:1 E    ; all:1 E    ; a:2 C D   ; child: E ",
  };
  const auto run = [&](SemRig &rig) {
    build(rig);
    rig.Step();
    rig.Step(); /* (On Init runs in the first step: the trees run wide from the second.) */
    std::vector<std::string> texts;
    for (size_t owner = 0; owner < 3; owner++) {
      texts.push_back(read(rig, owner));
    }
    return texts;
  };
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode, 3);
    const std::vector<std::string> got = run(rig);
    for (size_t owner = 0; owner < 3; owner++) {
      EXPECT_EQ(got[owner], expected[owner]) << "owner " << owner;
    }
  }
  SemRig wide(program, kProduction, 3);
  wide.SetColumns(true, 2, 1);
  const std::vector<std::string> got = run(wide);
  for (size_t owner = 0; owner < 3; owner++) {
    EXPECT_EQ(got[owner], expected[owner]) << "wide, owner " << owner;
  }
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_EQ(wide.LastColumnLanes(), uint64_t(3))
        << "the objects ran wide: " << wide.LastColumnRefusal();
  }
}

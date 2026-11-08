/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Tests for the nodes that had no test (lean runtime plan, phase A5): what each does today, so the
 * code moves of phases A11-A14 break a test if they change it. Nodes that need no scene run for
 * real in the rig of LN_SemRig.h; nodes that act on the scene run until their command is recorded
 * (the test reads the recorded command, nothing is applied). Nodes that read the scene while they
 * run (FindObject, GetScene, the material, bone and action reads ...) crash without one: the
 * acceptance probe `uncovered_nodes` covers them. A test whose name says Pins looks at a result
 * that may be a bug. */

#include "testing/testing.h"

#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <unistd.h>
#include <map>
#include <random>
#include <set>

#include "SCA_IInputDevice.h"

#include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

std::shared_ptr<LN_Program> CompileOrFail(TreeBuilder &b)
{
  std::string report;
  std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
  return program;
}

/** Writes values into game properties of the owner, one after the other, behind On Logic +
 * Physics Step. */
class Writer {
 public:
  explicit Writer(TreeBuilder &b) : m_b(b), m_event(b.Add("LogicNativeOnFixedUpdate"))
  {
    m_flow = &m_event;
    m_flow_pin = "Out";
  }
  GenNode &Event()
  {
    return m_event;
  }
  /** Runs `node` (a flow node with Flow/Done) in the chain. */
  void Run(GenNode &node)
  {
    m_b.Link(*m_flow, m_flow_pin, node, "Flow");
    m_flow = &node;
    m_flow_pin = "Done";
  }
  void Float(GenNode &source, const char *pin, const char *property)
  {
    Put("LogicNativeSetGamePropertyFloat", source, pin, property);
  }
  void Int(GenNode &source, const char *pin, const char *property)
  {
    Put("LogicNativeSetGamePropertyInt", source, pin, property);
  }
  void Bool(GenNode &source, const char *pin, const char *property)
  {
    Put("LogicNativeSetGamePropertyBool", source, pin, property);
  }
  void String(GenNode &source, const char *pin, const char *property)
  {
    Put("LogicNativeSetGamePropertyString", source, pin, property);
  }
  /** The three components of a vector output as properties name + X/Y/Z. */
  void Vector(GenNode &source, const char *pin, const std::string &property)
  {
    GenNode &split = m_b.Add("LogicNativeSeparateXYZ");
    m_b.Link(source, pin, split, "Vector");
    Float(split, "X", (property + "X").c_str());
    Float(split, "Y", (property + "Y").c_str());
    Float(split, "Z", (property + "Z").c_str());
  }

 private:
  void Put(const char *kind, GenNode &source, const char *pin, const char *property)
  {
    GenNode &node = m_b.Add(kind);
    m_b.String(node, "Property", property);
    m_b.Link(source, pin, node, "Value");
    Run(node);
  }
  TreeBuilder &m_b;
  GenNode &m_event;
  GenNode *m_flow = nullptr;
  const char *m_flow_pin = "Out";
};

/** A list node input from three int properties "a", "b", "c" (live). */
GenNode &IntList(TreeBuilder &b, const char *a = "a", const char *bb = "b", const char *c = "c")
{
  GenNode &list = b.Add("LogicNativeListFromItems", 2); /* custom1 2: the Integers input */
  for (const char *name : {a, bb, c}) {
    GenNode &read = b.Add("LogicNativeGetGamePropertyInt");
    b.String(read, "Property", name);
    b.Link(read, "Value", list, "Integers");
  }
  return list;
}

/* ---- lists ---- */

/* Lists: Sort orders ints (Descending flips), Find Index gives the first place or -1, Random Item
 * gives a member, Extend appends B to A, Remove Index / Remove Value / Set Index give the changed
 * list (the input list is not changed). */
TEST(LN_NodeCoverage, ListNodesSortFindExtendRemoveAndSet)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &list = IntList(b);
  GenNode &sort = b.Add("LogicNativeListSort");
  b.Link(list, "Integers", sort, "List");
  GenNode &sort_down = b.Add("LogicNativeListSort");
  b.Link(list, "Integers", sort_down, "List");
  b.Bool(sort_down, "Descending", true);
  const auto item = [&](GenNode &source, const char *pin, int index) -> GenNode & {
    GenNode &get = b.Add("LogicNativeListGetItem");
    b.Link(source, pin, get, "List");
    b.Int(get, "Index", index);
    return get;
  };
  for (int i = 0; i < 3; i++) {
    w.Int(item(sort, "List", i), "Value", ("up" + std::to_string(i)).c_str());
    w.Int(item(sort_down, "List", i), "Value", ("down" + std::to_string(i)).c_str());
  }
  GenNode &find = b.Add("LogicNativeListFindIndex");
  b.Link(list, "Integers", find, "List");
  GenNode &wanted = b.Add("LogicNativeGetGamePropertyInt");
  b.String(wanted, "Property", "wanted");
  b.Link(wanted, "Value", find, "Value");
  w.Int(find, "Index", "found");
  GenNode &random_item = b.Add("LogicNativeListRandomItem");
  b.Link(list, "Integers", random_item, "List");
  w.Int(random_item, "Value", "random");
  GenNode &extend = b.Add("LogicNativeListExtend");
  b.Link(list, "Integers", extend, "List A");
  b.Link(list, "Integers", extend, "List B");
  GenNode &length = b.Add("LogicNativeListLength");
  b.Link(extend, "List", length, "List");
  w.Int(length, "Length", "extended");
  GenNode &remove_index = b.Add("LogicNativeListRemoveIndex");
  b.Link(list, "Integers", remove_index, "List");
  b.Int(remove_index, "Index", 1);
  w.Run(remove_index);
  GenNode &after_remove = item(remove_index, "List", 1);
  w.Int(after_remove, "Value", "after_remove_index");
  GenNode &remove_value = b.Add("LogicNativeListRemoveValue");
  b.Link(list, "Integers", remove_value, "List");
  b.Link(wanted, "Value", remove_value, "Value");
  w.Run(remove_value);
  GenNode &length_after_value = b.Add("LogicNativeListLength");
  b.Link(remove_value, "List", length_after_value, "List");
  w.Int(length_after_value, "Length", "len_after_remove_value");
  GenNode &set_index = b.Add("LogicNativeListSetIndex");
  b.Link(list, "Integers", set_index, "List");
  b.Int(set_index, "Index", 0);
  b.Link(wanted, "Value", set_index, "Value");
  w.Run(set_index);
  w.Int(item(set_index, "List", 0), "Value", "set_zero");
  w.Int(item(list, "Integers", 0), "Value", "original_zero");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  KX_GameObject &o = rig.Object(0);
  rig.SetInt(o, "a", 30);
  rig.SetInt(o, "b", 10);
  rig.SetInt(o, "c", 20);
  rig.SetInt(o, "wanted", 20);
  rig.Step();
  EXPECT_EQ(rig.Float(o, "up0"), 10.0f);
  EXPECT_EQ(rig.Float(o, "up1"), 20.0f);
  EXPECT_EQ(rig.Float(o, "up2"), 30.0f);
  EXPECT_EQ(rig.Float(o, "down0"), 30.0f);
  EXPECT_EQ(rig.Float(o, "down2"), 10.0f);
  EXPECT_EQ(rig.Float(o, "found"), 2.0f);
  const float random = rig.Float(o, "random");
  EXPECT_TRUE(random == 30.0f || random == 10.0f || random == 20.0f) << random;
  EXPECT_EQ(rig.Float(o, "extended"), 6.0f);
  EXPECT_EQ(rig.Float(o, "after_remove_index"), 20.0f) << "[30, 20] after removing place 1";
  EXPECT_EQ(rig.Float(o, "len_after_remove_value"), 2.0f);
  EXPECT_EQ(rig.Float(o, "set_zero"), 20.0f);
  EXPECT_EQ(rig.Float(o, "original_zero"), 30.0f) << "the input list is not changed";
  /* A value not in the list: Find Index gives -1. */
  rig.SetInt(o, "wanted", 99);
  rig.Step();
  EXPECT_EQ(rig.Float(o, "found"), -1.0f);
}


/* Modify Property with Clamp: the operation is applied to the live value when the write flushes and
 * the result is held between Min and Max (Min and Max are read as given; a planted swap of the two
 * reads in the direct handler was not caught by any test before this one, plan phase A12). */
TEST(LN_NodeCoverage, ModifyPropertyClampsItsResultBetweenMinAndMax)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &modify = b.Add("LogicNativeModifyProperty", blender::NODE_MATH_ADD, 1);
  b.String(modify, "Property", "m");
  b.Float(modify, "Value", 2.0f);
  b.Float(modify, "Min", 1.0f);
  b.Float(modify, "Max", 5.0f);
  b.Link(event, "Out", modify, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  KX_GameObject &o = rig.Object(0);
  rig.SetFloat(o, "m", 0.0f);
  rig.Step();
  EXPECT_EQ(rig.Float(o, "m"), 2.0f) << "0 + 2, inside the range";
  rig.Step();
  EXPECT_EQ(rig.Float(o, "m"), 4.0f);
  rig.Step();
  EXPECT_EQ(rig.Float(o, "m"), 5.0f) << "4 + 2 is held at Max";
  rig.SetFloat(o, "m", -10.0f);
  rig.Step();
  EXPECT_EQ(rig.Float(o, "m"), 1.0f) << "-10 + 2 is held at Min";
}

/* ---- dictionaries ---- */

/* Dictionary nodes give a new dictionary and leave the input as it was: Add Value adds a number to a
 * key's value, Remove Key gives the dictionary without it and the removed value. */
TEST(LN_NodeCoverage, DictionaryNodesAddAndRemoveWithoutChangingTheirInput)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &v = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(v, "Property", "v");
  GenNode &make = b.Add("LogicNativeMakeDict");
  b.String(make, "Key", "k");
  b.Link(v, "Value", make, "Value");
  GenNode &add = b.Add("LogicNativeDictAddValue");
  b.Link(make, "Dictionary", add, "Dictionary");
  b.String(add, "Key", "k");
  b.Float(add, "Amount", 5.0f);
  w.Run(add);
  GenNode &get = b.Add("LogicNativeDictGetKey");
  b.Link(add, "Dictionary", get, "Dictionary");
  b.String(get, "Key", "k");
  w.Float(get, "Value", "added");
  GenNode &get_input = b.Add("LogicNativeDictGetKey");
  b.Link(make, "Dictionary", get_input, "Dictionary");
  b.String(get_input, "Key", "k");
  w.Float(get_input, "Value", "input_still");
  GenNode &remove = b.Add("LogicNativeDictRemoveKey");
  b.Link(add, "Dictionary", remove, "Dictionary");
  b.String(remove, "Key", "k");
  w.Run(remove);
  w.Float(remove, "Value", "removed");
  GenNode &length = b.Add("LogicNativeDictLength");
  b.Link(remove, "Dictionary", length, "Dictionary");
  w.Int(length, "Length", "len_after");
  GenNode &length_before = b.Add("LogicNativeDictLength");
  b.Link(add, "Dictionary", length_before, "Dictionary");
  w.Int(length_before, "Length", "len_before");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  KX_GameObject &o = rig.Object(0);
  rig.SetFloat(o, "v", 2.0f);
  rig.Step();
  EXPECT_EQ(rig.Float(o, "added"), 7.0f);
  EXPECT_EQ(rig.Float(o, "input_still"), 2.0f);
  EXPECT_EQ(rig.Float(o, "removed"), 7.0f);
  EXPECT_EQ(rig.Float(o, "len_after"), 0.0f);
  EXPECT_EQ(rig.Float(o, "len_before"), 1.0f);
}

/* ---- value, switch and conversion nodes ---- */

/* Value String keeps its text on its output; Key Code gives the engine's key numbers; File Path
 * passes a path through, Get Master Folder gives "" without a loaded file; Delta Factor is 1 at
 * normal time; the cursor, mouse and keyboard reads are 0 / false without an input device. */
TEST(LN_NodeCoverage, ValueStringKeyCodeFilePathAndTheInputReadsWithoutADevice)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &vs = b.Add("LogicNativeValueString");
  b.OutputString(vs, "String", "stored text");
  w.String(vs, "String", "vs");
  GenNode &key_a = b.Add("LogicNativeKeyCode");
  w.Int(key_a, "Code", "key_a");
  GenNode &key_space = b.Add("LogicNativeKeyCode");
  b.String(key_space, "Key", "SPACE");
  w.Int(key_space, "Code", "key_space");
  GenNode &path = b.Add("LogicNativeFilePath");
  b.String(path, "Value", "dir/file.txt");
  w.String(path, "Path", "path");
  GenNode &master = b.Add("LogicNativeGetMasterFolder");
  w.String(master, "Path", "master");
  GenNode &delta = b.Add("LogicNativeTime"); /* its Delta Factor (the Delta Factor node until
                                               * 2026-11-08) */
  w.Float(delta, "Delta Factor", "delta");
  GenNode &keyboard = b.Add("LogicNativeKeyboardActive");
  w.Bool(keyboard, "Active", "keyboard");
  GenNode &movement = b.Add("LogicNativeCursorMovement");
  w.Float(movement, "X", "move_x");
  GenNode &position = b.Add("LogicNativeCursorPosition");
  w.Float(position, "X", "cursor_x");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    SemRig rig(program, mode);
    KX_GameObject &o = rig.Object(0);
    rig.Step();
    EXPECT_EQ(rig.Text(o, "vs"), "stored text");
    EXPECT_EQ(rig.Float(o, "key_a"), float(SCA_IInputDevice::AKEY));
    EXPECT_EQ(rig.Float(o, "key_space"), float(SCA_IInputDevice::SPACEKEY));
    EXPECT_EQ(rig.Text(o, "path"), "dir/file.txt");
    EXPECT_EQ(rig.Text(o, "master"), "");
    EXPECT_EQ(rig.Float(o, "delta"), 1.0f);
    EXPECT_EQ(rig.Text(o, "keyboard"), "FALSE");
    EXPECT_EQ(rig.Float(o, "move_x"), 0.0f);
    EXPECT_EQ(rig.Float(o, "cursor_x"), 0.0f);
  }
}

/* Not None (Is None's option; the Value Valid node did the same until 2026-11-08) says TRUE for a
 * value that exists. (It also says TRUE for a tree property that was never set: it tests that a
 * value was produced, not that one was set.) */
TEST(LN_NodeCoverage, PinsNotNoneIsTrueEvenForATreePropertyNeverSet)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &v = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(v, "Property", "v");
  GenNode &valid = b.Add("LogicNativeNotNone");
  b.Link(v, "Value", valid, "Value");
  w.Bool(valid, "If Not None", "valid");
  GenNode &tree_property = b.Add("LogicNativeGetTreeProperty");
  b.String(tree_property, "Property", "never_set");
  GenNode &valid_unset = b.Add("LogicNativeNotNone");
  b.Link(tree_property, "Value", valid_unset, "Value");
  w.Bool(valid_unset, "If Not None", "valid_unset");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  rig.SetFloat(rig.Object(0), "v", 2.0f);
  rig.Step();
  EXPECT_EQ(rig.Text(rig.Object(0), "valid"), "TRUE");
  EXPECT_EQ(rig.Text(rig.Object(0), "valid_unset"), "TRUE");
}

/* Value Switch List gives the value of the first true condition (0 when none is); Value Switch List
 * Compare gives the value of the first case equal to the switch, else the default. */
TEST(LN_NodeCoverage, ValueSwitchListsPickTheFirstMatch)
{
  TreeBuilder b;
  Writer w(b);
  const auto prop = [&](const char *kind, const char *name) -> GenNode & {
    GenNode &read = b.Add(kind);
    b.String(read, "Property", name);
    return read;
  };
  GenNode &sl = b.Add("LogicNativeValueSwitchList");
  b.Link(prop("LogicNativeGetGamePropertyBool", "ca"), "Value", sl, "if A");
  b.Link(prop("LogicNativeGetGamePropertyFloat", "va"), "Value", sl, "Value A");
  b.Link(prop("LogicNativeGetGamePropertyBool", "cb"), "Value", sl, "elif B");
  b.Link(prop("LogicNativeGetGamePropertyFloat", "vb"), "Value", sl, "Value B");
  w.Float(sl, "Result", "sl");
  GenNode &slc = b.Add("LogicNativeValueSwitchListCompare");
  b.Link(prop("LogicNativeGetGamePropertyFloat", "sw"), "Value", slc, "Switch");
  b.Link(prop("LogicNativeGetGamePropertyFloat", "va"), "Value", slc, "Default");
  b.Link(prop("LogicNativeGetGamePropertyFloat", "c1"), "Value", slc, "Case A");
  b.Link(prop("LogicNativeGetGamePropertyFloat", "vb"), "Value", slc, "Value A");
  w.Float(slc, "Result", "slc");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  KX_GameObject &o = rig.Object(0);
  const struct {
    bool a, b;
    float sw, sl, slc;
  } rows[] = {{true, true, 1, 11, 11}, {false, true, 5, 22, 22}, {false, false, 5, 0, 22}, {true, false, 7, 11, 11}};
  for (const auto &row : rows) {
    rig.SetBool(o, "ca", row.a);
    rig.SetBool(o, "cb", row.b);
    rig.SetFloat(o, "va", 11.0f);
    rig.SetFloat(o, "vb", 22.0f);
    rig.SetFloat(o, "c1", 5.0f);
    rig.SetFloat(o, "sw", row.sw);
    rig.Step();
    EXPECT_EQ(rig.Float(o, "sl"), row.sl);
    EXPECT_EQ(rig.Float(o, "slc"), row.slc);
  }
}

/* Vector Rotate turns Origin around Pivot's axis by Angle; Vector To Rotation makes the rotation
 * that points the local axis at Direction (here the local X axis at +Y: a quarter turn about Z);
 * Random Value stays in [Min, Max); Toggle Tree Property flips a bool tree property each time. */
TEST(LN_NodeCoverage, VectorRotateVectorToRotationRandomValueAndToggleTreeProperty)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &rotate = b.Add("LogicNativeVectorRotate");
  b.Vector(rotate, "Origin", 1.0f, 0.0f, 0.0f);
  b.Float(rotate, "Angle", 1.5707964f);
  w.Vector(rotate, "Point", "vr");
  GenNode &random = b.Add("LogicNativeRandomValue");
  b.Float(random, "Min", 2.0f);
  b.Float(random, "Max", 3.0f);
  w.Float(random, "Value", "rv");
  GenNode &toggle = b.Add("LogicNativeToggleTreeProperty");
  b.String(toggle, "Property", "flag");
  w.Run(toggle);
  GenNode &flag = b.Add("LogicNativeGetTreeProperty");
  b.String(flag, "Property", "flag");
  w.String(flag, "Value", "flag_text");
  GenNode &to_rotation = b.Add("LogicNativeVectorToRotation");
  b.Vector(to_rotation, "Direction", 0.0f, 1.0f, 0.0f);
  b.Vector(to_rotation, "Up", 0.0f, 0.0f, 1.0f);
  GenNode &turn = b.Add("LogicNativeApplyRotation");
  b.Link(to_rotation, "Rotation", turn, "Rotation");
  b.Bool(turn, "Local", true);
  w.Run(turn);
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  KX_GameObject &o = rig.Object(0);
  std::set<std::string> numbers;
  for (int step = 0; step < 4; step++) {
    rig.Step();
    EXPECT_NEAR(rig.Float(o, "vrX"), 0.0f, 1.0e-6f);
    EXPECT_NEAR(rig.Float(o, "vrY"), 1.0f, 1.0e-6f);
    EXPECT_NEAR(rig.Float(o, "vrZ"), 0.0f, 1.0e-6f);
    EXPECT_GE(rig.Float(o, "rv"), 2.0f);
    EXPECT_LT(rig.Float(o, "rv"), 3.0f);
    numbers.insert(Bits(rig.Float(o, "rv")));
    EXPECT_EQ(rig.Text(o, "flag_text"), step % 2 == 0 ? "True" : "False");
  }
  EXPECT_EQ(numbers.size(), 4u);
  /* Four quarter turns of +90 degrees about Z make a full turn: the X axis is back. */
  const MT_Matrix3x3 m = o.NodeGetLocalOrientation();
  EXPECT_NEAR(m[0][0], 1.0f, 1.0e-4f);
  EXPECT_NEAR(m[1][0], 0.0f, 1.0e-4f);
}


/* ---- variables on disk (the Save / Load / Remove / List / Clear nodes need no scene) ---- */

/* Save Variable writes a value to a file, Load Variable reads it back (or gives the default),
 * Remove Variable and Clear Variables take them out, List Saved Variables lists the names; the
 * dictionary versions save and load a whole dictionary. The file is under a folder made for the
 * test. */
TEST(LN_NodeCoverage, VariableNodesSaveLoadRemoveListAndClear)
{
  const std::filesystem::path folder = std::filesystem::temp_directory_path() /
                                       ("ln_node_coverage_" + std::to_string(::getpid()));
  std::filesystem::create_directories(folder);
  const std::string path = folder.string();
  TreeBuilder b;
  Writer w(b);
  const auto with_file = [&](GenNode &node) {
    b.String(node, "Path", path.c_str());
    b.String(node, "File", "vars");
  };
  GenNode &v = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(v, "Property", "v");
  GenNode &save = b.Add("LogicNativeSaveVariable");
  with_file(save);
  b.String(save, "Name", "score");
  b.Link(v, "Value", save, "Value");
  w.Run(save);
  GenNode &load = b.Add("LogicNativeLoadVariable");
  with_file(load);
  b.String(load, "Name", "score");
  w.Float(load, "Value", "loaded");
  GenNode &list = b.Add("LogicNativeListSavedVariables");
  with_file(list);
  w.Run(list);
  GenNode &count = b.Add("LogicNativeListLength");
  b.Link(list, "List", count, "List");
  w.Int(count, "Length", "saved_count");
  GenNode &dict = b.Add("LogicNativeMakeDict");
  b.String(dict, "Key", "inner");
  b.Link(v, "Value", dict, "Value");
  GenNode &save_dict = b.Add("LogicNativeSaveVariableDict");
  with_file(save_dict);
  b.Link(dict, "Dictionary", save_dict, "Variables");
  w.Run(save_dict);
  GenNode &load_dict = b.Add("LogicNativeLoadVariableDict");
  with_file(load_dict);
  GenNode &dict_length = b.Add("LogicNativeDictLength");
  b.Link(load_dict, "Variables", dict_length, "Dictionary");
  w.Int(dict_length, "Length", "dict_keys");
  GenNode &remove = b.Add("LogicNativeRemoveVariable");
  with_file(remove);
  b.String(remove, "Name", "score");
  w.Run(remove);
  GenNode &after = b.Add("LogicNativeLoadVariable");
  with_file(after);
  b.String(after, "Name", "score");
  w.Float(after, "Value", "after_remove");
  GenNode &clear = b.Add("LogicNativeClearVariables");
  with_file(clear);
  w.Run(clear);
  GenNode &list_after = b.Add("LogicNativeListSavedVariables");
  with_file(list_after);
  w.Run(list_after);
  GenNode &count_after = b.Add("LogicNativeListLength");
  b.Link(list_after, "List", count_after, "List");
  w.Int(count_after, "Length", "saved_count_after_clear");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  {
    SemRig rig(program, kMemoOff);
    KX_GameObject &o = rig.Object(0);
    rig.SetFloat(o, "v", 6.5f);
    rig.Step();
    EXPECT_EQ(rig.Float(o, "loaded"), 6.5f);
    EXPECT_EQ(rig.Float(o, "saved_count"), 1.0f);
    /* Save Variable Dict replaces the file's content with the dictionary (the "score" variable is
     * gone, the one key "inner" is there). */
    EXPECT_EQ(rig.Float(o, "dict_keys"), 1.0f);
    EXPECT_EQ(rig.Float(o, "after_remove"), 0.0f) << "a removed variable loads as its default";
    EXPECT_EQ(rig.Float(o, "saved_count_after_clear"), 0.0f);
  }
  std::filesystem::remove_all(folder);
}

/* Save Variable Dict of a value that is no dictionary (a missing property's None, a number) leaves
 * the file as it was (2026-11-08: it wrote an empty dictionary over the whole file). */
TEST(LN_NodeCoverage, SaveVariableDictOfNoDictionaryKeepsTheFile)
{
  const std::filesystem::path folder = std::filesystem::temp_directory_path() /
                                       ("ln_node_coverage_dict_" + std::to_string(::getpid()));
  std::filesystem::create_directories(folder);
  const std::string path = folder.string();
  for (const bool from_number : {false, true}) {
    TreeBuilder b;
    Writer w(b);
    const auto with_file = [&](GenNode &node) {
      b.String(node, "Path", path.c_str());
      b.String(node, "File", "keep");
    };
    GenNode &v = b.Add("LogicNativeGetGamePropertyFloat");
    b.String(v, "Property", "v");
    GenNode &save = b.Add("LogicNativeSaveVariable");
    with_file(save);
    b.String(save, "Name", "score");
    b.Link(v, "Value", save, "Value");
    w.Run(save);
    GenNode &not_a_dict = b.Add("LogicNativeGetGameProperty");
    b.String(not_a_dict, "Property", from_number ? "v" : "missing");
    GenNode &save_dict = b.Add("LogicNativeSaveVariableDict");
    with_file(save_dict);
    b.Link(not_a_dict, "Value", save_dict, "Variables");
    w.Run(save_dict);
    GenNode &load = b.Add("LogicNativeLoadVariable");
    with_file(load);
    b.String(load, "Name", "score");
    w.Float(load, "Value", "loaded");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kMemoOff);
    KX_GameObject &o = rig.Object(0);
    rig.SetFloat(o, "v", 6.5f);
    rig.Step();
    EXPECT_EQ(rig.Float(o, "loaded"), 6.5f) << (from_number ? "a number" : "a missing property");
  }
  std::filesystem::remove_all(folder);
}


/* ---- nodes that act on the scene: their command is recorded (nothing is applied) ---- */

/** The command types a node records when a tree runs it once with its pins at the defaults. */
std::string RecordedTypes(const char *idname, const int16_t c1 = 0, const int16_t c2 = 0)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &node = b.Add(idname, c1, c2);
  b.Link(event, "Out", node, "Flow");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  if (program == nullptr) {
    return "no program: " + report.substr(0, report.find('\n'));
  }
  SemRig rig(program, kProduction);
  rig.SetFlush(false);
  rig.Step();
  std::string types;
  const std::string &dump = rig.LastRecorded();
  for (size_t at = dump.find("type="); at != std::string::npos; at = dump.find("type=", at + 1)) {
    if (!types.empty()) {
      types += " ";
    }
    types += dump.substr(at + 5, dump.find(' ', at) - at - 5);
  }
  return types;
}

/* A command node records its command when its Flow arrives. */
TEST(LN_NodeCoverage, CommandNodesRecordTheirCommand)
{
  const struct {
    const char *idname;
    const char *command;
  } expected[] = {
      {"LogicNativeGamepadVibration", "SetGamepadVibration"},
      {"LogicNativeLoadBlendFile", "LoadBlendFile"},
      {"LogicNativeRebuildCollisionShape", "RebuildCollisionShape"},
      {"LogicNativeSetActionFrame", "SetActionFrame"},
      {"LogicNativeSetBoneConstraintAttribute", "SetBoneConstraintAttribute"},
      {"LogicNativeSetBoneConstraintInfluence", "SetBoneConstraintInfluence"},
      {"LogicNativeSetBoneConstraintTarget", "SetBoneConstraintTarget"},
      {"LogicNativeSetRigidBodyAttribute", "SetRigidBodyAttribute"},
      {"LogicNativeToggleTreeProperty", "SetTreeProperty"},
      {"LogicNativeVehicleAccelerate", "VehicleApplyEngineForce"},
      {"LogicNativeVehicleBrake", "VehicleApplyBraking"},
      {"LogicNativeVehicleControl", "VehicleControl"},
      {"LogicNativeVehicleSteer", "VehicleApplySteering"},
  };
  for (const auto &entry : expected) {
    SCOPED_TRACE(entry.idname);
    EXPECT_EQ(RecordedTypes(entry.idname), entry.command);
  }
}

/* These nodes run without a scene and record nothing: their work happens in the scene they act on
 * (drawing, rays, physics, variables on a path, logic trees, node muting). The acceptance probe
 * `uncovered_nodes` checks their effect. */
TEST(LN_NodeCoverage, SceneNodesWithoutASceneRunAndRecordNothing)
{
  for (const char *idname : {"LogicNativeApplyForceToTarget",
                             "LogicNativeCameraRay",
                             "LogicNativeDraw",
                             "LogicNativeFollowPath",
                             "LogicNativeMouseRay",
                             "LogicNativeSetNodeMute",
                             "LogicNativeStartLogicTree",
                             "LogicNativeVehicleSetAttributes"})
  {
    SCOPED_TRACE(idname);
    EXPECT_EQ(RecordedTypes(idname), "");
  }
}

/* Nodes that do not compile as the editor makes them. Pinned so that a fix is a deliberate change:
 * - Load File Content and Play Material Sequence have no runtime command ("unavailable in this
 *   release"; the Add menu already hides them as deferred nodes);
 * - Set Bone Attribute needs a setting (selection) the default lacks (a design choice: the user
 *   picks one, plan phase A15 left it). Start Speaker, the other one, was removed 2026-11-08: Play
 *   Sound 3D with no Sound plays the Speaker object's own sound. */
TEST(LN_NodeCoverage, PinsNodesThatDoNotCompileAsTheEditorMakesThem)
{
  const struct {
    const char *idname;
    const char *error;
  } expected[] = {
      {"LogicNativeLoadFileContent", "unavailable in this release"},
      {"LogicNativePlayMaterialSequence", "unavailable in this release"},
      {"LogicNativeSetBoneAttribute", "Unsupported Set Bone Attribute selection"},
  };
  for (const auto &entry : expected) {
    SCOPED_TRACE(entry.idname);
    const std::string result = RecordedTypes(entry.idname);
    EXPECT_NE(result.find("no program"), std::string::npos) << result;
    EXPECT_NE(result.find(entry.error), std::string::npos) << result;
  }
}

/* Value Color keeps the color on its output (all four channels); List Global Category gives the
 * values Set Global Property kept in memory (a category nobody set gives an empty dictionary); Mouse
 * Moved pulses nothing without a mouse. */
TEST(LN_NodeCoverage, ValueColorListGlobalCategoryAndMouseMoved)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &color = b.Add("LogicNativeValueColor");
  b.OutputColor(color, "Value", 0.25f, 0.5f, 0.75f, 1.0f);
  GenNode &keep = b.Add("LogicNativeSetGameProperty");
  b.String(keep, "Property", "kept_color");
  b.Link(color, "Value", keep, "Value");
  w.Run(keep);
  GenNode &v = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(v, "Property", "v");
  GenNode &set = b.Add("LogicNativeSetGlobalProperty");
  b.String(set, "Category", "ln_cov_category");
  b.String(set, "Property", "score");
  b.Link(v, "Value", set, "Value");
  w.Run(set);
  GenNode &list = b.Add("LogicNativeListGlobalProperties");
  b.String(list, "Category", "ln_cov_category");
  w.Run(list);
  GenNode &length = b.Add("LogicNativeDictLength");
  b.Link(list, "Value", length, "Dictionary");
  w.Int(length, "Length", "listed");
  GenNode &get = b.Add("LogicNativeDictGetKey");
  b.Link(list, "Value", get, "Dictionary");
  b.String(get, "Key", "score");
  w.Float(get, "Value", "score");
  GenNode &none = b.Add("LogicNativeListGlobalProperties");
  b.String(none, "Category", "ln_cov_nobody_set_this");
  w.Run(none);
  GenNode &none_length = b.Add("LogicNativeDictLength");
  b.Link(none, "Value", none_length, "Dictionary");
  w.Int(none_length, "Length", "listed_none");
  GenNode &moved = b.Add("LogicNativeMouseMoved");
  GenNode &moved_set = b.Add("LogicNativeSetGamePropertyBool");
  b.String(moved_set, "Property", "moved");
  b.Bool(moved_set, "Value", true);
  b.Link(moved, "If Moved", moved_set, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  KX_GameObject &o = rig.Object(0);
  rig.SetFloat(o, "v", 4.5f);
  rig.Step();
  rig.Step();
  EXPECT_EQ(rig.Text(o, "kept_color"), "(0.25, 0.5, 0.75, 1)");
  /* The set and the list run in one step and apply at its end, so the list of the first step is
   * empty and the second step lists the value. */
  EXPECT_EQ(rig.Float(o, "listed"), 1.0f);
  EXPECT_EQ(rig.Float(o, "score"), 4.5f);
  EXPECT_EQ(rig.Float(o, "listed_none"), 0.0f);
  EXPECT_FALSE(rig.Has(o, "moved"));
}

/* Get Sound and Get Image are pickers: their output is the datablock picked in the node, and no
 * other node has a Sound or Image input to read it (Play Sound and the others pick their own sound
 * in the node), so nothing at runtime can show what they give. A tree that holds them compiles. */
TEST(LN_NodeCoverage, PickerNodesGetSoundAndGetImageHaveNoReaderAndCompile)
{
  TreeBuilder b;
  b.Add("LogicNativeOnFixedUpdate");
  b.Add("LogicNativeGetSound");
  b.Add("LogicNativeGetImage");
  std::string report;
  const std::shared_ptr<LN_Program> program = b.Compile(&report);
  EXPECT_NE(program, nullptr) << report;
}

/* Get Scene gives the owner's scene: none without one (the rig's objects have no scene), so Value
 * Valid says false. Its value in a game (the scene by name) is read by nothing a probe can see
 * without changing scenes. (Plan C2 P0 found Get Scene in no test: lean_plan/uncovered.txt said
 * this file's scene-node test ran it, which it did not.) */
TEST(LN_NodeCoverage, GetSceneGivesNoSceneWithoutOne)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &scene = b.Add("LogicNativeGetScene");
  GenNode &valid = b.Add("LogicNativeNotNone");
  b.Link(scene, "Scene", valid, "Value");
  w.Bool(valid, "If Not None", "valid");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kProduction);
  KX_GameObject &o = rig.Object(0);
  rig.SetBool(o, "valid", true);
  rig.Step();
  EXPECT_EQ(rig.Float(o, "valid"), 0.0f);
}

/* Set VSync's From Input (3, 2026-11-08) takes the mode from its Mode input, linked to a value
 * read while the game runs; the dropdown's modes stay constants. */
TEST(LN_NodeCoverage, SetVSyncFromInputReadsItsModeInput)
{
  for (const int16_t mode : {int16_t(1), int16_t(3)}) {
    TreeBuilder b;
    Writer w(b);
    GenNode &vsync = b.Add("LogicNativeSetVSync", mode);
    GenNode &read = b.Add("LogicNativeGetGamePropertyInt");
    b.String(read, "Property", "vsync");
    b.Link(read, "Value", vsync, "Mode");
    w.Run(vsync);
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    const LN_Instruction *set = nullptr;
    for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
      if (instruction.opcode == LN_OpCode::SetVSync) {
        set = &instruction;
      }
    }
    ASSERT_NE(set, nullptr);
    ASSERT_LT(set->int_expr_index, program->GetIntExpressions().size());
    const LN_IntExpression &expression = program->GetIntExpressions()[set->int_expr_index];
    if (mode == 3) {
      EXPECT_NE(expression.kind, LN_IntExpressionKind::Constant) << "the Mode input, read live";
    }
    else {
      EXPECT_EQ(expression.kind, LN_IntExpressionKind::Constant);
      EXPECT_EQ(expression.int_value, 1);
    }
  }
}

/* Mouse Ray, Camera Ray and Projectile Ray cast with their Flow unlinked (2026-11-08; their
 * condition was a constant false, so they never cast, though the tooltip calls Flow optional):
 * unlinked, the condition is a constant true, as the Raycast family's. */
TEST(LN_NodeCoverage, RaysWithoutAFlowCastWhenTheirOutputIsRead)
{
  for (const char *idname :
       {"LogicNativeMouseRay", "LogicNativeCameraRay", "LogicNativeProjectileRay"})
  {
    SCOPED_TRACE(idname);
    TreeBuilder b;
    /* (Mask is a Collision Layers socket: an Int socket as the editor makes it.) */
    b.SetRealSockets(true);
    Writer w(b);
    GenNode &ray = b.Add(idname);
    /* A value it gives, read by a write that runs every step (Projectile Ray's pin identifiers
     * are its own: "POINT"). */
    GenNode &split = b.Add("LogicNativeSeparateXYZ");
    b.Link(ray,
           std::strcmp(idname, "LogicNativeProjectileRay") == 0 ? "POINT" : "Picked Point",
           split,
           "Vector");
    w.Float(split, "X", "hit_x");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    bool found = false;
    for (const LN_QueryExpression &query : program->GetQueryExpressions()) {
      if (query.condition_bool_expr_index == LN_INVALID_INDEX) {
        continue;
      }
      ASSERT_LT(query.condition_bool_expr_index, program->GetBoolExpressions().size());
      const LN_BoolExpression &condition =
          program->GetBoolExpressions()[query.condition_bool_expr_index];
      EXPECT_EQ(condition.kind, LN_BoolExpressionKind::Constant);
      EXPECT_TRUE(condition.bool_value);
      found = true;
    }
    EXPECT_TRUE(found) << "no query with a condition";
  }
}

/* Slow Follow's factor is per Logic + Physics step: a route behind Every 2 Steps covers 2 steps a
 * run and follows as far as two steps would (2026-11-08: it followed half as fast). Factor 0.5
 * from x 0 to a Target at x 1: the first run (step 0) covers one step, x 0.5; the next (step 2)
 * two, x 0.875, where one run a step is after steps 0, 1 and 2 (it was 0.75). */
TEST(LN_NodeCoverage, SlowFollowBehindEveryNStepsFollowsAsFarAsItsSteps)
{
  for (const int16_t every : {int16_t(1), int16_t(2)}) {
    SCOPED_TRACE("every " + std::to_string(every));
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate", every, 1);
    GenNode &follow = b.Add("LogicNativeSlowFollow");
    b.ObjectNamed(follow, "Target", &diff::TargetBlenderObject());
    b.Float(follow, "Factor", 0.5f);
    b.Link(step, "Out", follow, "Flow");
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kProduction);
    rig.AddObject("Target", MT_Vector3(1.0f, 0.0f, 0.0f));
    /* A step, then the scene graph's update (Slow Follow sets the world position; the scene
     * updates world poses between steps, the rig does not). */
    const auto step_x = [&]() {
      rig.Step();
      rig.Object(0).NodeUpdateGS(0.0);
      return float(rig.Object(0).NodeGetWorldPosition().x());
    };
    EXPECT_FLOAT_EQ(step_x(), 0.5f);
    EXPECT_FLOAT_EQ(step_x(), every == 1 ? 0.75f : 0.5f);
    EXPECT_FLOAT_EQ(step_x(), 0.875f);
  }
}

/* Combine XYZW linked into a Vector input warns that W is dropped there (2026-11-08); a Generic
 * input (Set Game Property's Value) keeps all four, without a warning. */
TEST(LN_NodeCoverage, CombineXYZWWarnsWhereItsWIsDropped)
{
  for (const bool into_vector : {true, false}) {
    TreeBuilder b;
    Writer w(b);
    GenNode &xyzw = b.Add("LogicNativeCombineXYZW");
    GenNode *target = nullptr;
    if (into_vector) {
      target = &b.Add("LogicNativeApplyMovement");
      b.Link(xyzw, "Vector", *target, "Vector");
    }
    else {
      target = &b.Add("LogicNativeSetGameProperty");
      b.String(*target, "Property", "v4");
      b.Link(xyzw, "Vector", *target, "Value");
    }
    w.Run(*target);
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    bool warned = false;
    for (const LN_CompileIssue &issue : program->GetCompileReport().GetIssues()) {
      warned = warned || (issue.severity == LN_CompileSeverity::Warning &&
                          issue.message.find("W is dropped") != std::string::npos);
    }
    EXPECT_EQ(warned, into_vector) << report;
  }
}

/* ---- list and dictionary nodes in Game Property mode (2026-11-08) ---- */

LN_Value TestItem(const LN_ValueType type, const int number, const char *text = "")
{
  LN_Value value;
  value.type = type;
  value.exists = true;
  if (type == LN_ValueType::Int) {
    value.int_value = number;
  }
  else if (type == LN_ValueType::Float) {
    value.float_value = float(number) * 0.75f;
  }
  else {
    value.string_value = text;
  }
  return value;
}

/** Writes `value` into the game property as Logic Nodes write one. */
void PutValue(KX_GameObject &object, const char *name, const LN_Value &value)
{
  std::string reason;
  ASSERT_TRUE(LN_CommandBuffer::ApplyGamePropertyValue(&object, name, value, nullptr, reason))
      << reason;
}

/** The game property as Logic Nodes read it, every field. */
std::string StoredValue(KX_GameObject &object, const char *name)
{
  EXP_Value *property = object.GetProperty(name);
  if (property == nullptr) {
    return "<none>";
  }
  LN_DumpNames names;
  return LN_DumpValue(LN_GamePropertyValue(property, nullptr), names);
}

/** `value` as StoredValue gives it once written into a property (the same round trip). */
std::string AsStored(KX_GameObject &object, const LN_Value &value)
{
  /* (A write never changes a property's type: a list after a dictionary is refused.) */
  object.RemoveProperty("__expected");
  PutValue(object, "__expected", value);
  return StoredValue(object, "__expected");
}

struct CollectionNodeCase {
  const char *idname;
  LN_PropertyEditOp op;
  /** The collection input and output (List or Dictionary). */
  const char *collection;
  bool index, key, value, amount;
};

const CollectionNodeCase kCollectionNodes[] = {
    {"LogicNativeListAppend", LN_PropertyEditOp::ListAppend, "List", false, false, true, false},
    {"LogicNativeListRemoveIndex",
     LN_PropertyEditOp::ListRemoveIndex,
     "List",
     true,
     false,
     false,
     false},
    {"LogicNativeListRemoveValue",
     LN_PropertyEditOp::ListRemoveValue,
     "List",
     false,
     false,
     true,
     false},
    {"LogicNativeListSetIndex", LN_PropertyEditOp::ListSetIndex, "List", true, false, true, false},
    {"LogicNativeDictSetKey", LN_PropertyEditOp::DictSetKey, "Dictionary", false, true, true, false},
    {"LogicNativeDictAddValue",
     LN_PropertyEditOp::DictAddValue,
     "Dictionary",
     false,
     true,
     false,
     true},
    {"LogicNativeDictRemoveKey",
     LN_PropertyEditOp::DictRemoveKey,
     "Dictionary",
     false,
     true,
     false,
     false},
};

/* The Game Property mode (one rule: LN_ApplyPropertyEdit at the flush) changes the stored list or
 * dictionary exactly as the Output Only result (the kinds, unchanged since before 2026-11-08)
 * changes its copy: both modes of each node on the same inputs, read live from properties, over
 * 400 random lists or dictionaries (indexes in and out of range, negative ones, empty and missing
 * keys, equal and unequal values, whole and broken amounts). */
TEST(LN_NodeCoverage, ListAndDictionaryChangesEndAsTheirOutputs)
{
  for (const CollectionNodeCase &c : kCollectionNodes) {
    SCOPED_TRACE(c.idname);
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    const auto read = [&](const char *idname, const char *property) -> GenNode & {
      GenNode &node = b.Add(idname);
      b.String(node, "Property", property);
      return node;
    };
    GenNode &source = read("LogicNativeGetGameProperty", "src");
    GenNode &index = read("LogicNativeGetGamePropertyInt", "i");
    GenNode &key = read("LogicNativeGetGamePropertyString", "k");
    GenNode &value = read("LogicNativeGetGameProperty", "v");
    GenNode &amount = read("LogicNativeGetGameProperty", "a");
    const auto inputs = [&](GenNode &node) {
      if (c.index) {
        b.Link(index, "Value", node, "Index");
      }
      if (c.key) {
        b.Link(key, "Value", node, "Key");
      }
      if (c.value) {
        b.Link(value, "Value", node, "Value");
      }
      if (c.amount) {
        b.Link(amount, "Value", node, "Amount");
      }
    };
    GenNode &copy = b.Add(c.idname, LN_COLLECTION_STORE_OUTPUT_ONLY);
    b.Link(step, "Out", copy, "Flow");
    b.Link(source, "Value", copy, c.collection);
    inputs(copy);
    GenNode &keep = b.Add("LogicNativeSetGameProperty");
    b.String(keep, "Property", "out");
    b.Link(copy, c.collection, keep, "Value");
    b.Link(copy, "Done", keep, "Flow");
    GenNode &edit = b.Add(c.idname, LN_COLLECTION_STORE_GAME_PROPERTY);
    b.String(edit, "Property", "dst");
    b.Link(step, "Out", edit, "Flow");
    inputs(edit);
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    SemRig rig(program, kMemoOff);
    KX_GameObject &o = rig.Object(0);
    std::mt19937 random(uint32_t(c.op) * 7919u + 13u);
    const auto pick = [&](const int count) { return int(random() % uint32_t(count)); };
    const char *keys[] = {"a", "b", "c", "d"};
    const auto item = [&]() {
      switch (pick(3)) {
        case 0:
          return TestItem(LN_ValueType::Int, pick(5) - 2);
        case 1:
          return TestItem(LN_ValueType::Float, pick(5) - 2);
        default:
          return TestItem(LN_ValueType::String, 0, pick(2) == 0 ? "x" : "y");
      }
    };
    for (int round = 0; round < 400; round++) {
      LN_Value collection;
      collection.exists = true;
      if (LN_PropertyEditIsList(c.op)) {
        collection.type = LN_ValueType::List;
        for (int n = pick(6); n > 0; n--) {
          collection.list_value.push_back(item());
        }
      }
      else {
        collection.type = LN_ValueType::Dict;
        for (const char *k : keys) {
          if (pick(2) == 0) {
            collection.dict_value[k] = item();
          }
        }
      }
      PutValue(o, "src", collection);
      PutValue(o, "dst", collection);
      rig.SetInt(o, "i", pick(13) - 6);
      rig.SetString(o, "k", pick(5) == 0 ? "" : keys[pick(4)]);
      /* Remove Value: mostly a value the list holds. */
      PutValue(o,
               "v",
               !collection.list_value.empty() && pick(3) != 0 ?
                   collection.list_value[size_t(pick(int(collection.list_value.size())))] :
                   item());
      const float amounts[] = {0.0f, 1.0f, -2.0f, 2.5f, 1.0e6f, 0.1f};
      rig.SetFloat(o, "a", amounts[pick(6)]);
      rig.Step();
      ASSERT_EQ(StoredValue(o, "dst"), StoredValue(o, "out")) << "round " << round;
    }
  }
}

/* What Game Property mode adds: every change of a step takes effect, in order, after the step's
 * earlier writes of the property (the Output Only nodes' trap: two Appends in one step kept one
 * item); Done follows the change; a missing property starts empty; one of another type is left as
 * it is; another object's property changes too. In every interpreter mode. */
TEST(LN_NodeCoverage, ListAndDictionaryNodesChangeTheirGamePropertyInOrder)
{
  for (const SemMode &mode : AllModes()) {
    SCOPED_TRACE(mode.name);
    TreeBuilder b;
    Writer w(b);
    const auto read = [&](const char *idname, const char *property) -> GenNode & {
      GenNode &node = b.Add(idname);
      b.String(node, "Property", property);
      return node;
    };
    GenNode &one = read("LogicNativeGetGameProperty", "one");
    GenNode &two = read("LogicNativeGetGameProperty", "two");
    GenNode &empty = read("LogicNativeGetGameProperty", "empty");
    const auto change = [&](const char *idname, const char *property) -> GenNode & {
      GenNode &node = b.Add(idname, LN_COLLECTION_STORE_GAME_PROPERTY);
      b.String(node, "Property", property);
      w.Run(node);
      return node;
    };
    /* Two Appends in one step, after a Set Object Property of the same list. */
    GenNode &reset = b.Add("LogicNativeSetGameProperty");
    b.String(reset, "Property", "items");
    b.Link(empty, "Value", reset, "Value");
    w.Run(reset);
    b.Link(one, "Value", change("LogicNativeListAppend", "items"), "Value");
    b.Link(two, "Value", change("LogicNativeListAppend", "items"), "Value");
    /* A later Set Object Property wins over an earlier Append. */
    b.Link(one, "Value", change("LogicNativeListAppend", "replaced"), "Value");
    GenNode &replace = b.Add("LogicNativeSetGameProperty");
    b.String(replace, "Property", "replaced");
    b.Link(empty, "Value", replace, "Value");
    w.Run(replace);
    /* Missing properties: an Append and a Set Dictionary Key make them, a removal does not. */
    b.Link(two, "Value", change("LogicNativeListAppend", "fresh"), "Value");
    GenNode &set_key = change("LogicNativeDictSetKey", "fresh_dict");
    b.String(set_key, "Key", "k");
    b.Link(one, "Value", set_key, "Value");
    change("LogicNativeListRemoveIndex", "missing");
    /* A property of another type stays as it is. */
    b.Link(one, "Value", change("LogicNativeListAppend", "number"), "Value");
    /* Counts: two adds to one key in one step, then a removed key. */
    GenNode &add_a = change("LogicNativeDictAddValue", "counts");
    b.String(add_a, "Key", "n");
    b.Float(add_a, "Amount", 2.0f);
    GenNode &add_b = change("LogicNativeDictAddValue", "counts");
    b.String(add_b, "Key", "n");
    b.Float(add_b, "Amount", 3.0f);
    GenNode &remove_key = change("LogicNativeDictRemoveKey", "counts");
    b.String(remove_key, "Key", "gone");
    /* Another object's list. */
    GenNode &other = change("LogicNativeListAppend", "log");
    b.ObjectNamed(other, "Object", &diff::TargetBlenderObject());
    b.Link(two, "Value", other, "Value");
    /* Done follows: one count a step. */
    GenNode &count = b.Add("LogicNativeModifyProperty", blender::NODE_MATH_ADD);
    b.String(count, "Property", "done");
    b.Float(count, "Value", 1.0f);
    w.Run(count);
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    /* Three owners: the later trees have sort keys of their own (the first one's is 0, which
     * hid a change recorded at key 0 in front of the tree's other writes). */
    constexpr size_t kOwners = 3;
    SemRig rig(program, mode, kOwners);
    KX_GameObject &target = rig.AddObject("Target", MT_Vector3(1.0f, 0.0f, 0.0f));
    KX_GameObject &scratch = rig.AddObject("Scratch", MT_Vector3(0.0f, 0.0f, 0.0f));
    LN_Value empty_list;
    empty_list.type = LN_ValueType::List;
    empty_list.exists = true;
    LN_Value nine = empty_list;
    nine.list_value.push_back(TestItem(LN_ValueType::Int, 9));
    LN_Value counts;
    counts.type = LN_ValueType::Dict;
    counts.exists = true;
    counts.dict_value["n"] = TestItem(LN_ValueType::Int, 1);
    counts.dict_value["gone"] = TestItem(LN_ValueType::Int, 4);
    for (size_t owner = 0; owner < kOwners; owner++) {
      KX_GameObject &o = rig.Object(owner);
      PutValue(o, "one", TestItem(LN_ValueType::Int, 1));
      PutValue(o, "two", TestItem(LN_ValueType::String, 0, "two"));
      PutValue(o, "empty", empty_list);
      PutValue(o, "items", nine);
      PutValue(o, "replaced", nine);
      PutValue(o, "counts", counts);
      rig.SetFloat(o, "number", 3.5f);
      rig.SetFloat(o, "done", 0.0f);
    }
    rig.Step();
    LN_Value both = empty_list;
    both.list_value = {TestItem(LN_ValueType::Int, 1), TestItem(LN_ValueType::String, 0, "two")};
    LN_Value just_two = empty_list;
    just_two.list_value = {TestItem(LN_ValueType::String, 0, "two")};
    LN_Value fresh_dict;
    fresh_dict.type = LN_ValueType::Dict;
    fresh_dict.exists = true;
    fresh_dict.dict_value["k"] = TestItem(LN_ValueType::Int, 1);
    LN_Value counted = counts;
    counted.dict_value.erase("gone");
    counted.dict_value["n"] = TestItem(LN_ValueType::Int, 6);
    for (size_t owner = 0; owner < kOwners; owner++) {
      SCOPED_TRACE("owner " + std::to_string(owner));
      KX_GameObject &o = rig.Object(owner);
      EXPECT_EQ(StoredValue(o, "items"), AsStored(scratch, both))
          << "the Set first, then both Appends in order";
      EXPECT_EQ(StoredValue(o, "replaced"), AsStored(scratch, empty_list));
      EXPECT_EQ(StoredValue(o, "fresh"), AsStored(scratch, just_two));
      EXPECT_EQ(StoredValue(o, "fresh_dict"), AsStored(scratch, fresh_dict));
      EXPECT_FALSE(rig.Has(o, "missing"));
      EXPECT_EQ(rig.Float(o, "number"), 3.5f);
      EXPECT_EQ(StoredValue(o, "counts"), AsStored(scratch, counted)) << "1 + 2 + 3, an Int";
      EXPECT_EQ(rig.Float(o, "done"), 1.0f);
    }
    LN_Value log = empty_list;
    log.list_value.assign(kOwners, TestItem(LN_ValueType::String, 0, "two"));
    EXPECT_EQ(StoredValue(target, "log"), AsStored(scratch, log)) << "one item a tree";
    rig.Step();
    log.list_value.assign(2 * kOwners, TestItem(LN_ValueType::String, 0, "two"));
    EXPECT_EQ(StoredValue(target, "log"), AsStored(scratch, log)) << "one more a tree a step";
    EXPECT_EQ(rig.Float(rig.Object(kOwners - 1), "done"), 2.0f);
  }
}

/* Game Property mode needs a property name; Output Only (every node saved before 2026-11-08)
 * still compiles as a value node, Flow unlinked included. */
TEST(LN_NodeCoverage, ListAndDictionaryNodesNeedAPropertyOnlyInGamePropertyMode)
{
  for (const CollectionNodeCase &c : kCollectionNodes) {
    SCOPED_TRACE(c.idname);
    {
      TreeBuilder b;
      Writer w(b);
      w.Run(b.Add(c.idname, LN_COLLECTION_STORE_GAME_PROPERTY));
      std::string report;
      EXPECT_EQ(b.Compile(&report), nullptr);
      EXPECT_NE(report.find("needs the name of the game property"), std::string::npos) << report;
    }
    {
      TreeBuilder b;
      Writer w(b);
      GenNode &copy = b.Add(c.idname, LN_COLLECTION_STORE_OUTPUT_ONLY);
      GenNode &length = b.Add(LN_PropertyEditIsList(c.op) ? "LogicNativeListLength" :
                                                             "LogicNativeDictLength");
      b.Link(copy, c.collection, length, c.collection);
      w.Int(length, "Length", "length");
      const std::shared_ptr<LN_Program> program = CompileOrFail(b);
      ASSERT_NE(program, nullptr);
      SemRig rig(program, kMemoOff);
      rig.Step();
      EXPECT_TRUE(rig.Has(rig.Object(0), "length"));
    }
  }
}

/* Start Speaker was removed (2026-11-08): Play Sound 3D with no Sound records its play (the
 * Speaker object's own sound is found when it starts, LN_CommandBuffer::SpeakerPlayOf). */
TEST(LN_NodeCoverage, PlaySound3DWithoutASoundRecordsItsPlay)
{
  EXPECT_EQ(RecordedTypes("LogicNativePlaySound3D"), "PlaySound3D");
}

}  // namespace

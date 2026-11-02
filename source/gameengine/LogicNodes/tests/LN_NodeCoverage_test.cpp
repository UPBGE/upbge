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
  GenNode &delta = b.Add("LogicNativeDeltaFactor");
  w.Float(delta, "Factor", "delta");
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

/* Value Valid says TRUE for a value that exists. (It also says TRUE for a tree property that was
 * never set: it tests that a value was produced, not that one was set.) */
TEST(LN_NodeCoverage, PinsValueValidIsTrueEvenForATreePropertyNeverSet)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &v = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(v, "Property", "v");
  GenNode &valid = b.Add("LogicNativeValueValid");
  b.Link(v, "Value", valid, "Value");
  w.Bool(valid, "If Valid", "valid");
  GenNode &tree_property = b.Add("LogicNativeGetTreeProperty");
  b.String(tree_property, "Property", "never_set");
  GenNode &valid_unset = b.Add("LogicNativeValueValid");
  b.Link(tree_property, "Value", valid_unset, "Value");
  w.Bool(valid_unset, "If Valid", "valid_unset");
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
                             "LogicNativeDrawBox",
                             "LogicNativeDrawCube",
                             "LogicNativeDrawLine",
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
 * - Set Bone Attribute and Start Speaker need a setting (selection, loop mode) the default lacks
 *   (a design choice: the user picks one, plan phase A15 left it). */
TEST(LN_NodeCoverage, PinsNodesThatDoNotCompileAsTheEditorMakesThem)
{
  const struct {
    const char *idname;
    const char *error;
  } expected[] = {
      {"LogicNativeLoadFileContent", "unavailable in this release"},
      {"LogicNativePlayMaterialSequence", "unavailable in this release"},
      {"LogicNativeSetBoneAttribute", "Unsupported Set Bone Attribute selection"},
      {"LogicNativeStartSpeaker", "requires a speaker object and loop mode"},
  };
  for (const auto &entry : expected) {
    SCOPED_TRACE(entry.idname);
    const std::string result = RecordedTypes(entry.idname);
    EXPECT_NE(result.find("no program"), std::string::npos) << result;
    EXPECT_NE(result.find(entry.error), std::string::npos) << result;
  }
}

/* Join Path joins its two inputs. (Its second input has the same name as the first and the
 * identifier "Path_001"; the compiler matched sockets to pins by name first and failed every tree
 * with a Join Path until plan phase A15 matched the identifier first, bug B1.) */
TEST(LN_NodeCoverage, JoinPathJoinsItsTwoInputs)
{
  TreeBuilder b;
  Writer w(b);
  GenNode &join = b.Add("LogicNativeJoinPath");
  b.String(join, "Path", "folder/");
  b.String(join, "Path_001", "file.txt");
  w.String(join, "Path", "joined");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  SemRig rig(program, kMemoOff);
  rig.Step();
  EXPECT_EQ(rig.Text(rig.Object(0), "joined"), "folder/file.txt");
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

/* Get Node Group Socket Value has a compile handler but no builder for its Value output (the
 * node never gives a value): linked into any value input the tree does not compile ("requires a
 * value input", "requires a float value input"), so the node cannot be used at all today. Loud,
 * not a silent default. Pinned: a fix is a deliberate change. */
TEST(LN_NodeCoverage, PinsGetNodeGroupSocketValueGivesNoValueToALinkedInput)
{
  {
    TreeBuilder b;
    Writer w(b);
    GenNode &get = b.Add("LogicNativeGetNodeGroupSocketValue");
    b.String(get, "Node Name", "N");
    GenNode &set = b.Add("LogicNativeSetGameProperty");
    b.String(set, "Property", "from_group");
    b.Link(get, "Value", set, "Value");
    w.Run(set);
    std::string report;
    EXPECT_EQ(b.Compile(&report), nullptr);
    EXPECT_NE(report.find("requires a value input"), std::string::npos) << report;
  }
  {
    TreeBuilder b;
    Writer w(b);
    GenNode &get = b.Add("LogicNativeGetNodeGroupSocketValue");
    b.String(get, "Node Name", "N");
    w.Float(get, "Value", "from_group");
    std::string report;
    EXPECT_EQ(b.Compile(&report), nullptr);
    EXPECT_NE(report.find("requires a float value input"), std::string::npos) << report;
  }
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

}  // namespace

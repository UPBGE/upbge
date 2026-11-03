/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Get Object Attribute (plan "Attribute and Retirement", part 1). Every attribute of the node and
 * the other builders of an object attribute (Evaluate Object Property, Set Object Attribute's
 * partial write) read from the owner, a named child object, an object given by a link (Get Parent,
 * Get Owner) and a missing one, three steps long, with the objects moving between the steps. The
 * world after each step is pinned bit for bit against a golden recorded from the code before the
 * attribute ids (step G1), and is the same in every mode (the reference mode and the lean VM, the
 * memo on and off, profiled; the old interpreter too until plan step R4). */

#include "testing/testing.h"

#include <cmath>
#include <cstring>
#include <map>

#include "LN_LeanProgram.h"
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

/** A Blender object that only gives a constant object input its name (the game object's). */
blender::Object &NamedObject(const std::string &name)
{
  static std::map<std::string, std::unique_ptr<blender::Object>> objects;
  std::unique_ptr<blender::Object> &object = objects[name];
  if (!object) {
    object = std::make_unique<blender::Object>();
    SetCString(object->id.name, ("OB" + name).c_str());
  }
  return *object;
}

/** Writes values into game properties of the owner, one after the other, behind On Logic +
 * Physics Step (as LN_NodeCoverage_test's Writer). */
class Writer {
 public:
  explicit Writer(TreeBuilder &b) : m_b(b), m_event(b.Add("LogicNativeOnFixedUpdate")) {}
  void Run(GenNode &node)
  {
    m_b.Link(*m_flow, m_flow_pin, node, "Flow");
    m_flow = &node;
    m_flow_pin = "Done";
  }
  void Float(GenNode &source, const char *pin, const std::string &property)
  {
    Put("LogicNativeSetGamePropertyFloat", source, pin, property);
  }
  void Bool(GenNode &source, const char *pin, const std::string &property)
  {
    Put("LogicNativeSetGamePropertyBool", source, pin, property);
  }
  void String(GenNode &source, const char *pin, const std::string &property)
  {
    Put("LogicNativeSetGamePropertyString", source, pin, property);
  }
  /** The three components of a vector output: properties name + X/Y/Z. */
  void Vector(GenNode &source, const char *pin, const std::string &property)
  {
    Split("LogicNativeSeparateXYZ", "Vector", source, pin, property);
  }
  /** The Euler angles of a rotation output (Separate Euler): properties name + X/Y/Z. */
  void Euler(GenNode &source, const char *pin, const std::string &property)
  {
    Split("LogicNativeSeparateEuler", "Rotation", source, pin, property);
  }

 private:
  void Split(const char *kind,
             const char *input,
             GenNode &source,
             const char *pin,
             const std::string &property)
  {
    GenNode &split = m_b.Add(kind);
    m_b.Link(source, pin, split, input);
    Float(split, "X", property + "X");
    Float(split, "Y", property + "Y");
    Float(split, "Z", property + "Z");
  }
  void Put(const char *kind, GenNode &source, const char *pin, const std::string &property)
  {
    GenNode &node = m_b.Add(kind);
    m_b.String(node, "Property", property.c_str());
    m_b.Link(source, pin, node, "Value");
    Run(node);
  }
  TreeBuilder &m_b;
  GenNode &m_event;
  GenNode *m_flow = &m_event;
  const char *m_flow_pin = "Out";
};

/** Where an Object input points. */
enum class Source { Owner, Named, Parent, OwnerLink, Missing };

const char *SourceName(const Source source)
{
  switch (source) {
    case Source::Owner:
      return "owner";
    case Source::Named:
      return "named child";
    case Source::Parent:
      return "get parent";
    case Source::OwnerLink:
      return "get owner";
    case Source::Missing:
      return "missing";
  }
  return "?";
}

const Source kSources[] = {
    Source::Owner, Source::Named, Source::Parent, Source::OwnerLink, Source::Missing};

/** Points the node's Object input at `source`: empty (the owner), the constant "Target" (a child
 * of "Holder"), Get Parent of Target (Holder, an object given by a link), Get Owner, or a constant
 * object no scene has. */
void SetObject(TreeBuilder &b, GenNode &node, const char *pin, const Source source)
{
  switch (source) {
    case Source::Owner:
      break;
    case Source::Named:
      b.ObjectNamed(node, pin, &NamedObject("Target"));
      break;
    case Source::Parent: {
      GenNode &parent = b.Add("LogicNativeGetParent");
      b.ObjectNamed(parent, "Child Object", &NamedObject("Target"));
      b.Link(parent, "Parent Object", node, pin);
      break;
    }
    case Source::OwnerLink: {
      GenNode &owner = b.Add("LogicNativeGetOwner");
      b.Link(owner, "Owner Object", node, pin);
      break;
    }
    case Source::Missing:
      b.ObjectNamed(node, pin, &NamedObject("Nobody"));
      break;
  }
}

/** The Get Object Attribute options (node custom1) and what they read. */
const int16_t kVectorAttributes[] = {0, 1, 4, 5, 9, 10, 11, 12};
const int16_t kOrientationAttributes[] = {7, 8};
const int16_t kTransformAttributes[] = {13, 14};
/** The texts Evaluate Object Property (custom2 1: an attribute) is given. */
const char *const kEvaluatedNames[] = {"worldPosition",
                                       "position",
                                       "localPosition",
                                       "localScale",
                                       "worldScale",
                                       "worldOrientation",
                                       "localOrientation",
                                       "worldLinearVelocity",
                                       "localAngularVelocity",
                                       "color",
                                       "visible",
                                       "name",
                                       "hp",
                                       "nothing"};

/** Every read of an object attribute the compiler builds, from `source`, stored on the owner;
 * then Holder moves and the owner turns (applied at the flush: the next step reads the change). */
std::shared_ptr<LN_Program> AttributeProgram(const Source source)
{
  TreeBuilder b;
  Writer w(b);
  for (const int16_t attribute : kVectorAttributes) {
    GenNode &read = b.Add("LogicNativeGetObjectAttribute", attribute);
    SetObject(b, read, "Object", source);
    w.Vector(read, "Vector", "v" + std::to_string(attribute));
  }
  for (const int16_t attribute : kOrientationAttributes) {
    GenNode &read = b.Add("LogicNativeGetObjectAttribute", attribute);
    SetObject(b, read, "Object", source);
    /* As Euler angles and as a rotation (turning a sink by it). */
    w.Euler(read, "Orientation", "o" + std::to_string(attribute));
    GenNode &turn = b.Add("LogicNativeApplyRotation");
    b.ObjectNamed(turn, "Object", &NamedObject("Sink" + std::to_string(attribute)));
    b.Link(read, "Orientation", turn, "Rotation");
    b.Bool(turn, "Local", true);
    w.Run(turn);
  }
  for (const int16_t attribute : kTransformAttributes) {
    GenNode &read = b.Add("LogicNativeGetObjectAttribute", attribute);
    SetObject(b, read, "Object", source);
    w.Vector(read, "Position", "p" + std::to_string(attribute));
    w.Euler(read, "Orientation", "q" + std::to_string(attribute));
    w.Vector(read, "Scale", "s" + std::to_string(attribute));
    GenNode &turn = b.Add("LogicNativeApplyRotation");
    b.ObjectNamed(turn, "Object", &NamedObject("Sink" + std::to_string(attribute)));
    b.Link(read, "Orientation", turn, "Rotation");
    b.Bool(turn, "Local", true);
    w.Run(turn);
  }
  {
    GenNode &read = b.Add("LogicNativeGetObjectAttribute", 2);
    SetObject(b, read, "Object", source);
    w.Bool(read, "Visible", "visible");
  }
  {
    GenNode &read = b.Add("LogicNativeGetObjectAttribute", 3);
    SetObject(b, read, "Object", source);
    w.String(read, "Name", "name");
  }
  int index = 0;
  for (const char *name : kEvaluatedNames) {
    GenNode &evaluate = b.Add("LogicNativeEvaluateProperty", 0, 1);
    SetObject(b, evaluate, "Object", source);
    b.String(evaluate, "Property", name);
    const std::string property = "e" + std::to_string(index++);
    w.Bool(evaluate, "If True", property + "t");
    w.String(evaluate, "Value", property + "s");
    w.Vector(evaluate, "Value", property + "v");
  }
  /* Set Object Attribute with part of XYZ: it reads the attribute it keeps. */
  {
    GenNode &set = b.Add("LogicNativeSetObjectAttribute", 0); /* World Position */
    SetObject(b, set, "Object", source);
    b.Vector(set, "XYZ", 1.0f, 0.0f, 1.0f);
    b.Vector(set, "Value", 5.0f, 6.0f, 7.0f);
    w.Run(set);
  }
  {
    GenNode &set = b.Add("LogicNativeSetObjectAttribute", 13); /* Local Scale */
    SetObject(b, set, "Object", source);
    b.Vector(set, "XYZ", 0.0f, 1.0f, 0.0f);
    b.Vector(set, "Scale", 2.0f, 2.5f, 3.0f);
    w.Run(set);
  }
  /* The scene moves: the next step reads the change. */
  {
    GenNode &move = b.Add("LogicNativeApplyMovement");
    b.ObjectNamed(move, "Object", &NamedObject("Holder"));
    b.Vector(move, "Vector", 0.25f, -0.5f, 0.125f);
    w.Run(move);
    GenNode &turn = b.Add("LogicNativeApplyRotation");
    b.Rotation(turn, "Rotation", 0.05f, 0.1f, -0.15f);
    b.Bool(turn, "Local", true);
    w.Run(turn);
  }
  return CompileOrFail(b);
}

MT_Matrix3x3 Euler(const float x, const float y, const float z)
{
  return LN_Rotation::FromEulerXYZ(MT_Vector3(x, y, z)).ToMatrix();
}

/** The world the attribute programs read: the owner Obj0, Holder with its child Target (so world
 * and local differ), and the sinks the rotation reads turn. */
void BuildWorld(SemRig &rig)
{
  KX_GameObject &owner = rig.Object(0);
  owner.NodeSetLocalPosition(MT_Vector3(1.5f, -2.25f, 0.75f));
  owner.NodeSetLocalOrientation(Euler(0.3f, -0.7f, 1.1f));
  owner.NodeSetLocalScale(MT_Vector3(1.25f, 0.5f, 2.0f));
  owner.SetObjectColor(MT_Vector4(0.1f, 0.2f, 0.3f, 0.4f));
  owner.NodeUpdateGS(0.0);
  rig.SetFloat(owner, "hp", 7.5f);

  KX_GameObject &holder = rig.AddObject("Holder", MT_Vector3(10.0f, 20.0f, 30.0f));
  KX_GameObject &target = rig.AddObject("Target", MT_Vector3(0.0f, 0.0f, 0.0f));
  for (const int sink : {7, 8, 13, 14}) {
    rig.AddObject("Sink" + std::to_string(sink), MT_Vector3(0.0f, 0.0f, 0.0f));
  }
  holder.NodeSetLocalOrientation(Euler(0.0f, 0.0f, 0.5f));
  holder.NodeSetLocalScale(MT_Vector3(2.0f, 2.0f, 2.0f));
  rig.Parent(2, 1);
  target.NodeSetLocalPosition(MT_Vector3(0.5f, 1.0f, -1.5f));
  target.NodeSetLocalOrientation(Euler(0.2f, 0.4f, -0.6f));
  target.NodeSetLocalScale(MT_Vector3(1.0f, 3.0f, 0.25f));
  target.SetObjectColor(MT_Vector4(0.9f, 0.8f, 0.7f, 0.6f));
  target.SetVisible(false, false);
  holder.NodeUpdateGS(0.0);
  rig.SetInt(target, "hp", 3);
}

std::vector<KX_GameObject *> RigObjects(SemRig &rig)
{
  std::vector<KX_GameObject *> objects;
  for (size_t index = 0; index < rig.ObjectCount(); index++) {
    objects.push_back(&rig.Object(index));
  }
  return objects;
}

constexpr int kSteps = 3;

/** The world after each of three steps of the attribute program of `source`. */
std::string RunAttributes(const std::shared_ptr<LN_Program> &program, const SemMode &mode)
{
  SemRig rig(program, mode);
  BuildWorld(rig);
  std::string out;
  for (int step = 0; step < kSteps; step++) {
    rig.Step();
    out += "step " + std::to_string(step) + "\n" + DumpWorld(RigObjects(rig));
  }
  return out;
}

struct LeanScope {
  explicit LeanScope(const int mode)
  {
    LN_SetLeanModeForTests(mode);
  }
  ~LeanScope()
  {
    LN_SetLeanModeForTests(-1);
  }
};

struct FastScope {
  explicit FastScope(const int mode)
  {
    LN_RuntimeTree::SetFastAttributeModeForTests(mode);
  }
  ~FastScope()
  {
    LN_RuntimeTree::SetFastAttributeModeForTests(-1);
  }
};

/** UPBGE_LN_FAST_ATTRIBUTES: off (the text and the whole value at every read), on, verify. */
const int kFastModes[] = {0, 1, 2};
const char *FastName(const int mode)
{
  return mode == 0 ? "fast off" : (mode == 1 ? "fast on" : "fast verify");
}

/** The lean modes: the reference mode (0) and the lean VM (1 and 2, the old Auto and Force: since
 * plan step R3 both are the lean VM). */
const int kLeanModes[] = {0, 1, 2};
const char *LeanName(const int mode)
{
  return mode == 0 ? "reference" : (mode == 1 ? "lean (1)" : "lean (2)");
}

}  // namespace

/* G1: every attribute, from every kind of object input, reads exactly as the golden recorded before
 * the attribute ids (G2) and the typed reads (G3), in every interpreter, memo and fast attribute
 * mode; verify finds no difference. */
TEST(LN_ObjectAttribute, EveryAttributeReadsAsBeforeInEveryMode)
{
  const uint32_t differences_before = LN_RuntimeTree::FastAttributeVerifyDifferencesForTests();
  std::string golden;
  for (const Source source : kSources) {
    SCOPED_TRACE(SourceName(source));
    const std::shared_ptr<LN_Program> program = AttributeProgram(source);
    ASSERT_NE(program, nullptr);
    std::string reference;
    {
      LeanScope lean(0);
      FastScope fast(0);
      reference = RunAttributes(program, kProduction);
    }
    for (const int fast_mode : kFastModes) {
      FastScope fast(fast_mode);
      for (const int lean_mode : kLeanModes) {
        LeanScope lean(lean_mode);
        for (const SemMode &mode : AllModes()) {
          const std::string text = RunAttributes(program, mode);
          EXPECT_EQ(text, reference) << FastName(fast_mode) << ", " << LeanName(lean_mode) << ", "
                                     << mode.name << ": " << diff::FirstDifference(reference, text);
        }
      }
    }
    golden += std::string("source ") + SourceName(source) + "\n" + reference;
  }
  const std::string verdict = CheckGolden("object_attributes_v0", golden);
  EXPECT_EQ(verdict, "") << verdict;
  EXPECT_EQ(LN_RuntimeTree::FastAttributeVerifyDifferencesForTests(), differences_before);
}

/* G3: a pure expression over Target's position (Vector Math Add) is read, Target moves, a Get
 * Distance applies the move, and the same expression read again gives the new position: no memo
 * keeps anything above an attribute (it may change within the tick). */
TEST(LN_ObjectAttribute, APureExpressionOverAnAttributeIsReadAgainAfterAMoveInTheTick)
{
  TreeBuilder b;
  GenNode &event = b.Add("LogicNativeOnFixedUpdate");
  GenNode &sequence = b.Add("LogicNativeSequence", 0, 0, 4);
  b.Link(event, "Out", sequence, "Flow");
  GenNode &attribute = b.Add("LogicNativeGetObjectAttribute", 0); /* World Position */
  b.ObjectNamed(attribute, "Object", &NamedObject("Target"));
  GenNode &add = b.Add("LogicNativeVectorMath", blender::NODE_VECTOR_MATH_ADD);
  b.Link(attribute, "Vector", add, "A");
  b.Vector(add, "B", 0.5f, 0.0f, 0.0f);
  GenNode &split = b.Add("LogicNativeSeparateXYZ");
  b.Link(add, "Result", split, "Vector");
  GenNode &before = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(before, "Property", "x0");
  b.Link(split, "X", before, "Value");
  b.Link(sequence, "Then 0", before, "Flow");
  GenNode &move = b.Add("LogicNativeApplyMovement");
  b.ObjectNamed(move, "Object", &NamedObject("Target"));
  b.Vector(move, "Vector", 1.0f, 0.0f, 0.0f);
  b.Link(sequence, "Then 1", move, "Flow");
  GenNode &distance = b.Add("LogicNativeGetDistance", 0, 0);
  b.ObjectNamed(distance, "Object B", &NamedObject("Target"));
  GenNode &store = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(store, "Property", "d");
  b.Link(distance, "Distance", store, "Value");
  b.Link(sequence, "Then 2", store, "Flow");
  GenNode &after = b.Add("LogicNativeSetGamePropertyFloat");
  b.String(after, "Property", "x1");
  b.Link(split, "X", after, "Value");
  b.Link(sequence, "Then 3", after, "Flow");
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  for (const int fast_mode : kFastModes) {
    FastScope fast(fast_mode);
    for (const int lean_mode : kLeanModes) {
      LeanScope lean(lean_mode);
      for (const SemMode &mode : AllModes()) {
        SCOPED_TRACE(std::string(FastName(fast_mode)) + ", " + LeanName(lean_mode) + ", " +
                     mode.name);
        SemRig rig(program, mode);
        rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 0.0f));
        rig.Step();
        EXPECT_EQ(rig.Float(rig.Object(0), "x0"), 3.5f);
        EXPECT_EQ(rig.Float(rig.Object(0), "d"), std::sqrt(32.0f));
        EXPECT_EQ(rig.Float(rig.Object(0), "x1"), 4.5f);
      }
    }
  }
}

/* G2: every attribute the node offers is read by number, none by its text at every read, and only
 * the names of attributes have a number (a game property's name has none). */
TEST(LN_ObjectAttribute, EveryAttributeOfTheNodeHasAnIdAndOnlyAttributeNamesHaveOne)
{
  struct Option {
    int16_t attribute;
    const char *output;
  };
  const Option options[] = {{0, "Vector"},   {1, "Vector"},       {3, "Name"},
                            {4, "Vector"},   {5, "Vector"},       {7, "Orientation"},
                            {8, "Orientation"}, {9, "Vector"},    {10, "Vector"},
                            {11, "Vector"},  {12, "Vector"},      {13, "Position"},
                            {13, "Orientation"}, {13, "Scale"},   {14, "Position"},
                            {14, "Orientation"}, {14, "Scale"}};
  for (const Option &option : options) {
    SCOPED_TRACE(std::to_string(option.attribute) + " " + option.output);
    TreeBuilder b;
    Writer w(b);
    GenNode &read = b.Add("LogicNativeGetObjectAttribute", option.attribute);
    b.ObjectNamed(read, "Object", &NamedObject("Target"));
    if (std::strcmp(option.output, "Name") == 0) {
      w.String(read, option.output, "got");
    }
    else if (std::strcmp(option.output, "Orientation") == 0) {
      w.Euler(read, option.output, "got");
    }
    else {
      w.Vector(read, option.output, "got");
    }
    const std::shared_ptr<LN_Program> program = CompileOrFail(b);
    ASSERT_NE(program, nullptr);
    const std::vector<LN_ValueExpression> &values = program->GetValueExpressions();
    const std::vector<uint8_t> &ids = program->GetRegisterExpressionProgram().value_object_attribute;
    ASSERT_EQ(ids.size(), values.size());
    int attributes = 0;
    for (size_t index = 0; index < values.size(); index++) {
      if (values[index].kind == LN_ValueExpressionKind::ObjectAttribute) {
        attributes++;
        EXPECT_NE(ids[index], uint8_t(LN_ObjectAttributeId::None));
      }
      else {
        EXPECT_EQ(ids[index], uint8_t(LN_ObjectAttributeId::None));
      }
    }
    EXPECT_GE(attributes, 1);
  }
  for (const char *name : kEvaluatedNames) {
    const bool attribute = std::strcmp(name, "hp") != 0 && std::strcmp(name, "nothing") != 0;
    EXPECT_EQ(LN_ObjectAttributeIdFromName(name) != LN_ObjectAttributeId::None, attribute) << name;
  }
  EXPECT_EQ(LN_ObjectAttributeIdFromName("position"), LN_ObjectAttributeId::WorldPosition);
  EXPECT_EQ(LN_ObjectAttributeIdFromName(""), LN_ObjectAttributeId::None);
  EXPECT_EQ(LN_ObjectAttributeIdFromName("Name"), LN_ObjectAttributeId::None);
}

/* G1 bench: 10,000 owners, each reading its own world and local position, scale, visibility and
 * name, and Target's world position (a named object), through Separate XYZ into 13 properties. The
 * record cost an owner in the reference mode and on the lean VM. */
TEST(LN_ObjectAttribute, DISABLED_ReadBench)
{
  TreeBuilder b;
  Writer w(b);
  for (const int16_t attribute : {0, 1, 4}) {
    GenNode &read = b.Add("LogicNativeGetObjectAttribute", attribute);
    w.Vector(read, "Vector", "v" + std::to_string(attribute));
  }
  {
    GenNode &read = b.Add("LogicNativeGetObjectAttribute", 2);
    w.Bool(read, "Visible", "visible");
    GenNode &name = b.Add("LogicNativeGetObjectAttribute", 3);
    w.String(name, "Name", "name");
    GenNode &target = b.Add("LogicNativeGetObjectAttribute", 0);
    b.ObjectNamed(target, "Object", &NamedObject("Target"));
    w.Vector(target, "Vector", "t");
  }
  const std::shared_ptr<LN_Program> program = CompileOrFail(b);
  ASSERT_NE(program, nullptr);
  const uint32_t owners = 10000;
  const int rounds = std::getenv("UPBGE_LN_BENCH_ROUNDS") ?
                         std::atoi(std::getenv("UPBGE_LN_BENCH_ROUNDS")) :
                         40;
  for (const int lean_mode : kLeanModes) {
    LeanScope lean(lean_mode);
    SemRig rig(program, kProduction, owners);
    rig.AddObject("Target", MT_Vector3(3.0f, 4.0f, 5.0f));
    rig.SetDump(false);
    rig.SetCapturePrints(false);
    for (int warm = 0; warm < 5; warm++) {
      rig.Step();
    }
    rig.ResetTimes();
    for (int round = 0; round < rounds; round++) {
      rig.Step();
    }
    const SemRig::PhaseTimes &times = rig.Times();
    std::printf("%-10s record %.1f ns an owner, flush %.1f, snapshot %.1f\n",
                LeanName(lean_mode),
                times.record_ms * 1.0e6 / (double(rounds) * owners),
                times.flush_ms * 1.0e6 / (double(rounds) * owners),
                times.capture_ms * 1.0e6 / (double(rounds) * owners));
  }
}

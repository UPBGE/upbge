/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Implicit conversions (2026-11-16): a link from an output of another type converts by itself,
 * as Geometry Nodes converts. Every allowed pair against a model of its rule and, where Typecast
 * Value converts the pair, against the same tree with Typecast Value in between; in every mode of
 * the rig and with a crowd of owners (wide runs in the configurations that make them). The pairs
 * that stay refused, and flow links, still stop the tree. The editor's rule (link_types_convert)
 * says the same as the compiler's.
 */

#include "testing/testing.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "BLI_utildefines.h"

#include "LN_SemRig.h"
#include "NOD_logic.hh"

using namespace ln_test;
using namespace ln_test::sem;

namespace {

constexpr int16_t kToInteger = 0;
constexpr int16_t kToBoolean = 1;
constexpr int16_t kToFloat = 3;
constexpr int16_t kToVector = 4;
constexpr int16_t kAttributeWorldPosition = 0;
constexpr int16_t kAttributeWorldRotation = 1;
constexpr int16_t kAttributeColor = 11;

enum class Kind { Bool, Int, Float, Vector, Rotation, Color };
enum class Target { Bool, Int, Float, Vector, Rotation, Color, Condition };

const char *KindName(const Kind kind)
{
  switch (kind) {
    case Kind::Bool:
      return "bool";
    case Kind::Int:
      return "int";
    case Kind::Float:
      return "float";
    case Kind::Vector:
      return "vector";
    case Kind::Rotation:
      return "rotation";
    case Kind::Color:
      return "color";
  }
  return "?";
}

const char *TargetName(const Target target)
{
  switch (target) {
    case Target::Bool:
      return "bool";
    case Target::Int:
      return "int";
    case Target::Float:
      return "float";
    case Target::Vector:
      return "vector";
    case Target::Rotation:
      return "rotation";
    case Target::Color:
      return "color";
    case Target::Condition:
      return "condition";
  }
  return "?";
}

/** One owner's game properties: the sources read them while playing (never constants). */
struct Owner {
  bool b;
  int i;
  float f;
  float x, y, z;
};
const Owner kOwnerValues[] = {
    {true, -3, -2.7f, 0.0f, 0.0f, 0.0f},
    {false, 0, -0.5f, 1.0f, -2.0f, 0.5f},
    {true, 1, 0.0f, 0.25f, 0.0f, 0.0f},
    {false, 7, 0.3f, -0.75f, 0.125f, 3.0f},
    {true, -1, 3.9f, 0.0f, 0.0f, -1.5f},
    {false, 2, 1.0f, 0.5f, 0.5f, 0.5f},
    {true, 0, -0.0f, 0.0f, -0.25f, 0.0f},
    {false, -12, 12.75f, 2.0f, 4.0f, 8.0f},
};
/** Enough owners of one program for wide runs. */
constexpr uint32_t kOwners = 32;

const Owner &OwnerValues(const uint32_t index)
{
  return kOwnerValues[index % (sizeof(kOwnerValues) / sizeof(kOwnerValues[0]))];
}

GenNode &ReadFloat(TreeBuilder &b, const char *property)
{
  GenNode &read = b.Add("LogicNativeGetGamePropertyFloat");
  b.String(read, "Property", property);
  return read;
}

/** A source of `kind` reading the owner's properties; its output pin in `r_pin`. */
GenNode &Source(TreeBuilder &b, const Kind kind, const char *&r_pin)
{
  switch (kind) {
    case Kind::Bool: {
      GenNode &read = b.Add("LogicNativeGetGamePropertyBool");
      b.String(read, "Property", "pb");
      r_pin = "Value";
      return read;
    }
    case Kind::Int: {
      GenNode &read = b.Add("LogicNativeGetGamePropertyInt");
      b.String(read, "Property", "pi");
      r_pin = "Value";
      return read;
    }
    case Kind::Float:
      r_pin = "Value";
      return ReadFloat(b, "pf");
    case Kind::Vector:
    case Kind::Rotation: {
      GenNode &combine = b.Add(kind == Kind::Vector ? "LogicNativeCombineXYZ" :
                                                      "LogicNativeEuler");
      b.Link(ReadFloat(b, "px"), "Value", combine, "X");
      b.Link(ReadFloat(b, "py"), "Value", combine, "Y");
      b.Link(ReadFloat(b, "pz"), "Value", combine, "Z");
      r_pin = kind == Kind::Vector ? "Vector" : "Rotation";
      return combine;
    }
    case Kind::Color: {
      GenNode &color = b.Add("LogicNativeColorRGBA");
      r_pin = "Color";
      return color;
    }
  }
  r_pin = "";
  return b.Add("LogicNativeOnInit");
}

/** On Logic + Physics Step: `source` into a node with an input of `target`, writing what it gets
 * into the owner's properties out0.. (or its color). With `typecast`, Typecast Value set to
 * `typecast_mode` between them (the reference). */
TreeBuilder BuildTree(const Kind kind,
                      const Target target,
                      const bool typecast,
                      const int16_t typecast_mode)
{
  TreeBuilder b;
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  const char *pin = nullptr;
  GenNode *value = &Source(b, kind, pin);
  if (typecast) {
    GenNode &cast = b.Add("LogicNativeTypecast", typecast_mode);
    b.Link(*value, pin, cast, "Value");
    value = &cast;
    pin = typecast_mode == kToInteger ? "Integer" :
          typecast_mode == kToBoolean ? "Boolean" :
          typecast_mode == kToFloat   ? "Float" :
                                        "Vector";
  }
  GenNode *flow = &step;
  const char *flow_pin = "Out";
  const auto write_float = [&](GenNode &from, const char *from_pin, const char *property) {
    GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(write, "Property", property);
    b.Link(from, from_pin, write, "Value");
    b.Link(*flow, flow_pin, write, "Flow");
    flow = &write;
    flow_pin = "Done";
  };
  switch (target) {
    case Target::Bool:
    case Target::Int:
    case Target::Float: {
      GenNode &write = b.Add(target == Target::Bool ? "LogicNativeSetGamePropertyBool" :
                             target == Target::Int  ? "LogicNativeSetGamePropertyInt" :
                                                      "LogicNativeSetGamePropertyFloat");
      b.String(write, "Property", "out0");
      b.Link(*value, pin, write, "Value");
      b.Link(step, "Out", write, "Flow");
      break;
    }
    case Target::Vector:
    case Target::Rotation: {
      GenNode &separate = b.Add(target == Target::Vector ? "LogicNativeSeparateXYZ" :
                                                           "LogicNativeSeparateEuler");
      b.Link(*value, pin, separate, target == Target::Vector ? "Vector" : "Rotation");
      write_float(separate, "X", "out0");
      write_float(separate, "Y", "out1");
      write_float(separate, "Z", "out2");
      break;
    }
    case Target::Color: {
      GenNode &write = b.Add("LogicNativeSetObjectAttribute", kAttributeColor);
      b.Link(*value, pin, write, "Color");
      b.Link(step, "Out", write, "Flow");
      break;
    }
    case Target::Condition: {
      GenNode &branch = b.Add("LogicNativeBranch");
      b.Link(*value, pin, branch, "Condition");
      b.Link(step, "Out", branch, "Flow");
      GenNode &yes = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(yes, "Property", "out0");
      b.Float(yes, "Value", 1.0f);
      b.Link(branch, "True", yes, "Flow");
      GenNode &no = b.Add("LogicNativeSetGamePropertyFloat");
      b.String(no, "Property", "out0");
      b.Float(no, "Value", 0.0f);
      b.Link(branch, "False", no, "Flow");
      break;
    }
  }
  return b;
}

/** What the target gets from an owner's source, by the rule of the pair: true = 1, a number other
 * than 0 is true, a decimal number's fraction cut off, a number on all three axes, a vector not
 * of zero length is true, a rotation as its Euler angles, a vector as a color with alpha 1. */
std::vector<float> Model(const Kind kind, const Target target, const Owner &owner)
{
  float number = 0.0f;
  float vector[3] = {0.0f, 0.0f, 0.0f};
  bool truth = false;
  switch (kind) {
    case Kind::Bool:
      number = owner.b ? 1.0f : 0.0f;
      truth = owner.b;
      break;
    case Kind::Int:
      number = float(owner.i);
      truth = owner.i != 0;
      break;
    case Kind::Float:
      number = owner.f;
      truth = owner.f != 0.0f;
      break;
    case Kind::Vector:
    case Kind::Rotation:
    case Kind::Color:
      vector[0] = owner.x;
      vector[1] = owner.y;
      vector[2] = owner.z;
      truth = owner.x * owner.x + owner.y * owner.y + owner.z * owner.z > 0.0f;
      break;
  }
  const bool is_number = ELEM(kind, Kind::Bool, Kind::Int, Kind::Float);
  switch (target) {
    case Target::Bool:
    case Target::Condition:
      return {truth ? 1.0f : 0.0f};
    case Target::Int:
      return {kind == Kind::Float ? float(int(std::trunc(owner.f))) : number};
    case Target::Float:
      return {number};
    case Target::Vector:
    case Target::Rotation:
      if (is_number) {
        return {number, number, number};
      }
      return {vector[0], vector[1], vector[2]};
    case Target::Color:
      return {vector[0], vector[1], vector[2], 1.0f};
  }
  return {};
}

/** What one step of the program gives each owner (out0.., or the color). */
std::vector<std::vector<float>> RunStep(const std::shared_ptr<LN_Program> &program,
                                    const Target target,
                                    const SemMode &mode)
{
  SemRig rig(program, mode, kOwners);
  for (uint32_t index = 0; index < kOwners; index++) {
    KX_GameObject &owner = rig.Object(index);
    const Owner &values = OwnerValues(index);
    rig.SetBool(owner, "pb", values.b);
    rig.SetInt(owner, "pi", values.i);
    rig.SetFloat(owner, "pf", values.f);
    rig.SetFloat(owner, "px", values.x);
    rig.SetFloat(owner, "py", values.y);
    rig.SetFloat(owner, "pz", values.z);
    for (const char *out : {"out0", "out1", "out2"}) {
      rig.SetFloat(owner, out, -999.0f);
    }
  }
  rig.Step();
  std::vector<std::vector<float>> results;
  for (uint32_t index = 0; index < kOwners; index++) {
    KX_GameObject &owner = rig.Object(index);
    if (target == Target::Color) {
      const MT_Vector4 &color = owner.GetObjectColor();
      results.push_back({color[0], color[1], color[2], color[3]});
      continue;
    }
    const int count = ELEM(target, Target::Vector, Target::Rotation) ? 3 : 1;
    std::vector<float> values;
    for (int axis = 0; axis < count; axis++) {
      values.push_back(rig.Float(owner, ("out" + std::to_string(axis)).c_str()));
    }
    results.push_back(values);
  }
  return results;
}

std::string Floats(const std::vector<float> &values)
{
  std::string text;
  for (const float value : values) {
    text += std::to_string(value) + " ";
  }
  return text;
}

struct Pair {
  Kind kind;
  Target target;
  /** Typecast Value's mode for the same conversion, or -1 when it has none (rotation, color). */
  int16_t typecast;
};

const Pair kAllowedPairs[] = {
    {Kind::Bool, Target::Int, kToInteger},
    {Kind::Bool, Target::Float, kToFloat},
    {Kind::Bool, Target::Vector, -1},
    {Kind::Int, Target::Bool, kToBoolean},
    {Kind::Int, Target::Float, kToFloat},
    {Kind::Int, Target::Vector, kToVector},
    {Kind::Int, Target::Condition, kToBoolean},
    {Kind::Float, Target::Bool, kToBoolean},
    {Kind::Float, Target::Int, kToInteger},
    {Kind::Float, Target::Vector, kToVector},
    {Kind::Float, Target::Condition, kToBoolean},
    {Kind::Vector, Target::Bool, kToBoolean},
    {Kind::Vector, Target::Condition, kToBoolean},
    {Kind::Vector, Target::Rotation, -1},
    {Kind::Vector, Target::Color, -1},
    {Kind::Rotation, Target::Vector, kToVector},
};

}  // namespace

/* Every allowed pair: the model of its rule, the same as Typecast Value in between, bit for bit,
 * for every owner of a crowd in every mode of the rig. */
TEST(LN_ImplicitConversion, EveryAllowedLinkConvertsAsTypecastValueWould)
{
  for (const Pair &pair : kAllowedPairs) {
    SCOPED_TRACE(std::string(KindName(pair.kind)) + " -> " + TargetName(pair.target));
    TreeBuilder implicit = BuildTree(pair.kind, pair.target, false, 0);
    std::string report;
    const std::shared_ptr<LN_Program> program = implicit.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::shared_ptr<LN_Program> reference;
    if (pair.typecast >= 0) {
      TreeBuilder cast = BuildTree(pair.kind, pair.target, true, pair.typecast);
      reference = cast.Compile(&report);
      ASSERT_NE(reference, nullptr) << report;
    }
    for (const SemMode &mode : AllModes()) {
      SCOPED_TRACE(mode.name);
      const std::vector<std::vector<float>> got = RunStep(program, pair.target, mode);
      std::vector<std::vector<float>> expected;
      if (reference != nullptr) {
        expected = RunStep(reference, pair.target, mode);
      }
      for (uint32_t index = 0; index < kOwners; index++) {
        const std::vector<float> model = Model(pair.kind, pair.target, OwnerValues(index));
        EXPECT_EQ(Floats(got[index]), Floats(model)) << "owner " << index << " (the model)";
        if (reference != nullptr) {
          ASSERT_EQ(got[index].size(), expected[index].size());
          for (size_t axis = 0; axis < got[index].size(); axis++) {
            /* Bit for bit: the same conversion as Typecast Value's. */
            EXPECT_EQ(std::memcmp(&got[index][axis], &expected[index][axis], sizeof(float)), 0)
                << "owner " << index << " axis " << axis << ": " << got[index][axis]
                << " vs Typecast Value's " << expected[index][axis];
          }
        }
      }
    }
  }
}

/* A vector into a rotation input of a command, and a rotation into a vector input: the object
 * ends as with the matching node in between (Euler / Combine XYZ), bit for bit. */
TEST(LN_ImplicitConversion, VectorsAndRotationsDriveCommandsAsTheirNodes)
{
  for (const bool into_rotation : {true, false}) {
    SCOPED_TRACE(into_rotation ? "vector into World Rotation" : "rotation into World Position");
    MT_Matrix3x3 orientation[2];
    MT_Vector3 position[2];
    for (const bool implicit : {true, false}) {
      TreeBuilder b;
      GenNode &step = b.Add("LogicNativeOnFixedUpdate");
      /* The implicit tree's source is of the other type; the reference's of the input's. */
      const bool source_is_vector = into_rotation == implicit;
      GenNode &combine = b.Add(source_is_vector ? "LogicNativeCombineXYZ" : "LogicNativeEuler");
      b.Link(ReadFloat(b, "px"), "Value", combine, "X");
      b.Link(ReadFloat(b, "py"), "Value", combine, "Y");
      b.Link(ReadFloat(b, "pz"), "Value", combine, "Z");
      GenNode &write = b.Add("LogicNativeSetObjectAttribute",
                             into_rotation ? kAttributeWorldRotation : kAttributeWorldPosition);
      b.Link(combine, source_is_vector ? "Vector" : "Rotation", write,
             into_rotation ? "Rotation" : "Value");
      b.Link(step, "Out", write, "Flow");
      std::string report;
      const std::shared_ptr<LN_Program> program = b.Compile(&report);
      ASSERT_NE(program, nullptr) << report;
      SemRig rig(program, kProduction);
      KX_GameObject &owner = rig.Object(0);
      rig.SetFloat(owner, "px", 0.3f);
      rig.SetFloat(owner, "py", -1.1f);
      rig.SetFloat(owner, "pz", 2.25f);
      rig.Step();
      owner.NodeUpdateGS(0.0);
      orientation[implicit ? 0 : 1] = owner.NodeGetWorldOrientation();
      position[implicit ? 0 : 1] = owner.NodeGetWorldPosition();
    }
    for (int row = 0; row < 3; row++) {
      for (int column = 0; column < 3; column++) {
        EXPECT_EQ(orientation[0][row][column], orientation[1][row][column]);
      }
      EXPECT_EQ(position[0][row], position[1][row]);
    }
    if (into_rotation) {
      /* Really turned (not left as it was). */
      EXPECT_NE(orientation[0][0][0], 1.0f);
    }
    else {
      EXPECT_FLOAT_EQ(position[0][1], -1.1f);
    }
  }
}

/* The pairs that stay refused, and flow links, still stop the tree with their message. */
TEST(LN_ImplicitConversion, RefusedLinksStillStopTheTree)
{
  struct Refused {
    Kind kind;
    Target target;
  };
  const Refused refused[] = {
      {Kind::Vector, Target::Float},
      {Kind::Vector, Target::Int},
      {Kind::Rotation, Target::Bool},
      {Kind::Rotation, Target::Float},
      {Kind::Rotation, Target::Color},
      {Kind::Color, Target::Float},
      {Kind::Color, Target::Vector},
      {Kind::Color, Target::Bool},
  };
  for (const Refused &pair : refused) {
    SCOPED_TRACE(std::string(KindName(pair.kind)) + " -> " + TargetName(pair.target));
    TreeBuilder b = BuildTree(pair.kind, pair.target, false, 0);
    std::string report;
    EXPECT_EQ(b.Compile(&report), nullptr);
    EXPECT_NE(report.find("Data link type mismatch"), std::string::npos) << report;
  }
  /* A flow output into a number input, and a number into a Flow input. */
  {
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(write, "Property", "out0");
    b.Link(step, "Out", write, "Flow");
    b.Link(step, "Out", write, "Value");
    std::string report;
    EXPECT_EQ(b.Compile(&report), nullptr);
    EXPECT_NE(report.find("Execution output cannot connect to data input"), std::string::npos)
        << report;
  }
  {
    /* A number into Flow leaves the node unreached by any event: as every link into an inactive
     * node, a warning (the node never runs), not an error. */
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &first = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(first, "Property", "out1");
    b.Float(first, "Value", 5.0f);
    b.Link(step, "Out", first, "Flow");
    GenNode &write = b.Add("LogicNativeSetGamePropertyFloat");
    b.String(write, "Property", "out0");
    b.Link(ReadFloat(b, "pf"), "Value", write, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    EXPECT_NE(report.find("requires an execution output"), std::string::npos) << report;
    SemRig rig(program, kProduction);
    KX_GameObject &owner = rig.Object(0);
    rig.SetFloat(owner, "pf", 3.0f);
    rig.SetFloat(owner, "out0", -999.0f);
    rig.Step();
    EXPECT_EQ(rig.Float(owner, "out1"), 5.0f);
    EXPECT_EQ(rig.Float(owner, "out0"), -999.0f);
  }
}

/* The editor's link rule (link_types_convert) allows exactly the pairs the compiler converts: a
 * pair of node sockets is drawn without red exactly when the tree compiles with it. */
TEST(LN_ImplicitConversion, TheEditorsRuleIsTheCompilers)
{
  const auto socket_type = [](const Kind kind) {
    switch (kind) {
      case Kind::Bool:
        return blender::SOCK_BOOLEAN;
      case Kind::Int:
        return blender::SOCK_INT;
      case Kind::Float:
        return blender::SOCK_FLOAT;
      case Kind::Vector:
        return blender::SOCK_VECTOR;
      case Kind::Rotation:
        return blender::SOCK_ROTATION;
      case Kind::Color:
        return blender::SOCK_RGBA;
    }
    return blender::SOCK_CUSTOM;
  };
  const Kind kinds[] = {
      Kind::Bool, Kind::Int, Kind::Float, Kind::Vector, Kind::Rotation, Kind::Color};
  const std::pair<Target, Kind> targets[] = {{Target::Bool, Kind::Bool},
                                             {Target::Int, Kind::Int},
                                             {Target::Float, Kind::Float},
                                             {Target::Vector, Kind::Vector},
                                             {Target::Rotation, Kind::Rotation},
                                             {Target::Color, Kind::Color}};
  for (const Kind kind : kinds) {
    for (const auto &[target, target_kind] : targets) {
      if (kind == target_kind) {
        continue;
      }
      SCOPED_TRACE(std::string(KindName(kind)) + " -> " + TargetName(target));
      TreeBuilder b = BuildTree(kind, target, false, 0);
      std::string report;
      const bool compiles = b.Compile(&report) != nullptr;
      EXPECT_EQ(compiles,
                blender::nodes::logic::link_types_convert(socket_type(kind),
                                                          socket_type(target_kind)))
          << report;
    }
  }
}

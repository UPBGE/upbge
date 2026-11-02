/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * The random trees and the driver of the differential tester (lean runtime plan, phase A3).
 *
 * TreeGen builds a random node tree from a curated family table and the node descriptors
 * (NOD_logic_descriptors.hh): events (On Update, On Logic + Physics Step with Every N and
 * spread), flow nodes (branches, property writes, moves, latent nodes ...) and data DAGs
 * (math, vectors, strings, Random, distances, object attributes ...) whose leaves read game
 * properties that cannot be folded. Links are only made between compatible pins, so almost every
 * tree compiles through the real compiler: the compiler's own operand conventions stay the truth.
 *
 * RunWorld replays the manager's per-tick order on a small world of positioned objects: the run
 * decisions, step gates, snapshots, query warm-ups, recording (serial or through the tree batch
 * recorder), EndRecording, Flush. Its text is what two runs are compared by (and what the golden
 * hashes cover). Header only, in namespace ln_test::diff. */

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "DNA_node_types.h"
#include "nodes/NOD_logic_descriptors.hh"

#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"

#include "LN_JobPool.h"
#include "LN_KindCatalog.h"
#include "LN_Manager.h"
#include "LN_TestTrees.h"
#include "LN_TreeBatch.h"

namespace ln_test::diff {

using blender::nodes::logic::NodeMetadata;
using blender::nodes::logic::PinMetadata;
using blender::nodes::logic::PinType;

/* ---- random numbers ---- */

class Rng {
 public:
  explicit Rng(const uint64_t seed) : m_state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull)
  {
    next();
    next();
  }
  uint64_t next()
  {
    m_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = m_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t below(const uint32_t n)
  {
    return n == 0 ? 0 : uint32_t(next() % n);
  }
  bool chance(const double p)
  {
    return double(next() >> 11) * (1.0 / 9007199254740992.0) < p;
  }
  float real(const float lo, const float hi)
  {
    return lo + float(double(next() >> 11) * (1.0 / 9007199254740992.0)) * (hi - lo);
  }
  template<typename T> const T &pick(const std::vector<T> &items)
  {
    return items[below(uint32_t(items.size()))];
  }

 private:
  uint64_t m_state;
};

/** Values that reach the kinds through leaves that cannot be folded (game properties per object),
 * and the constants of unlinked inputs. Edge cases are common on purpose. */
inline float EdgeFloat(Rng &rng)
{
  static const float edges[] = {0.0f,
                                -0.0f,
                                1.0f,
                                -1.0f,
                                0.5f,
                                2.0f,
                                -3.75f,
                                100.0f,
                                1.0e30f,
                                -1.0e30f,
                                1.0e-40f,
                                -1.0e-40f,
                                3.14159265f,
                                7.0f,
                                255.0f,
                                0.1f,
                                std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN()};
  return rng.chance(0.55) ? edges[rng.below(sizeof(edges) / sizeof(edges[0]))] :
                            rng.real(-20.0f, 20.0f);
}
/** Small and moderate on purpose: a Length or a Count must not ask for gigabytes. */
inline int32_t EdgeInt(Rng &rng)
{
  static const int32_t edges[] = {0, 1, -1, 2, 7, 100, -100, 255, 1000, -1000};
  return rng.chance(0.6) ? edges[rng.below(sizeof(edges) / sizeof(edges[0]))] :
                           int32_t(rng.below(41)) - 20;
}
inline std::string EdgeString(Rng &rng)
{
  static const char *const edges[] = {
      "", "a", "Hello", "hello world", "42", "-3.5", "ABC abc", "aaaa", "x", "{}", "Obj0", "  "};
  if (rng.chance(0.05)) {
    return std::string(40, char('a' + rng.below(26)));
  }
  return edges[rng.below(sizeof(edges) / sizeof(edges[0]))];
}

/* ---- pins ---- */

enum class Cat : uint8_t {
  None,
  Exec,
  Float,
  Int,
  Bool,
  String,
  Vector,
  Rotation,
  Color,
  Generic,
  Object
};

inline Cat CategoryOf(const PinType type)
{
  switch (type) {
    case PinType::Execution:
      return Cat::Exec;
    case PinType::Float:
    case PinType::Angle:
      return Cat::Float;
    case PinType::Int:
      return Cat::Int;
    case PinType::Bool:
    case PinType::Condition:
      return Cat::Bool;
    case PinType::String:
      return Cat::String;
    case PinType::Vector:
    case PinType::VectorXYAngle:
      return Cat::Vector;
    case PinType::Rotation:
      return Cat::Rotation;
    case PinType::Color:
      return Cat::Color;
    case PinType::Generic:
      return Cat::Generic;
    case PinType::Object:
      return Cat::Object;
    default:
      return Cat::None;
  }
}

/** A link from a source of category `from` into an input of category `to` (the compiler's
 * LogicPinTypesCompatible, narrowed to what the generator makes: same type, a number into text,
 * anything into a generic input). */
inline bool CanLink(const Cat from, const Cat to)
{
  if (from == Cat::None || to == Cat::None || from == Cat::Generic) {
    return false;
  }
  if (from == Cat::Exec || to == Cat::Exec) {
    return from == to;
  }
  if (to == Cat::Generic || from == to) {
    return true;
  }
  return to == Cat::String && (from == Cat::Int || from == Cat::Float);
}

/* ---- the family table ---- */

enum class Role : uint8_t { Event, Flow, Data };

/** Whether `output` (an identifier) of the node set to (custom1, custom2) gives a value the
 * compiler can build. Null: every output. */
using OutputFilter = bool (*)(int16_t custom1, int16_t custom2, const std::string &output);

struct Family {
  const char *idname;
  Role role;
  int weight;
  /** The node's custom1 / custom2 choices (empty: 0). */
  std::vector<int16_t> custom1 = {};
  std::vector<int16_t> custom2 = {};
  /** Only these outputs (identifiers) serve as data sources; empty: every data output. */
  std::vector<const char *> data_outputs = {};
  OutputFilter output_ok = nullptr;
  /** The types the node's generic pins take (one is chosen for the node); empty: any scalar or
   * vector. */
  std::vector<Cat> generic = {};
  /** Needs the object seam (a constant object by name). */
  bool needs_scene = false;
};

inline bool StringOperationOutputOk(const int16_t custom1,
                                    const int16_t /*custom2*/,
                                    const std::string &output)
{
  const bool boolean = custom1 == 2 || custom1 == 5 || custom1 == 6;
  if (output == "Result") {
    return boolean;
  }
  if (output == "Count") {
    return custom1 == 3;
  }
  return !boolean && custom1 != 3; /* "String" */
}

/** Typecast builds only the output its target (custom1) names. */
inline bool TypecastOutputOk(const int16_t custom1,
                             const int16_t /*custom2*/,
                             const std::string &output)
{
  return (output == "Integer" && custom1 == 0) || (output == "Boolean" && custom1 == 1) ||
         (output == "String" && custom1 == 2) || (output == "Float" && custom1 == 3);
}

inline const std::vector<Family> &FamilyTable()
{
  static const std::vector<Family> table = [] {
    std::vector<Family> t;
    std::vector<int16_t> math;
    for (const int16_t op :
         {blender::NODE_MATH_ADD,      blender::NODE_MATH_SUBTRACT, blender::NODE_MATH_MULTIPLY,
          blender::NODE_MATH_DIVIDE,   blender::NODE_MATH_POWER,    blender::NODE_MATH_MINIMUM,
          blender::NODE_MATH_MAXIMUM,  blender::NODE_MATH_ABSOLUTE, blender::NODE_MATH_SIGN,
          blender::NODE_MATH_ROUND,    blender::NODE_MATH_FLOOR,    blender::NODE_MATH_CEIL,
          blender::NODE_MATH_TRUNC,    blender::NODE_MATH_FRACTION, blender::NODE_MATH_MODULO,
          blender::NODE_MATH_SINE,     blender::NODE_MATH_COSINE,   blender::NODE_MATH_RADIANS,
          blender::NODE_MATH_DEGREES})
    {
      math.push_back(op);
    }
    std::vector<int16_t> vmath;
    for (const int16_t op : {blender::NODE_VECTOR_MATH_ADD,
                             blender::NODE_VECTOR_MATH_SUBTRACT,
                             blender::NODE_VECTOR_MATH_MULTIPLY,
                             blender::NODE_VECTOR_MATH_DIVIDE,
                             blender::NODE_VECTOR_MATH_ABSOLUTE,
                             blender::NODE_VECTOR_MATH_MINIMUM,
                             blender::NODE_VECTOR_MATH_MAXIMUM,
                             blender::NODE_VECTOR_MATH_SCALE,
                             blender::NODE_VECTOR_MATH_NORMALIZE})
    {
      vmath.push_back(op);
    }
    /* Events. */
    t.push_back({"LogicNativeOnUpdate", Role::Event, 5});
    t.push_back({"LogicNativeOnFixedUpdate", Role::Event, 6, {0, 1, 2, 3, 4, 5}, {0, 1}, {"Time Passed"}});
    /* Flow. */
    t.push_back({"LogicNativeBranch", Role::Flow, 6});
    t.push_back({"LogicNativeSetGamePropertyFloat", Role::Flow, 9});
    t.push_back({"LogicNativeSetGamePropertyInt", Role::Flow, 4});
    t.push_back({"LogicNativeSetGamePropertyBool", Role::Flow, 4});
    t.push_back({"LogicNativeSetGamePropertyString", Role::Flow, 4});
    t.push_back({"LogicNativeApplyMovement", Role::Flow, 5});
    t.push_back({"LogicNativeApplyRotation", Role::Flow, 3});
    t.push_back({"LogicNativePrint", Role::Flow, 2});
    t.push_back({"LogicNativeDelay", Role::Flow, 2});
    t.push_back({"LogicNativeTimer", Role::Flow, 2});
    t.push_back({"LogicNativePulsify", Role::Flow, 3});
    t.push_back({"LogicNativeCooldown", Role::Flow, 3, {}, {}, {"Remaining", "Progress", "Is Ready"}});
    t.push_back({"LogicNativeOnce", Role::Flow, 2});
    t.push_back({"LogicNativeFlipFlop", Role::Flow, 2, {}, {}, {"Is A"}});
    t.push_back({"LogicNativeFlowRouter", Role::Flow, 2});
    t.push_back({"LogicNativeRepeatLimit", Role::Flow, 2, {}, {}, {"Remaining", "Is Exhausted"}});
    t.push_back({"LogicNativeStoreValue", Role::Flow, 3, {}, {}, {"Value"}});
    t.push_back({"LogicNativeSetTreeProperty", Role::Flow, 4, {}, {}, {}, nullptr, {Cat::Float, Cat::Int, Cat::Bool, Cat::String}});
    t.push_back({"LogicNativeOnNextFrame", Role::Flow, 1});
    t.push_back({"LogicNativeLoop", Role::Flow, 1});
    /* Data. */
    t.push_back({"LogicNativeMath", Role::Data, 24, math});
    t.push_back({"LogicNativeCompare", Role::Data, 5, {0, 1, 2, 3, 4, 5}});
    t.push_back({"LogicNativeVectorMath", Role::Data, 6, vmath});
    t.push_back({"LogicNativeCombineXYZ", Role::Data, 3});
    t.push_back({"LogicNativeSeparateXYZ", Role::Data, 3});
    t.push_back({"LogicNativeClampValue", Role::Data, 2});
    t.push_back({"LogicNativeLimitRange", Role::Data, 2});
    t.push_back({"LogicNativeInvertValue", Role::Data, 1});
    t.push_back({"LogicNativeMapRange", Role::Data, 2});
    t.push_back({"LogicNativeThreshold", Role::Data, 1, {0, 1}});
    t.push_back({"LogicNativeWithinRange", Role::Data, 1, {0, 1}});
    t.push_back({"LogicNativeRangedThreshold", Role::Data, 1, {0, 1}});
    t.push_back({"LogicNativeGate", Role::Data, 3, {0, 1, 2, 3, 4, 5, 6}});
    t.push_back({"LogicNativeStringOperation", Role::Data, 5, {0, 2, 3, 4, 5, 6, 7, 8, 9}, {}, {}, StringOperationOutputOk});
    t.push_back({"LogicNativeFormattedString", Role::Data, 2});
    t.push_back({"LogicNativeRandomFloat", Role::Data, 2});
    t.push_back({"LogicNativeRandomInt", Role::Data, 1});
    t.push_back({"LogicNativeRandomVector", Role::Data, 1});
    t.push_back({"LogicNativeTypecast", Role::Data, 5, {0, 1, 2, 3}, {0, 1, 2, 3}, {}, TypecastOutputOk, {Cat::Float, Cat::Int, Cat::Bool}});
    t.push_back({"LogicNativeMixValue", Role::Data, 1, {}, {}, {}, nullptr, {Cat::Float, Cat::Vector}});
    t.push_back({"LogicNativeValueSwitch", Role::Data, 1});
    t.push_back({"LogicNativeGetTreeProperty", Role::Data, 4, {}, {}, {}, nullptr, {Cat::Float, Cat::Int, Cat::Bool, Cat::String}});
    /* Added in phase A5 (nodes that had no test and need no scene). */
    t.push_back({"LogicNativeVectorRotate", Role::Data, 2});
    t.push_back({"LogicNativeVectorToRotation", Role::Data, 1});
    t.push_back({"LogicNativeValueSwitchList", Role::Data, 1, {}, {}, {}, nullptr, {Cat::Float, Cat::Int, Cat::String}});
    t.push_back({"LogicNativeValueSwitchListCompare", Role::Data, 1, {}, {}, {}, nullptr, {Cat::Float, Cat::Int, Cat::String}});
    t.push_back({"LogicNativeToggleTreeProperty", Role::Flow, 2});
    t.push_back({"LogicNativeGetAxisVector", Role::Data, 3, {0, 1, 2, 3, 4, 5}});
    t.push_back({"LogicNativeGetObjectAttribute", Role::Data, 3, {0, 1, 4, 5, 9, 10, 11, 12}, {}, {"Vector"}});
    t.push_back({"LogicNativeGetDistance", Role::Data, 3, {0, 1}, {0, 1}, {"Distance", "Squared Distance", "Valid"}, nullptr, {}, true});
    return t;
  }();
  return table;
}

inline const Family *FindFamily(const std::string &idname)
{
  for (const Family &family : FamilyTable()) {
    if (idname == family.idname) {
      return &family;
    }
  }
  return nullptr;
}

/* ---- one generated node ---- */

struct GenNode {
  blender::bNode node = {};
  std::deque<blender::bNodeSocket> sockets;
  std::deque<blender::bNodeSocketValueFloat> floats;
  std::deque<blender::bNodeSocketValueInt> ints;
  std::deque<blender::bNodeSocketValueBoolean> bools;
  std::deque<blender::bNodeSocketValueString> strings;
  std::deque<blender::bNodeSocketValueVector> vectors;
  std::deque<blender::bNodeSocketValueRotation> rotations;
  std::deque<blender::bNodeSocketValueRGBA> colors;
  std::deque<blender::bNodeSocketValueObject> objects;
  std::deque<std::string> names;
  const NodeMetadata *meta = nullptr;
  const Family *family = nullptr;
  /** The descriptors' pins, one per socket (variadic pins repeated with numbered names). */
  std::vector<PinMetadata> in_pins;
  std::vector<PinMetadata> out_pins;
  std::vector<blender::bNodeSocket *> inputs;
  std::vector<blender::bNodeSocket *> outputs;
  std::vector<bool> input_linked;
  /** What the node's generic pins carry. */
  Cat generic = Cat::Float;
  std::string label; /* idname + options, for reports */

  Cat resolve(const Cat category) const
  {
    return category == Cat::Generic ? generic : category;
  }

  void add_socket(const bool input, const PinMetadata &pin)
  {
    (input ? in_pins : out_pins).push_back(pin);
    blender::bNodeSocket &socket = sockets.emplace_back();
    void *default_value = nullptr;
    blender::eNodeSocketDatatype datatype = blender::SOCK_CUSTOM;
    /* Only inputs carry a default; an output's value is whatever the node computes. */
    switch (CategoryOf(pin.type)) {
      case Cat::Exec:
        datatype = blender::SOCK_BOOLEAN;
        break;
      case Cat::Float:
        datatype = blender::SOCK_FLOAT;
        default_value = input ? &floats.emplace_back() : nullptr;
        break;
      case Cat::Int:
        datatype = blender::SOCK_INT;
        default_value = input ? &ints.emplace_back() : nullptr;
        break;
      case Cat::Bool:
        datatype = blender::SOCK_BOOLEAN;
        default_value = input ? &bools.emplace_back() : nullptr;
        break;
      case Cat::String:
        datatype = blender::SOCK_STRING;
        default_value = input ? &strings.emplace_back() : nullptr;
        break;
      case Cat::Vector:
        datatype = blender::SOCK_VECTOR;
        if (input) {
          vectors.emplace_back().dimensions = 3;
          default_value = &vectors.back();
        }
        break;
      case Cat::Rotation:
        datatype = blender::SOCK_ROTATION;
        default_value = input ? &rotations.emplace_back() : nullptr;
        break;
      case Cat::Color:
        datatype = blender::SOCK_RGBA;
        default_value = input ? &colors.emplace_back() : nullptr;
        break;
      case Cat::Object:
        datatype = blender::SOCK_OBJECT;
        default_value = input ? &objects.emplace_back() : nullptr;
        break;
      default:
        datatype = blender::SOCK_CUSTOM;
        break;
    }
    InitSocketWithIdentifier(socket,
                             pin.identifier,
                             pin.ui_name,
                             datatype,
                             default_value,
                             blender::nodes::logic::pin_type_socket_idname(pin.type));
    Append(input ? node.inputs : node.outputs, socket);
    (input ? inputs : outputs).push_back(&socket);
    if (input) {
      input_linked.push_back(false);
    }
  }

  Cat input_category(const size_t index) const
  {
    return CategoryOf(in_pins[index].type);
  }
  /** The category of what output `index` carries (a generic output: the node's type). */
  Cat output_category(const size_t index) const
  {
    return resolve(CategoryOf(out_pins[index].type));
  }
  bool output_allowed(const size_t index) const
  {
    const std::string identifier = out_pins[index].identifier;
    if (family != nullptr && !family->data_outputs.empty()) {
      bool listed = false;
      for (const char *name : family->data_outputs) {
        listed = listed || identifier == name;
      }
      if (!listed) {
        return false;
      }
    }
    return family == nullptr || family->output_ok == nullptr ||
           family->output_ok(node.custom1, node.custom2, identifier);
  }
};

/** The Blender object a constant object input points at; the world's "Target" has its name. */
inline blender::Object &TargetBlenderObject()
{
  static blender::Object object;
  static const bool named = (SetCString(object.id.name, "OBTarget"), true);
  (void)named;
  return object;
}

/* ---- the world's property names ---- */

inline const std::vector<std::string> &FloatNames()
{
  static const std::vector<std::string> names = {"pf0", "pf1", "pf2", "pf3", "wf0", "wf1"};
  return names;
}
inline const std::vector<std::string> &IntNames()
{
  static const std::vector<std::string> names = {"pi0", "pi1", "wi0"};
  return names;
}
inline const std::vector<std::string> &BoolNames()
{
  static const std::vector<std::string> names = {"pb0", "pb1", "wb0"};
  return names;
}
inline const std::vector<std::string> &StringNames()
{
  static const std::vector<std::string> names = {"ps0", "ps1", "ws0"};
  return names;
}

/** The values the world's owner has for each property when every owner has the same properties
 * (RunOptions::identical_objects): what a folded twin of a tree holds as constants. */
struct FoldValues {
  std::map<std::string, float> floats;
  std::map<std::string, int32_t> ints;
  std::map<std::string, bool> bools;
  std::map<std::string, std::string> strings;
};

inline FoldValues FoldValuesFor(const uint64_t world_seed)
{
  Rng rng(world_seed ^ 0x77ull);
  FoldValues values;
  for (const std::string &name : FloatNames()) {
    values.floats[name] = EdgeFloat(rng);
  }
  for (const std::string &name : IntNames()) {
    values.ints[name] = EdgeInt(rng);
  }
  for (const std::string &name : BoolNames()) {
    values.bools[name] = rng.chance(0.5);
  }
  for (const std::string &name : StringNames()) {
    values.strings[name] = EdgeString(rng);
  }
  return values;
}

/* ---- the generator ---- */

struct Source {
  GenNode *node = nullptr;
  size_t output = 0;
  Cat category = Cat::None;
};

class GeneratedTree {
 public:
  blender::bNodeTree tree = {};
  std::deque<std::unique_ptr<GenNode>> nodes;
  std::deque<blender::bNodeLink> links;
  bool needs_scene = false;
  int events = 0;
  int32_t next_identifier = 1;
  GeneratedTree()
  {
    InitTree(tree);
  }
  GeneratedTree(const GeneratedTree &) = delete;

  /** The nodes and links as text, for a report. */
  std::string Describe() const
  {
    std::string out;
    for (const auto &node : nodes) {
      out += "  node " + std::to_string(node->node.identifier) + " " + node->label + "\n";
    }
    for (const blender::bNodeLink &link : links) {
      out += "  link " + std::to_string(link.fromnode->identifier) + "." +
             std::string(link.fromsock->identifier) + " -> " +
             std::to_string(link.tonode->identifier) + "." + std::string(link.tosock->identifier) +
             "\n";
    }
    return out;
  }
};

class TreeGen {
 public:
  /** `budget`: about how many flow nodes in all; `depth`: how deep a data DAG goes. */
  TreeGen(const uint64_t seed, const int budget = 10, const int depth = 3)
      : m_rng(seed), m_budget(budget), m_max_depth(depth)
  {
  }

  /** Families (idnames) the tree does not use: the ablation of a difference (does it stay when
   * Get Axis Vector is left out?). */
  void Exclude(const std::set<std::string> &idnames)
  {
    m_excluded = idnames;
  }

  /** The tree's property reads become constants with the values of `values` (the folded twin of
   * a tree: the compiler folds what has only constant inputs). Null: live reads. */
  void SetFold(const FoldValues *values)
  {
    m_fold = values;
  }

  std::unique_ptr<GeneratedTree> Build()
  {
    m_result = std::make_unique<GeneratedTree>();
    const int roots = 1 + int(m_rng.below(3));
    for (int index = 0; index < roots; index++) {
      GenNode *event = AddFamilyNode(PickFamily(Role::Event));
      m_result->events++;
      const size_t exec = FirstExecOutput(*event);
      if (event->family->data_outputs.size() == 1) {
        PoolAdd(*event, 1); /* On Logic + Physics Step's Time Passed */
      }
      if (m_rng.chance(0.4)) {
        BuildOrderPattern({event, exec, Cat::Exec});
      }
      else {
        BuildFlow({event, exec, Cat::Exec}, std::max(2, m_budget / roots), 0);
      }
    }
    return std::move(m_result);
  }

  /** A tree of one data node (set to `custom1` / `custom2`, its inputs constants or property
   * reads) whose `output` is written to a property or printed. */
  std::unique_ptr<GeneratedTree> ProbeData(const Family &family,
                                           const int16_t custom1,
                                           const int16_t custom2,
                                           const size_t output,
                                           const Cat generic = Cat::None)
  {
    m_result = std::make_unique<GeneratedTree>();
    m_live_inputs = true;
    GenNode *event = AddFamilyNode(*FindFamily("LogicNativeOnUpdate"));
    GenNode *node = AddFamilyNode(family);
    SetCustom(*node, custom1, custom2);
    if (generic != Cat::None) {
      node->generic = generic;
    }
    FillInputs(*node, m_max_depth);
    const Cat category = node->output_category(output);
    const char *sink_name = "LogicNativePrint";
    size_t sink_input = 1;
    switch (category) {
      case Cat::Float:
        sink_name = "LogicNativeSetGamePropertyFloat";
        sink_input = 3;
        break;
      case Cat::Int:
        sink_name = "LogicNativeSetGamePropertyInt";
        sink_input = 3;
        break;
      case Cat::Bool:
        sink_name = "LogicNativeSetGamePropertyBool";
        sink_input = 3;
        break;
      case Cat::String:
        sink_name = "LogicNativeSetGamePropertyString";
        sink_input = 3;
        break;
      default:
        break;
    }
    GenNode *sink = AddFamilyNode(*FindFamily(sink_name));
    Link(*event, FirstExecOutput(*event), *sink, 0);
    Link(*node, output, *sink, sink_input);
    FillInputs(*sink, m_max_depth);
    /* Written to a property nothing reads, so a later frame reads the same inputs. */
    if (std::string(sink_name) != "LogicNativePrint") {
      SetCString(static_cast<blender::bNodeSocketValueString *>(
                     sink->inputs[In(*sink, "Property")]->default_value)->value,
                 "po");
    }
    return std::move(m_result);
  }

  /** A tree of one flow node (set to `custom1` / `custom2`) behind an On Update. */
  std::unique_ptr<GeneratedTree> ProbeFlow(const Family &family,
                                           const int16_t custom1,
                                           const int16_t custom2)
  {
    m_result = std::make_unique<GeneratedTree>();
    m_live_inputs = true;
    if (family.role == Role::Event) {
      GenNode *node = AddFamilyNode(family);
      SetCustom(*node, custom1, custom2);
      return std::move(m_result);
    }
    GenNode *event = AddFamilyNode(*FindFamily("LogicNativeOnUpdate"));
    GenNode *node = AddFamilyNode(family);
    SetCustom(*node, custom1, custom2);
    size_t exec_input = 0;
    for (size_t index = 0; index < node->inputs.size(); index++) {
      if (node->input_category(index) == Cat::Exec) {
        exec_input = index;
        break;
      }
    }
    Link(*event, FirstExecOutput(*event), *node, exec_input);
    FillInputs(*node, m_max_depth);
    return std::move(m_result);
  }

  /** The last data node ProbeData / a probe made (the one under test). */
  size_t OutputCount(const Family &family)
  {
    m_result = std::make_unique<GeneratedTree>();
    return AddFamilyNode(family)->outputs.size();
  }

 private:
  Rng m_rng;
  int m_budget;
  int m_max_depth;
  std::unique_ptr<GeneratedTree> m_result;
  std::map<Cat, std::vector<Source>> m_pool;
  /** Probes read every input from a live source: constants would be folded by the compiler and
   * the runtime kinds behind them would never run. */
  bool m_live_inputs = false;
  const FoldValues *m_fold = nullptr;
  std::set<std::string> m_excluded;

  void SetCustom(GenNode &node, const int16_t custom1, const int16_t custom2)
  {
    node.node.custom1 = custom1;
    node.node.custom2 = custom2;
    node.label = std::string(node.meta->idname + 11) + " c1=" + std::to_string(custom1) +
                 " c2=" + std::to_string(custom2);
  }

  const Family &PickFamily(const Role role)
  {
    int total = 0;
    for (const Family &family : FamilyTable()) {
      if (family.role == role && m_excluded.count(family.idname) == 0) {
        total += family.weight;
      }
    }
    int roll = int(m_rng.below(uint32_t(total)));
    for (const Family &family : FamilyTable()) {
      if (family.role != role || m_excluded.count(family.idname) != 0) {
        continue;
      }
      if (roll < family.weight) {
        return family;
      }
      roll -= family.weight;
    }
    return FamilyTable().front();
  }

  /** A data family with an output of the given category (or null). */
  const Family *PickDataFamily(const Cat wanted)
  {
    std::vector<const Family *> fitting;
    int total = 0;
    for (const Family &family : FamilyTable()) {
      if (family.role != Role::Data || m_excluded.count(family.idname) != 0) {
        continue;
      }
      const NodeMetadata *meta = blender::nodes::logic::find_logic_node_metadata(family.idname);
      for (size_t out = 0; meta != nullptr && out < meta->outputs.size(); out++) {
        const Cat category = CategoryOf(meta->outputs[out].type);
        /* A generic output carries the node's chosen type: any family type may come. */
        const bool generic_fits = category == Cat::Generic &&
                                  (family.generic.empty() ||
                                   std::find(family.generic.begin(),
                                             family.generic.end(),
                                             wanted) != family.generic.end() ||
                                   wanted == Cat::Generic);
        const bool listed = family.data_outputs.empty() ||
                            std::any_of(family.data_outputs.begin(),
                                        family.data_outputs.end(),
                                        [&](const char *name) {
                                          return std::string(name) == meta->outputs[out].identifier;
                                        });
        if (listed && (generic_fits || CanLink(category, wanted))) {
          fitting.push_back(&family);
          total += family.weight;
          break;
        }
      }
    }
    if (total == 0) {
      return nullptr;
    }
    int roll = int(m_rng.below(uint32_t(total)));
    for (const Family *family : fitting) {
      if (roll < family->weight) {
        return family;
      }
      roll -= family->weight;
    }
    return fitting.front();
  }

  GenNode *AddFamilyNode(const Family &family)
  {
    const NodeMetadata *meta = blender::nodes::logic::find_logic_node_metadata(family.idname);
    assert(meta != nullptr);
    auto owned = std::make_unique<GenNode>();
    GenNode &gen = *owned;
    gen.meta = meta;
    gen.family = &family;
    const int32_t identifier = m_result->next_identifier++;
    InitNode(gen.node,
             family.idname,
             (std::string(meta->ui_name) + "." + std::to_string(identifier)).c_str(),
             identifier);
    if (!family.generic.empty()) {
      gen.generic = m_rng.pick(family.generic);
    }
    else {
      static const std::vector<Cat> any = {
          Cat::Float, Cat::Int, Cat::Bool, Cat::String, Cat::Vector};
      gen.generic = m_rng.pick(any);
    }
    const auto add_pins = [&](const bool input) {
      for (const PinMetadata &pin : input ? meta->inputs : meta->outputs) {
        if (pin.is_variadic) {
          const int count = 2 + int(m_rng.below(3)); /* a Flow Router needs 2 to 64 */
          for (int number = 0; number < count; number++) {
            PinMetadata copy = pin;
            gen.names.push_back(std::string(pin.identifier) + std::to_string(number));
            copy.identifier = gen.names.back().c_str();
            copy.ui_name = copy.identifier;
            copy.is_variadic = false;
            gen.add_socket(input, copy);
          }
        }
        else {
          gen.add_socket(input, pin);
        }
      }
    };
    add_pins(true);
    add_pins(false);
    Append(m_result->tree.nodes, gen.node);
    int16_t custom1 = 0, custom2 = 0;
    if (!family.custom1.empty()) {
      custom1 = m_rng.pick(family.custom1);
    }
    if (!family.custom2.empty()) {
      custom2 = m_rng.pick(family.custom2);
    }
    m_result->nodes.push_back(std::move(owned));
    GenNode *node = m_result->nodes.back().get();
    SetCustom(*node, custom1, custom2);
    if (family.output_ok != nullptr && family.role == Role::Data && !family.custom1.empty()) {
      /* A setting that has no output to read is not made (re-rolled a few times). */
      for (int attempt = 0; attempt < 8; attempt++) {
        bool any_output = false;
        for (size_t out = 0; out < node->outputs.size(); out++) {
          any_output = any_output || node->output_allowed(out);
        }
        if (any_output) {
          break;
        }
        SetCustom(*node, m_rng.pick(family.custom1), custom2);
      }
    }
    if (family.needs_scene) {
      m_result->needs_scene = true;
    }
    return node;
  }

  static size_t FirstExecOutput(const GenNode &node)
  {
    for (size_t index = 0; index < node.outputs.size(); index++) {
      if (node.output_category(index) == Cat::Exec) {
        return index;
      }
    }
    return 0;
  }

  void Link(GenNode &from, const size_t from_output, GenNode &to, const size_t to_input)
  {
    InitLink(m_result->links.emplace_back(),
             from.node,
             *from.outputs[from_output],
             to.node,
             *to.inputs[to_input]);
    Append(m_result->tree.links, m_result->links.back());
    to.input_linked[to_input] = true;
  }

  void PoolAdd(GenNode &node, const size_t output)
  {
    m_pool[node.output_category(output)].push_back({&node, output, node.output_category(output)});
  }

  /** The flow chain behind `from` (an execution output). */
  void BuildFlow(const Source from, const int budget, const int depth)
  {
    Source cursor = from;
    int left = budget;
    while (left > 0 && depth < 4) {
      const Family &family = PickFamily(Role::Flow);
      GenNode *node = AddFamilyNode(family);
      left--;
      /* The first execution input takes the flow. */
      size_t exec_input = SIZE_MAX;
      for (size_t index = 0; index < node->inputs.size(); index++) {
        if (node->input_category(index) == Cat::Exec) {
          exec_input = index;
          break;
        }
      }
      if (exec_input == SIZE_MAX) {
        continue;
      }
      Link(*cursor.node, cursor.output, *node, exec_input);
      FillInputs(*node, 0);
      /* Data outputs of stateful flow nodes can feed later nodes. */
      for (size_t out = 0; out < node->outputs.size(); out++) {
        if (node->output_category(out) != Cat::Exec && !family.data_outputs.empty() &&
            node->output_allowed(out))
        {
          PoolAdd(*node, out);
        }
      }
      /* Continue behind an execution output; a branch also gets a short second chain. */
      std::vector<size_t> exec_outputs;
      for (size_t out = 0; out < node->outputs.size(); out++) {
        if (node->output_category(out) == Cat::Exec) {
          exec_outputs.push_back(out);
        }
      }
      if (exec_outputs.empty()) {
        return;
      }
      const size_t chosen = exec_outputs[m_rng.below(uint32_t(exec_outputs.size()))];
      for (const size_t out : exec_outputs) {
        if (out != chosen && m_rng.chance(0.5) && left > 1) {
          const int share = 1 + int(m_rng.below(2));
          BuildFlow({node, out, Cat::Exec}, share, depth + 1);
          left -= share;
        }
      }
      cursor = {node, chosen, Cat::Exec};
    }
  }

  static size_t PinIndex(const std::vector<PinMetadata> &pins, const char *identifier)
  {
    for (size_t index = 0; index < pins.size(); index++) {
      if (std::string(pins[index].identifier) == identifier) {
        return index;
      }
    }
    assert(false && "no such pin");
    return 0;
  }
  static size_t In(const GenNode &node, const char *identifier)
  {
    return PinIndex(node.in_pins, identifier);
  }
  static size_t Out(const GenNode &node, const char *identifier)
  {
    return PinIndex(node.out_pins, identifier);
  }

  /** A flow node of the table behind `cursor`; the cursor moves behind its first exec output. */
  GenNode *Chain(Source &cursor, const char *idname)
  {
    GenNode *node = AddFamilyNode(*FindFamily(idname));
    size_t exec_input = 0;
    for (size_t index = 0; index < node->inputs.size(); index++) {
      if (node->input_category(index) == Cat::Exec) {
        exec_input = index;
        break;
      }
    }
    Link(*cursor.node, cursor.output, *node, exec_input);
    cursor = {node, FirstExecOutput(*node), Cat::Exec};
    return node;
  }

  /** Trees whose answer depends on the order things happen in (the plan's order-revealing
   * patterns): a Get Distance read before and after a move of its owner, a Branch whose two
   * routes read the same compare, a state node read by both operands of a Math, a distance next
   * to a live attribute. A runtime that reads in another order than the serial one ends elsewhere. */
  void BuildOrderPattern(const Source from)
  {
    Source cursor = from;
    GenNode *distance = AddFamilyNode(*FindFamily("LogicNativeGetDistance"));
    SetCustom(*distance, 0, int16_t(m_rng.below(2))); /* the owner to the Target or a position */
    FillInputs(*distance, 0);
    const size_t distance_out = Out(*distance, "Distance");
    PoolAdd(*distance, distance_out);

    if (m_rng.chance(0.5)) { /* a move before the first read: a pending move that the read applies */
      GenNode *move = Chain(cursor, "LogicNativeApplyMovement");
      FillInputs(*move, 0);
    }
    const int kind = int(m_rng.below(3));
    if (kind == 0) {
      /* The compare is read by two Branches with a move of the owner between them. */
      GenNode *compare = AddFamilyNode(*FindFamily("LogicNativeCompare"));
      SetCustom(*compare, int16_t(m_rng.below(6)), 0);
      Link(*distance, distance_out, *compare, In(*compare, "A"));
      static_cast<blender::bNodeSocketValueFloat *>(
          compare->inputs[In(*compare, "B")]->default_value)->value = m_rng.real(0.5f, 8.0f);
      GenNode *first = Chain(cursor, "LogicNativeBranch");
      Link(*compare, Out(*compare, "Result"), *first, In(*first, "Condition"));
      Source near = {first, Out(*first, "True"), Cat::Exec};
      Source far = {first, Out(*first, "False"), Cat::Exec};
      GenNode *step = Chain(near, "LogicNativeApplyMovement");
      FillInputs(*step, 0);
      GenNode *second = Chain(near, "LogicNativeBranch");
      Link(*compare, Out(*compare, "Result"), *second, In(*second, "Condition"));
      Source after_move = {second, Out(*second, "False"), Cat::Exec};
      GenNode *store = Chain(after_move, "LogicNativeSetGamePropertyFloat");
      Link(*distance, distance_out, *store, In(*store, "Value"));
      FillInputs(*store, 0);
      GenNode *turn = Chain(far, "LogicNativeApplyRotation");
      FillInputs(*turn, 0);
      GenNode *walk = Chain(far, "LogicNativeApplyMovement");
      FillInputs(*walk, 0);
      BuildFlow(after_move, 2, 2);
    }
    else if (kind == 1) {
      /* A stateful node read by both operands: its value changes when it runs, not when read. */
      GenNode *store = Chain(cursor, "LogicNativeStoreValue");
      FillInputs(*store, 0);
      GenNode *math = AddFamilyNode(*FindFamily("LogicNativeMath"));
      SetCustom(*math, m_rng.pick(FindFamily("LogicNativeMath")->custom1), 0);
      Link(*store, Out(*store, "Value"), *math, In(*math, "A"));
      Link(*store, Out(*store, "Value"), *math, In(*math, "B"));
      GenNode *set = Chain(cursor, "LogicNativeSetGamePropertyFloat");
      Link(*math, Out(*math, "Result"), *set, In(*set, "Value"));
      FillInputs(*set, 0);
      GenNode *move = Chain(cursor, "LogicNativeApplyMovement");
      FillInputs(*move, 0);
      BuildFlow(cursor, 2, 2);
    }
    else {
      /* A distance in one operand (it applies the pending moves), a live attribute in the other
       * (it reads the tick's snapshot). */
      GenNode *move = Chain(cursor, "LogicNativeApplyMovement");
      FillInputs(*move, 0);
      GenNode *separate = AddFamilyNode(*FindFamily("LogicNativeSeparateXYZ"));
      if (const std::optional<Source> vector = MakeLeaf(Cat::Vector)) {
        Link(*vector->node, vector->output, *separate, In(*separate, "Vector"));
      }
      GenNode *math = AddFamilyNode(*FindFamily("LogicNativeMath"));
      SetCustom(*math, m_rng.pick(FindFamily("LogicNativeMath")->custom1), 0);
      Link(*distance, distance_out, *math, In(*math, "A"));
      Link(*separate, Out(*separate, m_rng.chance(0.5) ? "X" : "Y"), *math, In(*math, "B"));
      GenNode *set = Chain(cursor, "LogicNativeSetGamePropertyFloat");
      Link(*math, Out(*math, "Result"), *set, In(*set, "Value"));
      FillInputs(*set, 0);
      BuildFlow(cursor, 3, 2);
    }
  }

  static bool IsTimePin(const std::string &pin)
  {
    return pin == "Delay" || pin == "Seconds" || pin == "Gap" || pin == "Duration" ||
           pin == "Count" || pin == "Limit" || pin == "Ticks to Complete" || pin == "Length";
  }

  /** Gives the node's unlinked data inputs a link or a constant. */
  void FillInputs(GenNode &node, const int depth)
  {
    const std::string idname = node.meta->idname;
    for (size_t index = 0; index < node.inputs.size(); index++) {
      const Cat category = node.input_category(index);
      const PinMetadata &pin = node.in_pins[index];
      if (category == Cat::Exec || node.input_linked[index]) {
        continue;
      }
      if (category == Cat::Object) {
        continue; /* empty: the owner (End B of Get Distance is set below) */
      }
      if (idname == "LogicNativeGate") {
        continue; /* the one multi-link input is filled below */
      }
      if (std::string(pin.identifier) == "Property") {
        SetPropertyName(node, index);
        continue;
      }
      /* Times and counts stay constants: a linked NaN or a huge count would only make a test
       * slow or endless, and says nothing about the kinds. */
      if (IsTimePin(pin.identifier) || std::string(pin.identifier) == "Format String") {
        SetConstant(node, index);
        continue;
      }
      const bool must = pin.requires_link || category == Cat::Generic || m_live_inputs;
      const bool link = must || m_rng.chance(0.5);
      if (link && (must || depth < m_max_depth)) {
        const Cat wanted = category == Cat::Generic ? node.generic : category;
        if (const std::optional<Source> source = MakeData(wanted, depth + 1)) {
          if (m_fold != nullptr && FoldRead(*source, node, index)) {
            continue;
          }
          Link(*source->node, source->output, node, index);
          continue;
        }
      }
      SetConstant(node, index);
    }
    if (idname == "LogicNativeGate") {
      /* Boolean Combine: one to three conditions into the one multi-link socket. */
      const int count = node.node.custom1 == 3 ? 1 : 1 + int(m_rng.below(3)); /* Not takes one */
      for (int c = 0; c < count; c++) {
        if (const std::optional<Source> source = MakeData(Cat::Bool, depth + 1)) {
          InitLink(m_result->links.emplace_back(),
                   source->node->node,
                   *source->node->outputs[source->output],
                   node.node,
                   *node.inputs[0]);
          Append(m_result->tree.links, m_result->links.back());
          node.input_linked[0] = true;
        }
      }
    }
    if (idname == "LogicNativeGetDistance" && node.node.custom2 == 0) {
      /* End B an object: the world's Target (End A empty is the owner). */
      static_cast<blender::bNodeSocketValueObject *>(node.inputs[2]->default_value)->value =
          &TargetBlenderObject();
    }
  }

  /** A read of a game property whose value is known, into an input of the same type: the value
   * as the input's constant instead of the link. */
  bool FoldRead(const Source &source, GenNode &node, const size_t index)
  {
    const std::string leaf = source.node->meta->idname;
    if (leaf.rfind("LogicNativeGetGameProperty", 0) != 0 || source.category != node.input_category(index) ||
        node.inputs[index]->default_value == nullptr)
    {
      return false;
    }
    const std::string name = static_cast<blender::bNodeSocketValueString *>(
                                 source.node->inputs[In(*source.node, "Property")]->default_value)
                                 ->value;
    void *value = node.inputs[index]->default_value;
    switch (source.category) {
      case Cat::Float:
        static_cast<blender::bNodeSocketValueFloat *>(value)->value = m_fold->floats.at(name);
        return true;
      case Cat::Int:
        static_cast<blender::bNodeSocketValueInt *>(value)->value = m_fold->ints.at(name);
        return true;
      case Cat::Bool:
        static_cast<blender::bNodeSocketValueBoolean *>(value)->value = m_fold->bools.at(name);
        return true;
      case Cat::String:
        SetCString(static_cast<blender::bNodeSocketValueString *>(value)->value,
                   m_fold->strings.at(name).substr(0, 63).c_str());
        return true;
      default:
        return false;
    }
  }

  void SetPropertyName(GenNode &node, const size_t index)
  {
    const std::string idname = node.meta->idname;
    static const std::vector<std::string> tree_names = {"tp0", "tp1"};
    const std::vector<std::string> *names = &FloatNames();
    if (idname.find("TreeProperty") != std::string::npos) {
      names = &tree_names; /* few names, so reads and writes meet */
    }
    else if (idname.find("Int") != std::string::npos) {
      names = &IntNames();
    }
    else if (idname.find("Bool") != std::string::npos) {
      names = &BoolNames();
    }
    else if (idname.find("String") != std::string::npos) {
      names = &StringNames();
    }
    SetCString(
        static_cast<blender::bNodeSocketValueString *>(node.inputs[index]->default_value)->value,
        m_rng.pick(*names).c_str());
  }

  void SetConstant(GenNode &node, const size_t index)
  {
    void *value = node.inputs[index]->default_value;
    if (value == nullptr) {
      return;
    }
    const std::string pin = node.in_pins[index].identifier;
    switch (node.input_category(index)) {
      case Cat::Float:
        static_cast<blender::bNodeSocketValueFloat *>(value)->value = IsTimePin(pin) ?
                                                                          m_rng.real(0.0f, 0.12f) :
                                                                          EdgeFloat(m_rng);
        break;
      case Cat::Int:
        static_cast<blender::bNodeSocketValueInt *>(value)->value =
            IsTimePin(pin) ? 1 + int32_t(m_rng.below(3)) : EdgeInt(m_rng);
        break;
      case Cat::Bool:
        static_cast<blender::bNodeSocketValueBoolean *>(value)->value = m_rng.chance(0.5);
        break;
      case Cat::String:
        SetCString(static_cast<blender::bNodeSocketValueString *>(value)->value,
                   EdgeString(m_rng).substr(0, 63).c_str());
        break;
      case Cat::Vector: {
        blender::bNodeSocketValueVector &vector =
            *static_cast<blender::bNodeSocketValueVector *>(value);
        /* Moves are mostly modest: a NaN or 1e30 position hides every later distance. */
        const bool modest = std::string(node.meta->idname) == "LogicNativeApplyMovement" &&
                            m_rng.chance(0.85);
        for (int i = 0; i < 3; i++) {
          vector.value[i] = modest ? m_rng.real(-2.5f, 2.5f) : EdgeFloat(m_rng);
        }
        break;
      }
      case Cat::Rotation: {
        blender::bNodeSocketValueRotation &rotation =
            *static_cast<blender::bNodeSocketValueRotation *>(value);
        for (int i = 0; i < 3; i++) {
          rotation.value_euler[i] = m_rng.real(-3.2f, 3.2f);
        }
        break;
      }
      case Cat::Color: {
        blender::bNodeSocketValueRGBA &color = *static_cast<blender::bNodeSocketValueRGBA *>(value);
        for (int i = 0; i < 4; i++) {
          color.value[i] = m_rng.real(0.0f, 1.0f);
        }
        break;
      }
      default:
        break;
    }
    if (std::string(node.meta->idname) == "LogicNativeFormattedString" && pin == "Format String") {
      static const char *const formats[] = {"{}", "x {} y {}", "{}{}{}", "", "[{}]"};
      SetCString(static_cast<blender::bNodeSocketValueString *>(value)->value,
                 formats[m_rng.below(5)]);
    }
  }

  /** A data source of category `wanted`: a leaf (a property read), a node of the table, or an
   * output already made (a shared source: a DAG, and a state node read twice). */
  std::optional<Source> MakeData(const Cat wanted, const int depth)
  {
    std::vector<Source> &pool = m_pool[wanted];
    if (!pool.empty() && m_rng.chance(0.3)) {
      return pool[m_rng.below(uint32_t(pool.size()))];
    }
    const bool leaf = depth >= m_max_depth || m_rng.chance(0.3);
    if (leaf) {
      if (std::optional<Source> source = MakeLeaf(wanted)) {
        return source;
      }
    }
    if (const Family *family = PickDataFamily(wanted)) {
      GenNode *node = AddFamilyNode(*family);
      /* A generic node follows the wanted type when it can. */
      if (wanted == Cat::Float || wanted == Cat::Int || wanted == Cat::Bool ||
          wanted == Cat::String || wanted == Cat::Vector)
      {
        if (family->generic.empty() ||
            std::find(family->generic.begin(), family->generic.end(), wanted) !=
                family->generic.end())
        {
          node->generic = wanted;
        }
      }
      FillInputs(*node, depth);
      /* An output that fits. */
      std::vector<size_t> fitting;
      for (size_t out = 0; out < node->outputs.size(); out++) {
        /* Finding F1 (lean plan A3): a Get Distance output (a number from a query) linked into a
         * String input of String Operation (Contains, Starts With, Ends With) does not compile:
         * the consumer reports "requires a boolean input" and the node a Branch or Apply
         * Rotation reads fails with it. Left out until the compiler takes it. */
        if (wanted == Cat::String && node->output_category(out) != Cat::String &&
            std::string(node->meta->idname) == "LogicNativeGetDistance")
        {
          continue;
        }
        if (node->output_allowed(out) &&
            (CanLink(node->output_category(out), wanted) ||
             (CategoryOf(node->out_pins[out].type) == Cat::Generic && node->generic == wanted)))
        {
          fitting.push_back(out);
        }
      }
      if (!fitting.empty()) {
        const size_t out = fitting[m_rng.below(uint32_t(fitting.size()))];
        Source source{node, out, node->output_category(out)};
        pool.push_back(source);
        return source;
      }
    }
    return MakeLeaf(wanted);
  }

  std::optional<Source> MakeLeaf(const Cat wanted)
  {
    const Family *family = nullptr;
    switch (wanted) {
      case Cat::Float:
        family = &LeafFamily("LogicNativeGetGamePropertyFloat");
        break;
      case Cat::Int:
        family = &LeafFamily("LogicNativeGetGamePropertyInt");
        break;
      case Cat::Bool:
        family = &LeafFamily("LogicNativeGetGamePropertyBool");
        break;
      case Cat::String:
        family = &LeafFamily("LogicNativeGetGamePropertyString");
        break;
      case Cat::Vector:
        /* The owner's pose or velocity, or one of its axes. */
        family = FindFamily(m_rng.chance(0.3) && m_excluded.count("LogicNativeGetAxisVector") == 0 ?
                                "LogicNativeGetAxisVector" :
                                "LogicNativeGetObjectAttribute");
        break;
      default:
        return std::nullopt;
    }
    GenNode *node = AddFamilyNode(*family);
    FillInputs(*node, m_max_depth);
    for (size_t out = 0; out < node->outputs.size(); out++) {
      if (node->output_allowed(out) && node->output_category(out) == wanted) {
        return Source{node, out, wanted};
      }
    }
    return std::nullopt;
  }

  static const Family &LeafFamily(const std::string &idname)
  {
    static std::map<std::string, Family> leaves;
    auto found = leaves.find(idname);
    if (found == leaves.end()) {
      static std::deque<std::string> keep;
      keep.push_back(idname);
      found = leaves.emplace(idname, Family{keep.back().c_str(), Role::Data, 1}).first;
    }
    return found->second;
  }
};

/* ---- a tree built by hand (lean runtime plan, phase A4) ---- */

/** Builds one node tree from the node descriptors by node idname and pin identifier, for the
 * semantics tests: every pin exists as the editor declares it and starts at the descriptor's
 * default (unlike the random generator, which sets its own constants). Nodes and links live as
 * long as the builder. */
class TreeBuilder {
 public:
  TreeBuilder() : m_result(std::make_unique<GeneratedTree>())
  {
  }

  /** Adds a node; `variadic` numbered pins for a pin the descriptor marks variadic. */
  GenNode &Add(const char *idname,
               const int16_t custom1 = 0,
               const int16_t custom2 = 0,
               const int variadic = 2)
  {
    const NodeMetadata *meta = blender::nodes::logic::find_logic_node_metadata(idname);
    assert(meta != nullptr && "unknown node idname");
    auto owned = std::make_unique<GenNode>();
    GenNode &gen = *owned;
    gen.meta = meta;
    const int32_t identifier = m_result->next_identifier++;
    InitNode(gen.node,
             idname,
             (std::string(meta->ui_name) + "." + std::to_string(identifier)).c_str(),
             identifier);
    gen.node.custom1 = custom1;
    gen.node.custom2 = custom2;
    for (const bool input : {true, false}) {
      for (const PinMetadata &pin : input ? meta->inputs : meta->outputs) {
        if (pin.is_variadic) {
          for (int number = 0; number < variadic; number++) {
            PinMetadata copy = pin;
            gen.names.push_back(std::string(pin.identifier) + std::to_string(number));
            copy.identifier = gen.names.back().c_str();
            copy.ui_name = copy.identifier;
            copy.is_variadic = false;
            AddPin(gen, input, copy);
          }
        }
        else {
          AddPin(gen, input, pin);
        }
      }
    }
    Append(m_result->tree.nodes, gen.node);
    gen.label = std::string(idname + 11) + " c1=" + std::to_string(custom1) +
                " c2=" + std::to_string(custom2);
    m_result->nodes.push_back(std::move(owned));
    return *m_result->nodes.back();
  }

  /** `from.out` into `to.in` (pin identifiers). */
  void Link(GenNode &from, const char *out, GenNode &to, const char *in)
  {
    const size_t from_index = PinIndex(from.out_pins, out);
    const size_t to_index = PinIndex(to.in_pins, in);
    InitLink(m_result->links.emplace_back(),
             from.node,
             *from.outputs[from_index],
             to.node,
             *to.inputs[to_index]);
    Append(m_result->tree.links, m_result->links.back());
    to.input_linked[to_index] = true;
  }

  void Float(GenNode &node, const char *pin, const float value)
  {
    static_cast<blender::bNodeSocketValueFloat *>(Default(node, pin))->value = value;
  }
  void Int(GenNode &node, const char *pin, const int32_t value)
  {
    static_cast<blender::bNodeSocketValueInt *>(Default(node, pin))->value = value;
  }
  void Bool(GenNode &node, const char *pin, const bool value)
  {
    static_cast<blender::bNodeSocketValueBoolean *>(Default(node, pin))->value = value;
  }
  void String(GenNode &node, const char *pin, const char *value)
  {
    SetCString(static_cast<blender::bNodeSocketValueString *>(Default(node, pin))->value, value);
  }
  void Vector(GenNode &node, const char *pin, const float x, const float y, const float z)
  {
    blender::bNodeSocketValueVector &vector = *static_cast<blender::bNodeSocketValueVector *>(
        Default(node, pin));
    vector.value[0] = x;
    vector.value[1] = y;
    vector.value[2] = z;
  }
  void Rotation(GenNode &node, const char *pin, const float x, const float y, const float z)
  {
    blender::bNodeSocketValueRotation &rotation =
        *static_cast<blender::bNodeSocketValueRotation *>(Default(node, pin));
    rotation.value_euler[0] = x;
    rotation.value_euler[1] = y;
    rotation.value_euler[2] = z;
  }
  /** A constant object (the Blender object's name, without the ID code, is the game object's). */
  void ObjectNamed(GenNode &node, const char *pin, blender::Object *object)
  {
    static_cast<blender::bNodeSocketValueObject *>(Default(node, pin))->value = object;
  }

  /** The value nodes (Value String / Color ...) keep their value on the output socket. */
  void OutputString(GenNode &node, const char *pin, const char *value)
  {
    blender::bNodeSocketValueString &holder = node.strings.emplace_back();
    SetCString(holder.value, value);
    node.outputs[PinIndex(node.out_pins, pin)]->default_value = &holder;
  }
  void OutputColor(GenNode &node, const char *pin, const float r, const float g, const float bl, const float a)
  {
    blender::bNodeSocketValueRGBA &holder = node.colors.emplace_back();
    holder.value[0] = r;
    holder.value[1] = g;
    holder.value[2] = bl;
    holder.value[3] = a;
    node.outputs[PinIndex(node.out_pins, pin)]->default_value = &holder;
  }

  GeneratedTree &Tree()
  {
    return *m_result;
  }
  /** The program; null when it does not compile (the report is in `r_report`). */
  std::shared_ptr<LN_Program> Compile(std::string *r_report = nullptr)
  {
    std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(m_result->tree);
    if (r_report != nullptr && program != nullptr) {
      *r_report = FormatCompileReport(program->GetCompileReport());
    }
    if (program != nullptr && program->GetCompileReport().HasErrors()) {
      return nullptr;
    }
    return program;
  }

 private:
  std::unique_ptr<GeneratedTree> m_result;

  static size_t PinIndex(const std::vector<PinMetadata> &pins, const char *identifier)
  {
    for (size_t index = 0; index < pins.size(); index++) {
      if (std::string(pins[index].identifier) == identifier) {
        return index;
      }
    }
    std::fprintf(stderr, "TreeBuilder: no pin '%s'\n", identifier);
    std::abort();
  }

  static void *Default(GenNode &node, const char *pin)
  {
    void *value = node.inputs[PinIndex(node.in_pins, pin)]->default_value;
    if (value == nullptr) {
      std::fprintf(stderr, "TreeBuilder: pin '%s' has no constant (a generic pin: link it)\n", pin);
      std::abort();
    }
    return value;
  }

  static std::vector<float> Numbers(const char *text)
  {
    std::vector<float> numbers;
    const char *at = text;
    while (*at != '\0') {
      char *end = nullptr;
      const float value = std::strtof(at, &end);
      if (end == at) {
        at++;
        continue;
      }
      numbers.push_back(value);
      at = end;
    }
    return numbers;
  }

  /** The pin as the editor declares it, its constant at the descriptor's default. */
  static void AddPin(GenNode &gen, const bool input, const PinMetadata &pin)
  {
    gen.add_socket(input, pin);
    if (!input || gen.inputs.back()->default_value == nullptr) {
      return;
    }
    void *value = gen.inputs.back()->default_value;
    const std::string text = pin.default_value != nullptr ? pin.default_value : "";
    const std::vector<float> numbers = Numbers(text.c_str());
    switch (CategoryOf(pin.type)) {
      case Cat::Float:
        static_cast<blender::bNodeSocketValueFloat *>(value)->value = numbers.empty() ? 0.0f :
                                                                                         numbers[0];
        break;
      case Cat::Int:
        static_cast<blender::bNodeSocketValueInt *>(value)->value = numbers.empty() ?
                                                                         0 :
                                                                         int32_t(numbers[0]);
        break;
      case Cat::Bool:
        static_cast<blender::bNodeSocketValueBoolean *>(value)->value = text == "true" ||
                                                                         text == "1";
        break;
      case Cat::String:
        SetCString(static_cast<blender::bNodeSocketValueString *>(value)->value, text.c_str());
        break;
      case Cat::Vector:
        for (int i = 0; i < 3 && i < int(numbers.size()); i++) {
          static_cast<blender::bNodeSocketValueVector *>(value)->value[i] = numbers[i];
        }
        break;
      case Cat::Rotation:
        for (int i = 0; i < 3 && i < int(numbers.size()); i++) {
          static_cast<blender::bNodeSocketValueRotation *>(value)->value_euler[i] = numbers[i];
        }
        break;
      case Cat::Color:
        for (int i = 0; i < 4 && i < int(numbers.size()); i++) {
          static_cast<blender::bNodeSocketValueRGBA *>(value)->value[i] = numbers[i];
        }
        break;
      default:
        break;
    }
  }
};

/* ---- the driver ---- */

/** What one run of a program in a small world does. */
struct RunOptions {
  /** Owners (each its own tree of the program); "Target" is always there too. 1, 3 or 17. */
  uint32_t objects = 3;
  uint64_t world_seed = 1;
  uint32_t frames = 8;
  /** The per-tick memo of pure expressions: -1 and 1 on (production), 0 off. Results must not
   * depend on it (plan phase A15). */
  int memo = -1;
  /** The lean VM (plan phase A16): -1 as set (environment or test setter), 0 the old interpreter,
   * 1 Auto, 2 Force. Results must not depend on it. */
  int lean = -1;
  /** Block execution (plan phase B1): -1 as set (environment or test setter), 0 off, 1 Auto,
   * 2 Force. Only inside a tree batch (chunks > 0). Results must not depend on it. */
  int blocks = -1;
  /** Compiled routes (plan phase A33): -1 as set (environment or test setter), 0 off, 1 on. Results
   * must not depend on it. */
  int routes = -1;
  /** The lean reader's memo (plan phase A33): -1 as set, 0 off, 1 on. Results must not depend on
   * it. */
  int lean_memo = -1;
  /** 0: the trees record one after the other (production for fewer than 64 trees); more: a tree
   * batch with this many chunks, on a pool with `helpers` helper threads. */
  uint32_t chunks = 0;
  uint32_t helpers = 0;
  /** Add every tree's state meaning to the text. */
  bool states = true;
  /** Serial recording with a profile counter set per tree, as with UPBGE_LOGIC_NODES_PROFILE. */
  bool profiled = false;
  /** Every owner holds the same property values (FoldValuesFor) and none changes between frames:
   * the setting of the folded-against-runtime pair. */
  bool identical_objects = false;
};

struct RunResult {
  /** Everything two runs are compared by. */
  std::string text;
  /** The world at the end and what was printed: what the folded-against-runtime pair compares. */
  std::string final_world;
  /** Something no variant may do (a batch safety net firing, a Warning). */
  std::string failure;
  bool batched = false;
  uint64_t tree_runs = 0;
  uint64_t warnings = 0;
};

/** The kinds (by catalog name) a program uses, with how many times. */
inline std::map<std::string, uint32_t> KindsOf(const LN_Program &program)
{
  std::map<std::string, uint32_t> kinds;
  const std::string text = LN_DumpProgramText(program);
  static const char *const domains[] = {
      "Op::", "Bool::", "Int::", "Float::", "String::", "Vector::", "Color::", "Value::", "Query::"};
  for (const char *domain : domains) {
    const size_t length = std::strlen(domain);
    for (size_t at = text.find(domain); at != std::string::npos; at = text.find(domain, at + 1)) {
      if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) != 0)) {
        continue; /* "Query::" inside "..Query::" of another word: only whole domain names */
      }
      size_t end = at + length;
      while (end < text.size() &&
             (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_'))
      {
        end++;
      }
      kinds[text.substr(at, end - at)]++;
    }
  }
  return kinds;
}

inline uint64_t CountWarnings(const std::string &text)
{
  uint64_t count = 0;
  for (size_t at = text.find("Warning"); at != std::string::npos; at = text.find("Warning", at + 1)) {
    count++;
  }
  return count;
}

/** The manager's tick, for a few trees of one program: decide who runs, snapshots, warm-ups,
 * recording, EndRecording, Flush, in the manager's order and with its context fields. */
inline RunResult RunWorld(const std::shared_ptr<LN_Program> &program, const RunOptions &options)
{
  RunResult result;
  Rng world(options.world_seed);
  /* What the run prints (the Print node, warnings) is caught once for the whole run: a capture
   * costs a temporary file. */
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();

  /* The world: owners with properties of every type, a pose each, and the Target. */
  std::deque<BatchObject> objects;
  for (uint32_t index = 0; index <= options.objects; index++) {
    const bool is_target = index == options.objects;
    const float angle = float(index) * 0.9f;
    const float radius = is_target ? 5.0f : (index == 0 ? 0.0f : 1.5f + float(index % 5));
    objects.emplace_back(MT_Vector3(is_target ? 3.0f : radius * std::cos(angle),
                                    is_target ? 4.0f : radius * std::sin(angle),
                                    is_target ? 0.0f : float(index % 3) * 0.5f));
    KX_GameObject &object = objects.back().object;
    object.SetName(is_target ? "Target" : "Obj" + std::to_string(index));
    if (!is_target) {
      if (index % 3 != 0) { /* a non-identity turn, so local and world differ */
        object.NodeSetLocalOrientation(
            MT_Matrix3x3(MT_Vector3(world.real(-3.0f, 3.0f), world.real(-1.4f, 1.4f), world.real(-3.0f, 3.0f))));
      }
      if (index % 4 == 1) {
        object.NodeSetLocalScale(MT_Vector3(2.0f, 2.0f, 2.0f));
      }
      object.NodeUpdateGS(0.0);
      if (options.identical_objects) {
        const FoldValues values = FoldValuesFor(options.world_seed);
        for (const auto &entry : values.floats) {
          object.SetProperty(entry.first, new EXP_FloatValue(entry.second, entry.first));
        }
        for (const auto &entry : values.ints) {
          object.SetProperty(entry.first, new EXP_IntValue(entry.second, entry.first));
        }
        for (const auto &entry : values.bools) {
          object.SetProperty(entry.first, new EXP_BoolValue(entry.second, entry.first));
        }
        for (const auto &entry : values.strings) {
          object.SetProperty(entry.first, new EXP_StringValue(entry.second, entry.first));
        }
      }
      else {
        for (const std::string &name : FloatNames()) {
          object.SetProperty(name, new EXP_FloatValue(EdgeFloat(world), name));
        }
        for (const std::string &name : IntNames()) {
          object.SetProperty(name, new EXP_IntValue(EdgeInt(world), name));
        }
        for (const std::string &name : BoolNames()) {
          object.SetProperty(name, new EXP_BoolValue(world.chance(0.5), name));
        }
        for (const std::string &name : StringNames()) {
          object.SetProperty(name, new EXP_StringValue(EdgeString(world), name));
        }
      }
    }
  }
  std::vector<KX_GameObject *> world_objects;
  for (BatchObject &object : objects) {
    world_objects.push_back(&object.object);
  }
  LN_SceneAccessForTests access;
  access.objects = world_objects;
  LN_SetSceneAccessForTests(&access);

  std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
  std::vector<LN_RuntimeTree *> tree_pointers;
  for (uint32_t index = 0; index < options.objects; index++) {
    trees.push_back(std::make_unique<LN_RuntimeTree>(program, &objects[index].object, index, 0));
    trees.back()->SetStepSpread(index);
    tree_pointers.push_back(trees.back().get());
  }

  struct MemoScope {
    MemoScope(const int mode, const int lean_mode, const int block_mode, const int route_mode)
    {
      LN_RuntimeTree::SetMemoForTests(mode);
      if (lean_mode >= 0) {
        LN_SetLeanModeForTests(lean_mode);
      }
      restore_lean = lean_mode >= 0;
      if (block_mode >= 0) {
        LN_SetBlockModeForTests(block_mode);
      }
      restore_blocks = block_mode >= 0;
      if (route_mode >= 0) {
        LN_SetRouteModeForTests(route_mode);
      }
      restore_routes = route_mode >= 0;
    }
    void SetLeanMemo(const int lean_memo)
    {
      if (lean_memo >= 0) {
        LN_RuntimeTree::SetLeanMemoForTests(lean_memo);
      }
      restore_lean_memo = lean_memo >= 0;
    }
    ~MemoScope()
    {
      LN_RuntimeTree::SetMemoForTests(-1);
      if (restore_lean) {
        LN_SetLeanModeForTests(-1);
      }
      if (restore_blocks) {
        LN_SetBlockModeForTests(-1);
      }
      if (restore_routes) {
        LN_SetRouteModeForTests(-1);
      }
      if (restore_lean_memo) {
        LN_RuntimeTree::SetLeanMemoForTests(-1);
      }
    }
    bool restore_lean = false;
    bool restore_blocks = false;
    bool restore_routes = false;
    bool restore_lean_memo = false;
  } memo_scope(options.memo, options.lean, options.blocks, options.routes);
  memo_scope.SetLeanMemo(options.lean_memo);
  const bool eligible = program->GetTreeBatchProfile().eligible;

  LN_JobPool pool;
  pool.SetHelperCount(options.helpers);
  LN_TreeBatchRecorder recorder;
  LN_CommandBuffer buffer;
  /* As the scene's buffer (plan phase A33's pending move marks): the world's objects are real. */
  buffer.SetMarksPendingMoves(true);

  Rng environment(options.world_seed ^ 0xA5A5A5A5ull);
  const double step = 1.0 / 60.0;
  double time = 0.0;
  uint64_t tick_index = 0, update_index = 0, fixed_index = 0;

  const auto tick = [&](const LN_Event event) {
    LN_TickContext context;
    context.event = event;
    context.current_time = time;
    context.delta_time = step;
    context.unscaled_dt = step;
    context.use_fixed_timestep = true;
    context.tick_index = tick_index++;
    context.phase_tick_index = event == LN_Event::OnUpdate ? update_index++ : fixed_index++;
    const bool fixed = event == LN_Event::OnFixedUpdate;

    std::vector<LN_RuntimeTree *> runnable;
    for (LN_RuntimeTree *tree : tree_pointers) {
      if (fixed && tree->HasStepGates()) {
        tree->PrepareStepGates(context.phase_tick_index, step, step);
      }
      if (tree->CanTick(event)) {
        runnable.push_back(tree);
      }
    }
    LN_TickReadContext read;
    read.has_timing = true;
    read.frame_delta = float(step);
    read.elapsed_time = float(time);
    read.fps = 60.0f;
    read.tick_index = context.tick_index;
    read.has_gravity = true;
    for (LN_RuntimeTree *tree : runnable) {
      if (tree->ShouldCaptureSnapshot()) {
        tree->CaptureSnapshot(&read, float(step), context.tick_index, false);
      }
    }
    for (LN_RuntimeTree *tree : runnable) {
      tree->WarmQueryCache(context);
    }

    buffer.Clear();
    buffer.BeginRecording();
    size_t at = 0;
    while (at < runnable.size()) {
      size_t end = at;
      if (options.chunks > 0 && eligible) {
        while (end < runnable.size() && runnable[end]->IsInitExecuted()) {
          end++;
        }
      }
      if (end - at >= 2) {
        const size_t read_a_move = recorder.Record(pool,
                                                   buffer,
                                                   runnable.data() + at,
                                                   end - at,
                                                   context,
                                                   nullptr,
                                                   nullptr,
                                                   std::min<size_t>(options.chunks, end - at));
        result.batched = true;
        if (read_a_move != 0) {
          result.failure += "tick " + std::to_string(context.tick_index) +
                            ": a batch chunk read a pending move\n";
        }
        at = end;
      }
      else {
        LN_RuntimeProfileCounters counters;
        runnable[at]->ExecuteReady(buffer, context, options.profiled ? &counters : nullptr);
        at++;
      }
    }
    buffer.EndRecording();
    result.tree_runs += runnable.size();

    using Order = LN_CommandBuffer::DumpOrder;
    result.text += "== tick " + std::to_string(context.tick_index) + " " +
                   (fixed ? "fixed" : "update") + " runs=" + std::to_string(runnable.size()) + "\n";
    /* The world only changes when something was recorded: a tick that recorded nothing leaves it
     * as the last dump shows it (and the final dump below). Trees that did not run keep their
     * state, so only the ones that ran are dumped each tick. */
    const bool recorded = buffer.Size() > 0;
    if (recorded) {
      result.text += "-- record\n" + buffer.DumpRecordedForTests(Order::Record);
      result.text += "-- flush\n" + buffer.DumpRecordedForTests(Order::Flush);
      result.text += "-- coalesced\n" + buffer.DumpRecordedForTests(Order::FlushCoalesced);
    }
    buffer.Flush();
    if (recorded) {
      result.text += "-- after flush\n" + DumpWorld(world_objects);
    }
    if (options.states) {
      for (LN_RuntimeTree *tree : runnable) {
        result.text += "-- state " + std::to_string(tree->GetSceneObjectIndex()) + "\n" +
                       tree->DumpStateForTests(context.tick_index).meaning;
      }
    }
    time += step;
  };

  for (uint32_t frame = 0; frame < options.frames; frame++) {
    /* The environment changes now and then: a property replaced by another value (the property
     * map changes, so the snapshots must find it again). Same in every variant. */
    if (options.objects > 0 && !options.identical_objects && environment.chance(0.5)) {
      KX_GameObject &object = objects[environment.below(options.objects)].object;
      switch (environment.below(4)) {
        case 0: {
          const std::string &name = environment.pick(FloatNames());
          object.SetProperty(name, new EXP_FloatValue(EdgeFloat(environment), name));
          break;
        }
        case 1: {
          const std::string &name = environment.pick(IntNames());
          object.SetProperty(name, new EXP_IntValue(EdgeInt(environment), name));
          break;
        }
        case 2: {
          const std::string &name = environment.pick(BoolNames());
          object.SetProperty(name, new EXP_BoolValue(environment.chance(0.5), name));
          break;
        }
        default: {
          const std::string &name = environment.pick(StringNames());
          object.SetProperty(name, new EXP_StringValue(EdgeString(environment), name));
          break;
        }
      }
    }
    tick(LN_Event::OnUpdate);
    const uint32_t steps = (frame % 3 == 2) ? 2 : 1;
    for (uint32_t s = 0; s < steps; s++) {
      tick(LN_Event::OnFixedUpdate);
    }
  }

  result.final_world = DumpWorld(world_objects);
  result.text += "== end\n" + result.final_world;
  if (options.states) {
    for (size_t index = 0; index < tree_pointers.size(); index++) {
      result.text += "-- state " + std::to_string(index) + "\n" +
                     tree_pointers[index]->DumpStateForTests(tick_index - 1).meaning;
    }
  }
  tree_pointers.clear();
  trees.clear();
  LN_SetSceneAccessForTests(nullptr);
  const std::string printed = testing::internal::GetCapturedStdout();
  const std::string errors = testing::internal::GetCapturedStderr();
  result.warnings = CountWarnings(printed) + CountWarnings(errors);
  result.text += "== printed\n" + printed;
  result.final_world += "== printed\n" + printed;
  return result;
}

/** The first line where two texts differ, for a report: the part of both lines around the first
 * character that differs (the dump lines are long). */
inline std::string FirstDifference(const std::string &a, const std::string &b)
{
  size_t start = 0;
  size_t line = 1;
  std::string last_tick;
  while (start < a.size() && start < b.size()) {
    const size_t end_a = a.find('\n', start);
    const size_t end_b = b.find('\n', start);
    if (end_a != end_b || a.compare(start, end_a - start, b, start, end_b - start) != 0) {
      break;
    }
    if (a.compare(start, 7, "== tick") == 0) {
      last_tick = a.substr(start, end_a - start);
    }
    start = end_a + 1;
    line++;
  }
  const auto line_of = [&](const std::string &t) {
    if (start >= t.size()) {
      return std::string();
    }
    const size_t at = t.find('\n', start);
    return t.substr(start, (at == std::string::npos ? t.size() : at) - start);
  };
  const std::string first = line_of(a), second = line_of(b);
  size_t column = 0;
  while (column < first.size() && column < second.size() && first[column] == second[column]) {
    column++;
  }
  const size_t from = column > 70 ? column - 70 : 0;
  const auto window = [&](const std::string &text) {
    if (text.empty()) {
      return std::string("(end)");
    }
    return std::string(from > 0 ? "..." : "") + text.substr(from, 170) +
           (text.size() > from + 170 ? "..." : "");
  };
  /* The name of the thing the line is about: its first two words. */
  std::string head = first.substr(0, first.find(' ', first.find(' ') + 1));
  return "line " + std::to_string(line) + " after '" + last_tick + "' (" + head + ")\n    first: " +
         window(first) + "\n   second: " + window(second);
}

}  // namespace ln_test::diff

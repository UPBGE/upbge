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
#include <cstdio>
#include <cstdlib>
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

#include "KX_NodeRelationships.h"
#include "LN_Columns.h"
#include "LN_CommandDiagnostics.h"
#include "LN_JobPool.h"
#include "LN_KindCatalog.h"
#include "LN_Manager.h"
#include "LN_Random.h"
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
  /** Compiles only behind a Logic + Physics Step root (Required Phase Fixed Update). */
  bool fixed_only = false;
  /** The editor's socket types for collision layers, assets and scenes (GenNode::real_sockets;
   * the rows from descriptors, AutoFamily: the table's own rows keep the trees the goldens
   * hold). */
  bool real_sockets = false;
  /** A row of plan C2 P0's accepted list: random trees take it only with
   * TreeGen::SetAcceptedFamilies (the fixed seeds keep the table their goldens were recorded
   * with, and enough batch-safe trees); its probe tree is always made. */
  bool accepted_row = false;
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

/** Get Object Attribute (custom1 = the attribute): the outputs each attribute shows. Visible and
 * Color read the snapshot and are covered by their own families' reads. */
inline bool ObjectAttributeOutputOk(const int16_t custom1,
                                    const int16_t /*custom2*/,
                                    const std::string &output)
{
  const bool transform = custom1 == 13 || custom1 == 14;
  if (output == "Vector") {
    return custom1 == 0 || custom1 == 1 || custom1 == 4 || custom1 == 5 ||
           (custom1 >= 9 && custom1 <= 12);
  }
  if (output == "Orientation") {
    return custom1 == 7 || custom1 == 8 || transform;
  }
  if (output == "Position" || output == "Scale") {
    return transform;
  }
  return output == "Name" && custom1 == 3;
}

/** Typecast builds only the output its target (custom1) names. */
inline bool TypecastOutputOk(const int16_t custom1,
                             const int16_t /*custom2*/,
                             const std::string &output)
{
  return (output == "Integer" && custom1 == 0) || (output == "Boolean" && custom1 == 1) ||
         (output == "String" && custom1 == 2) || (output == "Float" && custom1 == 3);
}

/** Plan C2 P0: a family row worked out from a node's descriptor (its role from its pins, Logic +
 * Physics Step only when its phase says so, every output, any scalar on a generic pin, mode 0);
 * nullopt for a node the generator cannot feed (a pin that must be linked whose type it does not
 * make: lists, dictionaries, assets) and for an event without flow outputs. */
inline std::optional<Family> AutoFamily(const char *idname, const int weight)
{
  const NodeMetadata *meta = blender::nodes::logic::find_logic_node_metadata(idname);
  if (meta == nullptr) {
    return std::nullopt;
  }
  bool flow_in = false, flow_out = false;
  for (const PinMetadata &pin : meta->inputs) {
    if (pin.type == PinType::Execution) {
      flow_in = true;
      continue;
    }
    if (pin.requires_link && CategoryOf(pin.type) == Cat::None) {
      return std::nullopt;
    }
  }
  for (const PinMetadata &pin : meta->outputs) {
    flow_out = flow_out || pin.type == PinType::Execution;
  }
  Family family{meta->idname, Role::Data, weight};
  if (std::string(meta->category) == "Events" && !flow_in) {
    if (!flow_out) {
      return std::nullopt;
    }
    family.role = Role::Event;
  }
  else if (flow_in) {
    family.role = Role::Flow;
  }
  family.fixed_only = meta->required_phase == blender::nodes::logic::RequiredPhase::FixedUpdate;
  family.real_sockets = true;
  /* Random Value (2026-11-07): every type (custom1) and variation (custom2), each type's own
   * output; Get Random List Item's variation. */
  if (std::string(meta->idname) == "LogicNativeRandomValue") {
    family.custom1 = {0, 1, 2, 3};
    family.custom2 = {0, 1};
    family.output_ok = [](const int16_t custom1, const int16_t, const std::string &output) {
      static const char *outputs[] = {"Value", "Value_Int", "Value_Vector", "Value_Bool"};
      return custom1 >= 0 && custom1 < 4 && output == outputs[custom1];
    };
  }
  else if (std::string(meta->idname) == "LogicNativeListRandomItem") {
    family.custom2 = {0, 1};
  }
  return family;
}

/** UPBGE_LN_DIFF_EXTRA_FAMILIES (comma separated, "idname:weight"): nodes probed as families
 * (c2_p0/family_probe.py; the goldens are recorded without it). */
inline const std::vector<std::pair<std::string, int>> &ProbedFamilyNames()
{
  static const std::vector<std::pair<std::string, int>> names = [] {
    std::vector<std::pair<std::string, int>> out;
    if (const char *extra = std::getenv("UPBGE_LN_DIFF_EXTRA_FAMILIES")) {
      std::string text = extra;
      size_t start = 0;
      while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        std::string item = text.substr(start, comma == std::string::npos ? std::string::npos :
                                                                           comma - start);
        int weight = 1;
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
          weight = std::max(1, std::atoi(item.c_str() + colon + 1));
          item = item.substr(0, colon);
        }
        if (!item.empty()) {
          out.emplace_back(item, weight);
        }
        if (comma == std::string::npos) {
          break;
        }
        start = comma + 1;
      }
    }
    return out;
  }();
  return names;
}

/** The node idnames the table takes from their descriptors (AutoFamily) after its own rows: plan
 * C2 P0's accepted list, weight 1 (each probed alone first by c2_p0/family_probe.py: compiles
 * alone and in random trees, runs in the rig without a scene, every mode and columns end alike),
 * then the probed ones. */
inline const std::vector<std::pair<std::string, int>> &AutoFamilyNames()
{
  static const std::vector<std::pair<std::string, int>> names = [] {
    static const char *const accepted[] = {
        /* c2_p0/family_probe.tsv, 2026-11-05: accepted, and four that compile in fewer random
         * trees (While Paused, String Switch Flow, Vehicle Control, Loop From List). */
        "LogicNativeApplyForce",
        "LogicNativeApplyImpulse",
        "LogicNativeApplyTorque",
        "LogicNativeAssignGeometryNodesModifier",
        "LogicNativeBoneRotationToObjectRotation",
        "LogicNativeCharacterJump",
        "LogicNativeCombineXYZW",
        "LogicNativeCopyProperty",
        "LogicNativeCursorMovement",
        "LogicNativeCursorPosition",
        "LogicNativeDictAddValue",
        "LogicNativeDictHasKey",
        "LogicNativeDictLength",
        "LogicNativeDictRemoveKey",
        "LogicNativeDictSetKey",
        "LogicNativeDraw",
        "LogicNativeEnableDisableModifier",
        "LogicNativeEvaluateCurve",
        "LogicNativeEvaluateProperty",
        "LogicNativeFindRecipe",
        "LogicNativeFloatCurve",
        "LogicNativeFollowPath",
        "LogicNativeFormula",
        "LogicNativeGamepadActive",
        "LogicNativeGamepadStick",
        "LogicNativeGetAverageCenterPosition",
        "LogicNativeGetBoneAttribute",
        "LogicNativeGetBoneCenterPoseWorld",
        "LogicNativeGetBoneCenterWorld",
        "LogicNativeGetBoneHeadPoseWorld",
        "LogicNativeGetBoneHeadWorld",
        "LogicNativeGetBoneLength",
        "LogicNativeGetBoneTailPoseWorld",
        "LogicNativeGetBoneTailWorld",
        "LogicNativeGetCharacterInfo",
        "LogicNativeGetCollisionGroup",
        "LogicNativeGetFullscreen",
        "LogicNativeGetGameProperty",
        "LogicNativeGetGamePropertyBool",
        "LogicNativeGetGamePropertyFloat",
        "LogicNativeGetGamePropertyInt",
        "LogicNativeGetGamePropertyString",
        "LogicNativeGetGlobalProperty",
        "LogicNativeGetInputBinding",
        "LogicNativeGetItemInfo",
        "LogicNativeGetLightColor",
        "LogicNativeGetLightPower",
        "LogicNativeGetMasterFolder",
        "LogicNativeGetMaterialFromSlot",
        "LogicNativeGetMaterialName",
        "LogicNativeGetMaterialParameter",
        "LogicNativeGetMaterialSlotCount",
        "LogicNativeGetMenuPlayer",
        "LogicNativeGetObjectID",
        "LogicNativeGetPlayerGamepad",
        "LogicNativeGetRecipeInfo",
        "LogicNativeGetResolution",
        "LogicNativeGetRigidBodyConstraints",
        "LogicNativeGetTimescale",
        "LogicNativeGetVSync",
        "LogicNativeInputAction",
        "LogicNativeItemCount",
        "LogicNativeKeyLogger",
        "LogicNativeKeyboardActive",
        "LogicNativeListAppend",
        "LogicNativeListContains",
        "LogicNativeListFindIndex",
        "LogicNativeListGlobalProperties",
        "LogicNativeListLength",
        "LogicNativeListRemoveIndex",
        "LogicNativeListRemoveValue",
        "LogicNativeListSavedVariables",
        "LogicNativeListSetIndex",
        "LogicNativeLoadVariable",
        "LogicNativeLogicTreeStatus",
        "LogicNativeLoopFromList",
        "LogicNativeMakeLightUnique",
        "LogicNativeMatrixToXYZ",
        "LogicNativeModifyProperty",
        "LogicNativeMouseWheel",
        "LogicNativeNone",
        "LogicNativeNotNone",
        "LogicNativeObjectsColliding",
        "LogicNativeOnCollision",
        "LogicNativeOnConstraintBreak",
        "LogicNativeOnGameLoaded",
        "LogicNativeOnPlayerJoin",
        "LogicNativePauseSound",
        "LogicNativePlayAction",
        "LogicNativePlaySound",
        "LogicNativePlaySound3D",
        "LogicNativeRandomValue",
        "LogicNativeRaycast",
        "LogicNativeRaycastAll",
        "LogicNativeRebuildCollisionShape",
        "LogicNativeReceiveEvent",
        "LogicNativeRemoveObject",
        "LogicNativeResizeVector",
        "LogicNativeResumeSound",
        "LogicNativeScreenToWorld",
        "LogicNativeSendEvent",
        "LogicNativeSeparateEuler",
        "LogicNativeSetBoneConstraintAttribute",
        "LogicNativeSetBoneConstraintInfluence",
        "LogicNativeSetBoneConstraintTarget",
        "LogicNativeSetBonePoseLocation",
        "LogicNativeSetBonePoseRotation",
        "LogicNativeSetBonePoseScale",
        "LogicNativeSetBonePoseTransform",
        "LogicNativeSetCharacterGravity",
        "LogicNativeSetCharacterJumpSpeed",
        "LogicNativeSetCharacterMaxJumps",
        "LogicNativeSetCharacterVelocity",
        "LogicNativeSetCharacterWalkDirection",
        "LogicNativeSetCollisionGroup",
        "LogicNativeSetDynamics",
        "LogicNativeSetGameProperty",
        "LogicNativeSetGeometryNodesInput",
        "LogicNativeSetLightColor",
        "LogicNativeSetLightPower",
        "LogicNativeSetLightShadow",
        "LogicNativeSetMaterialParameter",
        "LogicNativeSetMaterialSlot",
        "LogicNativeSetNodeMute",
        "LogicNativeSetPhysics",
        "LogicNativeSetRigidBodyAttribute",
        "LogicNativeSetShapeKeyValue",
        "LogicNativeSetText",
        "LogicNativeSetVolume",
        "LogicNativeShapeCast",
        "LogicNativeShapeCastAll",
        "LogicNativeSlowFollow",
        "LogicNativeStopAllSounds",
        "LogicNativeStopSound",
        "LogicNativeSwitchFlowInt",
        "LogicNativeSwitchFlowString",
        "LogicNativeTime",
        "LogicNativeToggleProperty",
        "LogicNativeTweenValue",
        "LogicNativeValueChanged",
        "LogicNativeValueChangedTo",
        "LogicNativeVehicleAccelerate",
        "LogicNativeVehicleBrake",
        "LogicNativeVehicleControl",
        "LogicNativeVehicleSetAttributes",
        "LogicNativeVehicleSteer",
        "LogicNativeWhilePaused",
        "LogicNativeWorldToScreen",
        "LogicNativeXYZToMatrix",
    };
    std::vector<std::pair<std::string, int>> out;
    for (const char *idname : accepted) {
      out.emplace_back(idname, 1);
    }
    for (const auto &entry : ProbedFamilyNames()) {
      out.push_back(entry);
    }
    return out;
  }();
  return names;
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
    t.push_back({"LogicNativeInvertValue", Role::Data, 1});
    t.push_back({"LogicNativeMapRange", Role::Data, 2});
    t.push_back({"LogicNativeThreshold", Role::Data, 1, {0, 1}});
    t.push_back({"LogicNativeWithinRange", Role::Data, 1, {0, 1}});
    t.push_back({"LogicNativeRangedThreshold", Role::Data, 1, {0, 1}});
    t.push_back({"LogicNativeGate", Role::Data, 3, {0, 1, 2, 3, 4, 5, 6}});
    t.push_back({"LogicNativeStringOperation", Role::Data, 5, {0, 2, 3, 4, 5, 6, 7, 8, 9}, {}, {}, StringOperationOutputOk});
    t.push_back({"LogicNativeFormattedString", Role::Data, 2});
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
    t.push_back({"LogicNativeGetObjectAttribute",
                 Role::Data,
                 3,
                 {0, 1, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14},
                 {},
                 {"Vector", "Orientation", "Position", "Scale", "Name"},
                 ObjectAttributeOutputOk});
    t.push_back({"LogicNativeGetDistance", Role::Data, 3, {0, 1}, {0, 1}, {"Distance", "Squared Distance", "Valid"}, nullptr, {}, true});
    /* Added in plan C1-7 (nodes the columns gained). */
    t.push_back({"LogicNativeBooleanEdge", Role::Event, 2});
    t.push_back({"LogicNativeFlowGate", Role::Flow, 2, {}, {}, {"Is Open"}});
    t.push_back({"LogicNativeTranslate", Role::Flow, 3, {}, {}, {}, nullptr, {}, false, true});
    t.push_back({"LogicNativeMoveToward", Role::Flow, 3, {}, {}, {}, nullptr, {}, false, true});
    t.push_back({"LogicNativeRotateToward", Role::Flow, 2, {}, {}, {}, nullptr, {}, false, true});
    t.push_back({"LogicNativeAlignAxisToVector", Role::Flow, 2, {}, {}, {}, nullptr, {}, false, true});
    t.push_back({"LogicNativeGetGravity", Role::Data, 1});
    t.push_back({"LogicNativeBarrier", Role::Flow, 2});
    /* Positions, orientations (Value as Euler angles), velocities, scales, color, visibility;
     * every axis. */
    t.push_back({"LogicNativeSetObjectAttribute", Role::Flow, 3, {0, 1, 2, 3, 5, 6, 7, 8, 9, 11, 12, 13}, {15}, {}, nullptr, {}, false, true});
    t.push_back({"LogicNativeEuler", Role::Data, 2});
    t.push_back({"LogicNativeColorRGBA", Role::Data, 1});
    /* Plan C2 P0: rows worked out from descriptors (AutoFamilyNames), never twice. */
    for (const auto &[idname, weight] : AutoFamilyNames()) {
      const bool listed = std::any_of(t.begin(), t.end(), [&](const Family &family) {
        return idname == family.idname;
      });
      if (listed) {
        continue;
      }
      if (std::optional<Family> family = AutoFamily(idname.c_str(), weight)) {
        /* A probed name (UPBGE_LN_DIFF_EXTRA_FAMILIES) is in every random tree. */
        const auto &probed = ProbedFamilyNames();
        family->accepted_row = std::none_of(
            probed.begin(), probed.end(), [&](const auto &entry) { return entry.first == idname; });
        t.push_back(*family);
      }
      else {
        std::fprintf(stderr, "extra family skipped: %s (the generator cannot feed it)\n",
                     idname.c_str());
      }
    }
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
  /* Plan C2 P0: asset pins of a TreeBuilder with real sockets (SetRealSockets). */
  std::deque<blender::bNodeSocketValueCollection> collections;
  std::deque<blender::bNodeSocketValueMaterial> materials;
  std::deque<blender::bNodeSocketValueImage> images;
  std::deque<blender::bNodeSocketValueSound> sounds;
  std::deque<blender::bNodeSocketValueFont> fonts;
  std::deque<blender::bNodeSocketValueText> texts;
  std::deque<std::string> names;
  /** The editor's socket types for the pins the generator leaves untyped: collision layers as
   * ints, collections / materials / images / sounds / fonts / texts with their own values, a
   * scene by its name. Off for the random generator (its trees and goldens stay as they are). */
  bool real_sockets = false;
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
    if (real_sockets && datatype == blender::SOCK_CUSTOM) {
      switch (pin.type) {
        case PinType::CollisionLayers:
          datatype = blender::SOCK_INT;
          default_value = input ? &ints.emplace_back() : nullptr;
          break;
        case PinType::Collection:
          datatype = blender::SOCK_COLLECTION;
          default_value = input ? &collections.emplace_back() : nullptr;
          break;
        case PinType::Material:
          datatype = blender::SOCK_MATERIAL;
          default_value = input ? &materials.emplace_back() : nullptr;
          break;
        case PinType::Image:
          datatype = blender::SOCK_IMAGE;
          default_value = input ? &images.emplace_back() : nullptr;
          break;
        case PinType::Sound:
          datatype = blender::SOCK_SOUND;
          default_value = input ? &sounds.emplace_back() : nullptr;
          break;
        case PinType::Font:
          datatype = blender::SOCK_FONT;
          default_value = input ? &fonts.emplace_back() : nullptr;
          break;
        case PinType::Text:
          datatype = blender::SOCK_TEXT_ID;
          default_value = input ? &texts.emplace_back() : nullptr;
          break;
        case PinType::Scene:
          /* (The compiler reads a scene pin's default as its name.) */
          datatype = blender::SOCK_STRING;
          default_value = input ? &strings.emplace_back() : nullptr;
          break;
        default:
          break;
      }
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

/** Plan C1-9: the Blender objects "Leader" and "Turret" (a finder world's root object and the
 * owners' children's name, Get Child By Name's hint). */
inline blender::Object &FinderBlenderObject(const char *name)
{
  static blender::Object leader, turret;
  static const bool named = (SetCString(leader.id.name, "OBLeader"),
                             SetCString(turret.id.name, "OBTurret"),
                             true);
  (void)named;
  return std::string(name) == "Leader" ? leader : turret;
}

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

  /** Plan C1-9: Object inputs may take objects found while playing (Get Object By Name, Get
   * Child, Get Child By Name, Get Object, Get Owner) and numbers may come from Get Average Center
   * Position of an object list. Off by default (the goldens' trees stay as recorded); run such a
   * tree in a world with `RunOptions::finder_world`. */
  void SetObjectFinders(const bool on)
  {
    m_finders = on;
  }

  /** Plan C2 P0: the trees also take the accepted rows (Family::accepted_row). */
  void SetAcceptedFamilies(const bool on)
  {
    m_accepted = on;
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
      {
        const std::string root = event->meta->idname;
        m_root_fixed = root == "LogicNativeOnFixedUpdate" || root == "LogicNativeBooleanEdge";
      }
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

  /** A flow node (set to `custom1` / `custom2`) behind its event, one of its outputs used: a data
   * output written to a property every tick by a writer on the same event, a route followed by a
   * Print. The kinds behind a flow node's outputs (Play Action's Failed...) run in a probe of
   * their own, not only when a random tree uses them (2026-11-08: removing the merged nodes'
   * former rows reshuffled the seeds, and Play Action's Failed was then reached by one random tree
   * only). */
  std::unique_ptr<GeneratedTree> ProbeFlowOutput(const Family &family,
                                                 const int16_t custom1,
                                                 const int16_t custom2,
                                                 const size_t output)
  {
    m_result = std::make_unique<GeneratedTree>();
    m_live_inputs = true;
    GenNode *event = AddFamilyNode(
        *FindFamily(family.fixed_only ? "LogicNativeOnFixedUpdate" : "LogicNativeOnUpdate"));
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
    if (output >= node->outputs.size()) {
      /* Fewer outputs than the shape counted (a node whose outputs vary). */
      return std::move(m_result);
    }
    const char *sink_name = "LogicNativePrint";
    size_t sink_input = 1;
    switch (node->output_category(output)) {
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
    if (node->output_category(output) == Cat::Exec) {
      /* A route (Done, Failed...): a Print behind it, so its condition is compiled and runs. */
      Link(*node, output, *sink, 0);
    }
    else {
      Link(*event, FirstExecOutput(*event), *sink, 0);
      Link(*node, output, *sink, sink_input);
    }
    FillInputs(*sink, m_max_depth);
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
    GenNode *event = AddFamilyNode(
        *FindFamily(family.fixed_only ? "LogicNativeOnFixedUpdate" : "LogicNativeOnUpdate"));
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
  bool m_finders = false;
  bool m_accepted = false;
  const FoldValues *m_fold = nullptr;
  std::set<std::string> m_excluded;
  /** The root being built is a Logic + Physics Step event: fixed-only flow nodes may follow. */
  bool m_root_fixed = false;

  bool Picks(const Family &family, const Role role) const
  {
    return family.role == role && m_excluded.count(family.idname) == 0 &&
           (!family.fixed_only || m_root_fixed) && (!family.accepted_row || m_accepted);
  }

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
      if (Picks(family, role)) {
        total += family.weight;
      }
    }
    int roll = int(m_rng.below(uint32_t(total)));
    for (const Family &family : FamilyTable()) {
      if (!Picks(family, role)) {
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
      if (family.role != Role::Data || m_excluded.count(family.idname) != 0 ||
          (family.accepted_row && !m_accepted))
      {
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
    gen.real_sockets = family.real_sockets;
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
      if (std::string(family.idname) == "LogicNativeOnNextFrame") {
        m_root_fixed = false; /* its Completed route runs on the next frame */
      }
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
        if (m_finders && m_rng.chance(0.5)) {
          GenNode *finder = MakeFinder(depth + 1);
          Link(*finder, 0, node, index);
        }
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
        /* Collision layers (real sockets): the pin's default (every layer), no random mask. */
        if (node.real_sockets && node.in_pins[index].type == PinType::CollisionLayers) {
          const char *text = node.in_pins[index].default_value;
          static_cast<blender::bNodeSocketValueInt *>(value)->value =
              text != nullptr ? int32_t(std::strtol(text, nullptr, 0)) : 0;
        }
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
  /** Plan C1-9: the finder families (made only with SetObjectFinders). */
  static const Family &FinderFamily(const char *idname)
  {
    static const std::vector<Family> families = {
        {"LogicNativeObjectByName", Role::Data, 1, {}, {}, {}, nullptr, {}, true},
        {"LogicNativeGetChild", Role::Data, 1, {}, {}, {}, nullptr, {}, true},
        {"LogicNativeGetChildByName", Role::Data, 1, {}, {}, {}, nullptr, {}, true},
        {"LogicNativeFindObject", Role::Data, 1, {}, {}, {}, nullptr, {}, true},
        {"LogicNativeGetOwner", Role::Data, 1},
        /* Plan C1-10: a parent of a find, a name the owner keeps in a text property. */
        {"LogicNativeGetParent", Role::Data, 1, {}, {}, {}, nullptr, {}, true},
        {"LogicNativeGetGamePropertyString", Role::Data, 1},
        {"LogicNativeListFromItems", Role::Data, 1, {10}},
        {"LogicNativeGetAverageCenterPosition", Role::Data, 1, {1}, {}, {}, nullptr, {}, true},
    };
    for (const Family &family : families) {
      if (std::string(family.idname) == idname) {
        return family;
      }
    }
    return families.front();
  }

  /** An object found while playing (a node whose first output is the object). */
  GenNode *MakeFinder(const int depth)
  {
    const uint32_t roll = m_rng.below(7);
    if (roll == 5 && depth < m_max_depth + 2) {
      /* Plan C1-10: the parent of another find (a turret's NPC, an NPC's none). */
      GenNode *node = AddFamilyNode(FinderFamily("LogicNativeGetParent"));
      GenNode *child = MakeFinder(depth + 1);
      Link(*child, 0, *node, 0);
      return node;
    }
    if (roll == 6) {
      /* Plan C1-10: the object named by the owner's text property "fname" (RunOptions::
       * finder_world gives the owners one: the Target, an NPC, Turret, Leader, nobody, nothing). */
      GenNode *node = AddFamilyNode(FinderFamily("LogicNativeObjectByName"));
      GenNode *name = AddFamilyNode(FinderFamily("LogicNativeGetGamePropertyString"));
      SetCString(static_cast<blender::bNodeSocketValueString *>(
                     name->inputs[In(*name, "Property")]->default_value)
                     ->value,
                 "fname");
      Link(*name, 0, *node, In(*node, "Name"));
      return node;
    }
    if (roll == 0) {
      GenNode *node = AddFamilyNode(FinderFamily("LogicNativeObjectByName"));
      static const char *const names[] = {"Target", "Obj1", "Turret", "Leader", "Nobody"};
      SetCString(static_cast<blender::bNodeSocketValueString *>(
                     node->inputs[In(*node, "Name")]->default_value)
                     ->value,
                 names[m_rng.below(5)]);
      return node;
    }
    if (roll == 1 || roll == 2) {
      GenNode *node = AddFamilyNode(FinderFamily(roll == 1 ? "LogicNativeGetChild" :
                                                             "LogicNativeGetChildByName"));
      GenNode *owner = AddFamilyNode(FinderFamily("LogicNativeGetOwner"));
      Link(*owner, 0, *node, 0);
      if (roll == 2) {
        static_cast<blender::bNodeSocketValueObject *>(
            node->inputs[In(*node, "Child")]->default_value)
            ->value = &FinderBlenderObject("Turret");
      }
      return node;
    }
    if (roll == 3) {
      GenNode *node = AddFamilyNode(FinderFamily("LogicNativeFindObject"));
      static_cast<blender::bNodeSocketValueObject *>(node->inputs[0]->default_value)->value =
          m_rng.chance(0.5) ? &TargetBlenderObject() : &FinderBlenderObject("Leader");
      return node;
    }
    (void)depth;
    return AddFamilyNode(FinderFamily("LogicNativeGetOwner"));
  }

  /** Get Average Center Position of a list of one to three objects found as MakeFinder finds
   * them: its Center, Count or Valid. */
  std::optional<Source> MakeAverage(const Cat wanted, const int depth)
  {
    GenNode *list = AddFamilyNode(FinderFamily("LogicNativeListFromItems"));
    SetCustom(*list, 10, 0); /* the Objects input */
    const size_t items = In(*list, "Objects");
    const int count = 1 + int(m_rng.below(3));
    for (int item = 0; item < count; item++) {
      GenNode *finder = MakeFinder(depth + 1);
      InitLink(m_result->links.emplace_back(),
               finder->node,
               *finder->outputs[0],
               list->node,
               *list->inputs[items]);
      Append(m_result->tree.links, m_result->links.back());
      list->input_linked[items] = true;
    }
    GenNode *average = AddFamilyNode(FinderFamily("LogicNativeGetAverageCenterPosition"));
    SetCustom(*average, 1, 0); /* Object List */
    Link(*list, Out(*list, "Objects"), *average, In(*average, "Objects"));
    const char *output = wanted == Cat::Vector ? "Center" : wanted == Cat::Int ? "Count" : "Valid";
    const size_t out = Out(*average, output);
    Source source{average, out, average->output_category(out)};
    m_pool[wanted].push_back(source);
    return source;
  }

  std::optional<Source> MakeData(const Cat wanted, const int depth)
  {
    if (m_finders && depth < m_max_depth &&
        (wanted == Cat::Vector || wanted == Cat::Int || wanted == Cat::Bool) && m_rng.chance(0.15))
    {
      return MakeAverage(wanted, depth);
    }
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
    gen.real_sockets = m_real_sockets;
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

  /** One more output after the node's own (a merged node's extra output, NOD_logic_merged.hh). */
  void AddOutputPin(GenNode &node, const PinMetadata &pin)
  {
    AddPin(node, false, pin);
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

  /** Nodes added from now on get the editor's socket types for collision layers and asset pins
   * (GenNode::real_sockets). */
  void SetRealSockets(const bool on)
  {
    m_real_sockets = on;
  }
  /** An asset pin's value (a node added with real sockets): the ID by the socket's type. */
  void Asset(GenNode &node, const char *pin, blender::ID *id)
  {
    blender::bNodeSocket &socket = *node.inputs[PinIndex(node.in_pins, pin)];
    switch (socket.type) {
      case blender::SOCK_COLLECTION:
        static_cast<blender::bNodeSocketValueCollection *>(socket.default_value)->value =
            reinterpret_cast<blender::Collection *>(id);
        break;
      case blender::SOCK_MATERIAL:
        static_cast<blender::bNodeSocketValueMaterial *>(socket.default_value)->value =
            reinterpret_cast<blender::Material *>(id);
        break;
      case blender::SOCK_IMAGE:
        static_cast<blender::bNodeSocketValueImage *>(socket.default_value)->value =
            reinterpret_cast<blender::Image *>(id);
        break;
      case blender::SOCK_SOUND:
        static_cast<blender::bNodeSocketValueSound *>(socket.default_value)->value =
            reinterpret_cast<blender::bSound *>(id);
        break;
      case blender::SOCK_FONT:
        static_cast<blender::bNodeSocketValueFont *>(socket.default_value)->value =
            reinterpret_cast<blender::VFont *>(id);
        break;
      case blender::SOCK_TEXT_ID:
        static_cast<blender::bNodeSocketValueText *>(socket.default_value)->value =
            reinterpret_cast<blender::Text *>(id);
        break;
      case blender::SOCK_OBJECT:
        static_cast<blender::bNodeSocketValueObject *>(socket.default_value)->value =
            reinterpret_cast<blender::Object *>(id);
        break;
      default:
        break;
    }
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
  bool m_real_sockets = false;

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
        if (gen.real_sockets && pin.type == PinType::CollisionLayers) {
          static_cast<blender::bNodeSocketValueInt *>(value)->value = numbers.empty() ?
                                                                           0 :
                                                                           int32_t(numbers[0]);
        }
        else if (gen.real_sockets && pin.type == PinType::Scene) {
          SetCString(static_cast<blender::bNodeSocketValueString *>(value)->value, text.c_str());
        }
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
  /** Plan C1: trees whose program lowers run in wide runs (LN_Columns.h, runs of any length).
   * Off: every tree runs the column pass (the column system's pass for one tree). */
  bool columns = false;
  /** The text holds only the world after every tick (a column run records no commands and keeps
   * no tree bookkeeping: what a VM run and a column run are compared by). */
  bool world_only = false;
  /** Serial recording with a profile counter set per tree, as with UPBGE_LOGIC_NODES_PROFILE. */
  bool profiled = false;
  /** Every owner holds the same property values (FoldValuesFor) and none changes between frames:
   * the setting of the folded-against-runtime pair. */
  bool identical_objects = false;
  /** Plan C1-9 (trees made with TreeGen::SetObjectFinders): two owners in three get a child
   * "Turret" (half a metre beside, a metre over them), and a root "Leader" stands apart. */
  bool finder_world = false;
  /** Plan C2 P2: owners in a hierarchy: owner 1, 4, 7... is a child of the owner before it (a
   * cart and its turret, both with the tree), owner 2, 5, 8... a child of the Target (the
   * object the trees measure by name). */
  bool parented_owners = false;
  /** Plan C2 P2: every owner has the tree twice (applied tree indices 0 and 1, as an object's
   * trees): groups, or alone in their places where no group takes them (parented owners). */
  bool two_trees = false;
};

struct RunResult {
  /** Everything two runs are compared by. */
  std::string text;
  /** The world at the end and what was printed: what the folded-against-runtime pair compares. */
  std::string final_world;
  /** Something no variant may do (a batch safety net firing, a Warning). */
  std::string failure;
  bool batched = false;
  /** Columns: the reasons ticks planned no lanes (each once), and the lanes run. */
  std::string column_refusals;
  uint64_t column_lanes = 0;
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
  /* A run prints its own command warnings, whatever ran before it in the process (they print
   * once per process in the game: two runs of one tree differed in them). */
  LN_ResetCommandFailureWarningsForTests();
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
  if (options.finder_world) {
    /* Plan C1-10: a name each owner keeps (TreeGen's "fname" finds): the Target, another owner,
     * the first Turret, the Leader, nobody, nothing. */
    for (uint32_t index = 0; index < options.objects; index++) {
      const std::string other = "Obj" + std::to_string((index + 1) % options.objects);
      const std::string names[] = {"Target", other, "Turret", "Leader", "Nobody", ""};
      objects[index].object.SetProperty(
          "fname", new EXP_StringValue(names[(index * 5 + 1) % 6], "fname"));
    }
    for (uint32_t index = 0; index < options.objects; index++) {
      if (index % 3 == 2) {
        continue;
      }
      objects.emplace_back(MT_Vector3(0.5f, 0.0f, 1.0f));
      BatchObject &turret = objects.back();
      turret.object.SetName("Turret");
      turret.node.SetParentRelation(new KX_NormalParentRelation());
      objects[index].node.AddChild(&turret.node);
      objects[index].object.NodeUpdateGS(0.0);
    }
    objects.emplace_back(MT_Vector3(-3.0f, 2.0f, 0.25f));
    objects.back().object.SetName("Leader");
  }
  if (options.parented_owners) {
    BatchObject &target = objects[options.objects];
    for (uint32_t index = 1; index < options.objects; index++) {
      if (index % 3 == 0) {
        continue;
      }
      BatchObject &parent = index % 3 == 1 ? objects[index - 1] : target;
      objects[index].node.SetParentRelation(new KX_NormalParentRelation());
      parent.node.AddChild(&objects[index].node);
      parent.object.NodeUpdateGS(0.0);
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
    for (uint32_t k = 0; k < (options.two_trees ? 2u : 1u); k++) {
      trees.push_back(std::make_unique<LN_RuntimeTree>(program, &objects[index].object, index, k));
      trees.back()->SetStepSpread(uint32_t(trees.size() - 1));
      /* each owner has this one tree, or two */
      trees.back()->GetBatchNote().sole_tree = !options.two_trees;
      tree_pointers.push_back(trees.back().get());
    }
  }
  /* Plan C1: the column runner of the run (columns on only for it). */
  struct ColumnModeScope {
    explicit ColumnModeScope(const bool on) : on(on)
    {
      if (on) {
        LN_SetColumnModeForTests(1);
      }
    }
    ~ColumnModeScope()
    {
      if (on) {
        LN_SetColumnModeForTests(-1);
      }
    }
    bool on;
  } column_mode(options.columns);
  LN_ColumnRunner column_runner;
  column_runner.SetMinRunLanes(1);

  struct MemoScope {
    MemoScope(const int mode, const int lean_mode, const int route_mode)
    {
      LN_RuntimeTree::SetMemoForTests(mode);
      if (lean_mode >= 0) {
        LN_SetLeanModeForTests(lean_mode);
      }
      restore_lean = lean_mode >= 0;
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
      if (restore_routes) {
        LN_SetRouteModeForTests(-1);
      }
      if (restore_lean_memo) {
        LN_RuntimeTree::SetLeanMemoForTests(-1);
      }
    }
    bool restore_lean = false;
    bool restore_routes = false;
    bool restore_lean_memo = false;
  } memo_scope(options.memo, options.lean, options.routes);
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
    context.steps_done = fixed_index;
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
    const bool columns = options.columns &&
                         column_runner.Plan(
                                             runnable.data(), runnable.size(), event, buffer, nullptr, &read);
    /* Every tree outside the runs takes the column pass, as the manager records it (whether or not
     * a run was planned); without `columns` every tree does. */
    if (columns) {
      result.column_lanes += column_runner.LaneCount();
    }
    else if (options.columns &&
             result.column_refusals.find(column_runner.LastRefusal()) == std::string::npos)
    {
      result.column_refusals += column_runner.LastRefusal() + "\n";
    }
    for (size_t index = 0; index < runnable.size(); index++) {
      /* As the manager: a lane's tree captures only its properties (a bridged one all of it). */
      if (columns && runnable[index]->IsColumnLane()) {
        continue;
      }
      if (runnable[index]->ShouldCaptureSnapshot()) {
        runnable[index]->CaptureSnapshot(&read, float(step), context.tick_index, false);
      }
    }
    if (columns) {
      column_runner.CaptureLanes(context.tick_index, nullptr);
    }
    for (size_t index = 0; index < runnable.size(); index++) {
      if (!(columns && column_runner.IsLane(index))) {
        runnable[index]->WarmQueryCache(context);
      }
    }

    buffer.Clear();
    buffer.BeginRecording();
    size_t at = 0;
    while (at < runnable.size()) {
      if (columns && column_runner.IsLane(at)) {
        at = column_runner.RunLanes(at, buffer, context, nullptr);
        continue;
      }
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
        /* As the manager: the column pass. */
        runnable[at]->ExecuteReady(buffer, context, options.profiled ? &counters : nullptr);
        LN_AddColumnPasses(1);
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
    /* Forces and torques (plan C2 P5: no physics here to show them), in the text only when some
     * were applied (the goldens' seeds apply none). */
    std::string forces;
    /* UPBGE_LN_DIFF_FORCES=0: without them (the text recorded before plan C2 P5, so the goldens
     * of then can show that the forces are their only change). */
    static const bool note_forces = [] {
      const char *value = std::getenv("UPBGE_LN_DIFF_FORCES");
      return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    const auto flush_noting_forces = [&]() {
      if (!note_forces) {
        buffer.Flush();
        LN_ApplyPendingRandomGameSeed();
        return;
      }
      LN_SetForceObserverForTests(
          [](void *user,
             const KX_GameObject *object,
             const MT_Vector3 &vector,
             const bool local,
             const bool torque) {
            *static_cast<std::string *>(user) +=
                (object != nullptr ? const_cast<KX_GameObject *>(object)->GetName() :
                                     std::string("-")) +
                (torque ? " torque " : " force ") + LN_DumpFloatBits(vector.x()) + "," +
                LN_DumpFloatBits(vector.y()) + "," + LN_DumpFloatBits(vector.z()) +
                (local ? " local\n" : "\n");
          },
          &forces);
      buffer.Flush();
      LN_SetForceObserverForTests(nullptr, nullptr);
      /* As the manager: Set Random Seed takes effect after the tick's flush. */
      LN_ApplyPendingRandomGameSeed();
    };
    if (options.world_only) {
      flush_noting_forces();
      if (columns) {
        column_runner.EndTick();
      }
      result.text += DumpWorld(world_objects);
      if (!forces.empty()) {
        result.text += "forces:\n" + forces;
      }
      time += step;
      return;
    }
    if (recorded) {
      result.text += "-- record\n" + buffer.DumpRecordedForTests(Order::Record);
      result.text += "-- flush\n" + buffer.DumpRecordedForTests(Order::Flush);
      result.text += "-- coalesced\n" + buffer.DumpRecordedForTests(Order::FlushCoalesced);
    }
    flush_noting_forces();
    if (columns) {
      column_runner.EndTick();
    }
    if (recorded) {
      result.text += "-- after flush\n" + DumpWorld(world_objects);
    }
    if (!forces.empty()) {
      result.text += "forces:\n" + forces;
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
  if (options.states && !options.world_only) {
    for (size_t index = 0; index < tree_pointers.size(); index++) {
      result.text += "-- state " + std::to_string(index) + "\n" +
                     tree_pointers[index]->DumpStateForTests(tick_index - 1).meaning;
    }
  }
  tree_pointers.clear();
  trees.clear();
  LN_SetSceneAccessForTests(nullptr);
  std::string printed = testing::internal::GetCapturedStdout();
  const std::string errors = testing::internal::GetCapturedStderr();
  /* A NaN's sign is no logic (LN_DumpFloatBits): "-nan" printed as "nan". */
  for (size_t at = printed.find("-nan"); at != std::string::npos && !LN_DumpNaNAsIs();
       at = printed.find("-nan", at))
  {
    printed.erase(at, 1);
  }
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

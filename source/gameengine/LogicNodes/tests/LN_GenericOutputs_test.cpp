/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Every node's typed data output can feed a generic input (2026-11-13): Get Pose Bone Transform's
 * Location linked into Print's Message failed the Print ("Print requires a message input") and
 * disabled the whole tree, because the generic value builder handled that node's Rotation only.
 * Here every output of every node whose pin is Bool, Int, Float, String, Vector, Rotation or
 * Color is linked into a Print and compiled: none may leave the Print without its message.
 */

#include "testing/testing.h"

#include <string>
#include <vector>

#include "LN_SemRig.h"

using namespace ln_test::sem;
using blender::nodes::logic::NodeMetadata;
using blender::nodes::logic::PinMetadata;
using blender::nodes::logic::PinType;

namespace {

bool IsTypedValuePin(const PinType type)
{
  switch (type) {
    case PinType::Condition:
    case PinType::Bool:
    case PinType::Int:
    case PinType::Float:
    case PinType::Angle:
    case PinType::String:
    case PinType::Vector:
    case PinType::Rotation:
    case PinType::Color:
      return true;
    default:
      return false;
  }
}

}  // namespace

/** The settings to try a node in: the random tester's custom1 / custom2 choices for it (an
 * output that exists in one mode only, as Random Value's Integer), else every custom1 up to 31
 * (most nodes keep their mode there: Get Rigid Body Attribute's Damping outputs, Get Object
 * Attribute's Color...). */
std::vector<std::pair<int16_t, int16_t>> SettingsOf(const char *idname)
{
  std::vector<std::pair<int16_t, int16_t>> settings;
  for (const ln_test::diff::Family &family : ln_test::diff::FamilyTable()) {
    if (std::string(family.idname) != idname) {
      continue;
    }
    const std::vector<int16_t> first = family.custom1.empty() ? std::vector<int16_t>{0} :
                                                                family.custom1;
    const std::vector<int16_t> second = family.custom2.empty() ? std::vector<int16_t>{0} :
                                                                 family.custom2;
    for (const int16_t custom1 : first) {
      for (const int16_t custom2 : second) {
        settings.emplace_back(custom1, custom2);
      }
    }
    break;
  }
  for (int16_t custom = 0; custom < 32; custom++) {
    settings.emplace_back(custom, 0);
    /* (Gamepad Button keeps its button in custom2: Strength shows for the triggers only.) */
    settings.emplace_back(0, custom);
  }
  return settings;
}

/** Outputs the editor never shows: Typecast's Vector (its update shows it for a "To Vector" that
 * the To Type dropdown does not offer). */
bool NeverShown(const NodeMetadata &meta, const PinMetadata &pin)
{
  return std::string(meta.idname) == "LogicNativeTypecast" &&
         std::string(pin.identifier) == "Vector";
}

/** A node keeping its constant in its output socket (a Value node: no inputs). The test's trees
 * have no output socket values; in a game a String value prints (the bone attribute check,
 * ~/Desktop/CPP_LN_TESTS/bone_attributes/build.py, prints its BEGIN so). */
bool KeepsItsValueInItsOutput(const NodeMetadata &meta)
{
  return meta.inputs.size() == 0 && std::string(meta.category) == "Values";
}

enum class Fed { Yes, Lost, Blocked };

/** What the node's output, linked into a Print's Message, gives the Print: the message, none
 * (Lost: the output has no generic path), or Blocked: the tree has errors of its own (a node
 * missing a setting it needs, as Set Overlay Collection without a collection), so it tells
 * nothing. The node runs as in a real tree: a flow node is driven by the step, the Print after
 * it. */
Fed PrintMessage(const NodeMetadata &meta,
                 const PinMetadata &pin,
                 const int16_t custom1,
                 const int16_t custom2)
{
  TreeBuilder b;
  /* The editor's sockets (a ray's Mask is a Collision Layers socket, an asset pin its ID type). */
  b.SetRealSockets(true);
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &source = b.Add(meta.idname, custom1, custom2);
  GenNode &print = b.Add("LogicNativePrint");
  const PinMetadata *flow_in = nullptr;
  const PinMetadata *flow_out = nullptr;
  for (const PinMetadata &input : meta.inputs) {
    if (input.type == PinType::Execution && flow_in == nullptr && !input.is_variadic) {
      flow_in = &input;
    }
  }
  for (const PinMetadata &output : meta.outputs) {
    if (output.type == PinType::Execution && flow_out == nullptr && !output.is_variadic) {
      flow_out = &output;
    }
  }
  if (flow_in != nullptr) {
    b.Link(step, "Out", source, flow_in->identifier);
  }
  if (flow_in != nullptr && flow_out != nullptr) {
    b.Link(source, flow_out->identifier, print, "Flow");
  }
  else {
    b.Link(step, "Out", print, "Flow");
  }
  b.Link(source, pin.identifier, print, "Message");
  std::string report;
  b.Compile(&report);
  if (report.find("Print requires a message input") == std::string::npos) {
    return report.find("error") == std::string::npos ? Fed::Yes : Fed::Blocked;
  }
  /* Lost only when the Print's is the tree's only error. */
  const size_t first = report.find("error");
  return report.find("error", first + 1) == std::string::npos ? Fed::Lost : Fed::Blocked;
}

TEST(LN_GenericOutputs, EveryTypedOutputFeedsAGenericInput)
{
  std::vector<std::string> failures;
  std::vector<std::string> blocked;
  int checked = 0;
  for (const NodeMetadata &meta : blender::nodes::logic::logic_node_metadata()) {
    if (KeepsItsValueInItsOutput(meta)) {
      continue;
    }
    for (const PinMetadata &pin : meta.outputs) {
      if (!IsTypedValuePin(pin.type) || pin.is_variadic || NeverShown(meta, pin)) {
        continue;
      }
      checked++;
      bool fed = false;
      bool lost = false;
      for (const auto &[custom1, custom2] : SettingsOf(meta.idname)) {
        const Fed result = PrintMessage(meta, pin, custom1, custom2);
        fed = fed || result == Fed::Yes;
        lost = lost || result == Fed::Lost;
        if (fed) {
          break;
        }
      }
      if (!fed && lost) {
        failures.push_back(std::string(meta.idname) + "." + pin.identifier);
      }
      else if (!fed) {
        blocked.push_back(std::string(meta.idname) + "." + pin.identifier);
      }
    }
  }
  EXPECT_GT(checked, 300);
  /* Outputs no setting could compile cleanly (their node needs a data-block the rig has not):
   * few, and listed. */
  std::string blocked_text;
  for (const std::string &name : blocked) {
    blocked_text += name + "\n";
  }
  EXPECT_LE(blocked.size(), 8u) << blocked_text;
  std::printf("%zu typed outputs checked; %zu blocked by their node's own needs:\n%s",
              size_t(checked),
              blocked.size(),
              blocked_text.c_str());
  std::string text;
  for (const std::string &failure : failures) {
    text += failure + "\n";
  }
  EXPECT_TRUE(failures.empty()) << failures.size() << " outputs cannot feed a generic input:\n"
                                << text;
}

/** The user's tree: On Logic + Physics Step -> Apply Force To Target -> Print, Get Pose Bone
 * Transform's Location into both the Target Position and the Print's Message: it compiles. */
TEST(LN_GenericOutputs, ABonePoseLocationPrintsWithoutDisablingTheTree)
{
  TreeBuilder b;
  b.SetRealSockets(true);
  GenNode &step = b.Add("LogicNativeOnFixedUpdate");
  GenNode &bone = b.Add("LogicNativeGetBonePoseTransform");
  GenNode &force = b.Add("LogicNativeApplyForceToTarget");
  GenNode &print = b.Add("LogicNativePrint");
  b.Link(step, "Out", force, "Flow");
  b.Link(bone, "Location", force, "Target Position");
  b.Link(force, "Done", print, "Flow");
  b.Link(bone, "Location", print, "Message");
  std::string report;
  EXPECT_NE(b.Compile(&report), nullptr) << report;
}

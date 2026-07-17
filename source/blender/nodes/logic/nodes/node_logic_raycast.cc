/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_raycast_cc {
namespace decl = blender::nodes::logic::decl;

enum InputMode { EndPoint = 0, DirectionDistance };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Optional trigger; unconnected raycasts evaluate when an output is read");
  b.add_input<decl::Object>("Caster"_ustr)
      .description(
          "Object used for local-space conversion and self filtering; local mode uses the owner "
          "when empty");
  b.add_input<decl::Object>("Ignore Object"_ustr)
      .description("Additional object to exclude from the raycast");
  b.add_input<decl::Vector>("Origin"_ustr)
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .description("Ray origin in world or local space");
  b.add_input<decl::Vector>("Destination"_ustr)
      .default_value(float3(0.0f, 0.0f, 1.0f))
      .description("Ray destination in world or local space");
  b.add_input<decl::Vector>("Direction"_ustr)
      .default_value(float3(0.0f, 0.0f, 1.0f))
      .description("Ray direction; normalized by the game engine");
  b.add_input<decl::Float>("Max Distance"_ustr)
      .default_value(100.0f)
      .min(0.001f)
      .description("Maximum ray travel distance in world units");
  b.add_input<decl::Bool>("Local"_ustr)
      .default_value(false)
      .description("Interpret positions and direction in caster local space; distance remains in world units");
  b.add_input<decl::String>("Property"_ustr)
      .description("Only report hits on objects with this game property; empty accepts any object");
  b.add_input<decl::Bool>("X-Ray"_ustr)
      .default_value(false)
      .description("Skip objects that do not match the property filter instead of blocking the ray");
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers filter");
  b.add_input<decl::Bool>("Include Sensors"_ustr)
      .description("Allow Jolt sensor bodies to be returned");
  b.add_input<decl::Bool>("Hit Backfaces"_ustr)
      .description("Report back sides of one-sided triangle surfaces");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .default_value(false)
      .description("Draw the raycast as a transient game-engine debug line");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues when the raycast node is triggered");
  b.add_output<decl::Condition>("Has Result"_ustr).description("True when the ray hits an object");
  b.add_output<decl::Condition>("Blocked"_ustr)
      .description("True when a nonmatching object blocks a non-X-Ray property query");
  b.add_output<decl::Object>("Hit Object"_ustr, "Picked Object"_ustr)
      .description("Object hit by the ray");
  b.add_output<decl::Vector>("Point"_ustr)
      .description("World-space hit point, filtered blocker point, or ray end on a clear miss");
  b.add_output<decl::Vector>("Normal"_ustr).description("World-space hit normal");
  b.add_output<decl::Vector>("Direction"_ustr).description("Normalized ray direction");
  b.add_output<decl::Float>("Distance"_ustr)
      .description("Distance to the hit or filtered blocker, or maximum distance on a clear miss");
  b.add_output<decl::Float>("Fraction"_ustr)
      .description("Distance divided by maximum distance; 1 for a clear miss");
  b.add_output<decl::Int>("Face Index"_ustr).description("Hit mesh polygon index, or -1");
  b.add_output<decl::Condition>("Has UV"_ustr).description("True when UV output is valid");
  b.add_output<decl::Vector>("UV"_ustr).description("Hit UV as X/Y, with Z set to 0");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "input_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = DirectionDistance;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  if (node->custom1 < EndPoint || node->custom1 > DirectionDistance) {
    node->custom1 = EndPoint;
  }
  auto available = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  available("Destination"_ustr, node->custom1 == EndPoint);
  available("Direction"_ustr, node->custom1 == DirectionDistance);
  available("Max Distance"_ustr, node->custom1 == DirectionDistance);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRaycast"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_raycast_cc

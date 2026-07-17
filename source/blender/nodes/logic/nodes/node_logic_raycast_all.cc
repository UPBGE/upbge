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

namespace blender::nodes::node_logic_raycast_all_cc {
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
  b.add_input<decl::Int>("Max Results"_ustr)
      .default_value(32)
      .min(1)
      .max(256)
      .description("Maximum unique bodies returned; bounds query memory and retained hits");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .default_value(false)
      .description("Draw the raycast as a transient game-engine debug line");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues when the raycast node is triggered");
  b.add_output<decl::Condition>("Has Result"_ustr).description("True when the ray hits any object");
  b.add_output<decl::Condition>("Blocked"_ustr)
      .description("True when a nonmatching object stops a non-X-Ray property query");
  b.add_output<decl::Int>("Hit Count"_ustr).description("Number of hits returned");
  b.add_output<decl::Vector>("Direction"_ustr).description("Normalized world-space ray direction");
  b.add_output<decl::Vector>("End Point"_ustr).description("Furthest world-space point tested by the ray");
  b.add_output<decl::List>("Hit Objects"_ustr).description("Objects hit by the ray, nearest first");
  b.add_output<decl::List>("Points"_ustr).description("World-space hit points, nearest first");
  b.add_output<decl::List>("Normals"_ustr).description("World-space hit normals, nearest first");
  b.add_output<decl::List>("Distances"_ustr)
      .description("Distances from ray origin to each hit point, nearest first");
  b.add_output<decl::List>("Fractions"_ustr)
      .description("Hit distances divided by maximum distance, nearest first");
  b.add_output<decl::List>("Face Indices"_ustr).description("Hit mesh polygon indices, or -1");
  b.add_output<decl::List>("Has UVs"_ustr).description("Validity flag for each UV entry");
  b.add_output<decl::List>("UVs"_ustr).description("Hit UV values as X/Y vectors, Z set to 0");
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

  logic_node_type_base(&ntype, "LogicNativeRaycastAll"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_raycast_all_cc

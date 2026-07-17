/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_shape_cast_cc {
namespace decl = blender::nodes::logic::decl;

enum ShapeType { Sphere = 0, Box, Capsule };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Optional trigger; unconnected casts evaluate when an output is read");
  b.add_input<decl::Object>("Caster"_ustr)
      .description("Object used for local-space conversion and self filtering; local mode uses the owner when empty");
  b.add_input<decl::Object>("Ignore Object"_ustr)
      .description("Additional object to exclude from the cast");
  b.add_input<decl::Vector>("Origin"_ustr).description("Shape center at the start of the cast");
  b.add_input<decl::Vector>("Destination"_ustr)
      .default_value(float3(0.0f, 0.0f, 1.0f))
      .description("Shape center at the end of the cast; may equal Origin for an overlap query");
  b.add_input<decl::Rotation>("Rotation"_ustr)
      .description("Fixed shape orientation for the complete cast");
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(0.5f)
      .min(0.001f)
      .description("Sphere or capsule radius in world units");
  b.add_input<decl::Vector>("Half Extents"_ustr)
      .default_value(float3(0.5f, 0.5f, 0.5f))
      .min(0.001f)
      .description("Box half-size in world units");
  b.add_input<decl::Float>("Height"_ustr)
      .default_value(2.0f)
      .min(0.002f)
      .description("Capsule total end-to-end height; values below twice the radius produce a sphere");
  b.add_input<decl::Float>("Extra Radius"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("Additional Jolt convex sweep radius added in every direction");
  b.add_input<decl::Bool>("Local"_ustr)
      .description("Interpret origin, destination, and rotation in caster local space; dimensions stay in world units");
  b.add_input<decl::String>("Property"_ustr)
      .description("Only report objects with this game property; empty accepts any object");
  b.add_input<decl::Bool>("X-Ray"_ustr)
      .description("With a non-empty Property, pass through nonmatching objects instead of reporting Blocked");
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers filter; zero accepts all layers for compatibility");
  b.add_input<decl::Bool>("Include Sensors"_ustr)
      .description("Allow Jolt sensor bodies to be returned");
  b.add_input<decl::Bool>("Hit Backfaces"_ustr)
      .description("Report back sides of one-sided triangle surfaces");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .description("Draw the swept shape, center path, contact point, and hit normal");

  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Condition>("Has Result"_ustr).description("True when the shape hits an object");
  b.add_output<decl::Condition>("Blocked"_ustr)
      .description("True when a nonmatching object stops a non-X-Ray property query");
  b.add_output<decl::Object>("Hit Object"_ustr).description("Object hit by the shape");
  b.add_output<decl::Vector>("Point"_ustr).description("World-space contact point on the hit object");
  b.add_output<decl::Vector>("Normal"_ustr).description("World-space surface normal pointing away from the hit object");
  b.add_output<decl::Vector>("Cast Position"_ustr)
      .description("World-space shape center at the hit, or Destination when nothing is hit");
  b.add_output<decl::Vector>("Direction"_ustr).description("Normalized cast direction, or zero for an overlap query");
  b.add_output<decl::Float>("Distance"_ustr)
      .description("Distance travelled before the hit, or the complete cast distance on a miss");
  b.add_output<decl::Float>("Fraction"_ustr)
      .description("Hit position along the cast from 0 to 1; returns 1 on a miss");
  b.add_output<decl::Condition>("Started Overlapping"_ustr)
      .description("True when the shape starts penetrating the hit object");
  b.add_output<decl::Float>("Penetration Depth"_ustr)
      .description("Initial penetration depth, or zero for a swept hit");
  b.add_output<decl::Int>("Face Index"_ustr)
      .default_value(-1)
      .description("Evaluated mesh polygon index, or -1 when unavailable");
  b.add_output<decl::Condition>("Has UV"_ustr)
      .description("True when UV contains valid coordinates from the active UV map");
  b.add_output<decl::Vector>("UV"_ustr)
      .description("Interpolated active UV-map coordinate; Z is always zero");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "shape_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Sphere;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  if (node->custom1 < Sphere || node->custom1 > Capsule) {
    node->custom1 = Sphere;
  }
  auto available = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  available("Rotation"_ustr, node->custom1 != Sphere);
  available("Radius"_ustr, node->custom1 != Box);
  available("Half Extents"_ustr, node->custom1 == Box);
  available("Height"_ustr, node->custom1 == Capsule);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeShapeCast"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_shape_cast_cc

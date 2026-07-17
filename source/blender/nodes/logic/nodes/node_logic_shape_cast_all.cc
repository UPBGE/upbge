/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_shape_cast_all_cc {
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
  b.add_input<decl::Float>("Radius"_ustr).default_value(0.5f).min(0.001f);
  b.add_input<decl::Vector>("Half Extents"_ustr)
      .default_value(float3(0.5f, 0.5f, 0.5f))
      .min(0.001f)
      .description("Box half-size in world units");
  b.add_input<decl::Float>("Height"_ustr)
      .default_value(2.0f)
      .min(0.002f)
      .description("Capsule total end-to-end height; values below twice the radius produce a sphere");
  b.add_input<decl::Float>("Extra Radius"_ustr)
      .min(0.0f)
      .description("Additional Jolt convex sweep radius added in every direction");
  b.add_input<decl::Bool>("Local"_ustr)
      .description("Interpret origin, destination, and rotation in caster local space; dimensions stay in world units");
  b.add_input<decl::String>("Property"_ustr);
  b.add_input<decl::Bool>("X-Ray"_ustr)
      .description("With a non-empty Property, pass through nonmatching objects instead of reporting Blocked");
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers filter; zero accepts all layers for compatibility");
  b.add_input<decl::Bool>("Include Sensors"_ustr);
  b.add_input<decl::Bool>("Hit Backfaces"_ustr)
      .description("Report back sides of one-sided triangle surfaces");
  b.add_input<decl::Int>("Max Results"_ustr)
      .default_value(32)
      .min(1)
      .max(256)
      .description("Maximum unique bodies returned; also bounds query memory and narrow-phase work");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .description("Draw the swept shape, center path, and every returned contact");

  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Condition>("Has Result"_ustr).description("True when at least one object is returned");
  b.add_output<decl::Condition>("Blocked"_ustr)
      .description("True when a nonmatching object stops a non-X-Ray property query");
  b.add_output<decl::Int>("Hit Count"_ustr).description("Number of unique bodies returned");
  b.add_output<decl::List>("Hit Objects"_ustr).description("Hit objects, nearest first");
  b.add_output<decl::List>("Points"_ustr).description("Contact points on hit objects, nearest first");
  b.add_output<decl::List>("Normals"_ustr).description("World-space hit normals, nearest first");
  b.add_output<decl::List>("Cast Positions"_ustr).description("Shape center positions at each hit");
  b.add_output<decl::List>("Distances"_ustr).description("Travel distances, nearest first");
  b.add_output<decl::List>("Fractions"_ustr).description("Hit fractions from 0 to 1, nearest first");
  b.add_output<decl::List>("Started Overlapping"_ustr).description("Initial-overlap flag for each hit");
  b.add_output<decl::List>("Penetration Depths"_ustr).description("Initial penetration depth for each hit");
  b.add_output<decl::List>("Face Indices"_ustr)
      .description("Evaluated mesh polygon index per hit; unavailable entries are -1");
  b.add_output<decl::List>("Has UVs"_ustr)
      .description("Whether each UV entry is valid for the corresponding hit");
  b.add_output<decl::List>("UVs"_ustr)
      .description("Interpolated active UV-map coordinates aligned with the hit lists");
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
  logic_node_type_base(&ntype, "LogicNativeShapeCastAll"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_shape_cast_all_cc

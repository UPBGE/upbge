/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Sweep Object (2026-11-11): "will it fit there?" with the object's own collision shape (Jolt
 * casts the body's shape; query LN_QueryExpressionKind::SweepObject). Moves nothing.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_sweep_object_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1. */
enum RotationMode { CurrentRotation = 0, CustomRotation };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues when the sweep was made (Flow linked: each time Flow runs)");
  b.add_output<decl::Condition>("Has Result"_ustr)
      .description("True when the object would hit something on the way");
  b.add_output<decl::Condition>("Blocked"_ustr)
      .description("True when an object without the Property is in the way (X-Ray off)");
  b.add_output<decl::Object>("Hit Object"_ustr).description("What the object would hit first");
  b.add_output<decl::Object>("Hit Part"_ustr)
      .description("The part of a compound object that would be hit (its child with Compound); "
                   "the Hit Object itself for any other object");
  b.add_output<decl::Vector>("Hit Point"_ustr)
      .description("Where the object would touch it; zero without a hit");
  b.add_output<decl::Vector>("Hit Normal"_ustr)
      .description("The touched surface's normal; zero without a hit");
  b.add_output<decl::String>("Hit Material"_ustr)
      .description("Name of the material on the touched face; empty without a hit or a material");
  b.add_output<decl::Vector>("Safe Position"_ustr)
      .description("How far the object can go without touching anything (1 mm short of the "
                   "hit): Destination when the way is clear");
  b.add_output<decl::Float>("Safe Fraction"_ustr)
      .description("Safe Position as a part of the way, from 0 (cannot move at all) to 1 (the "
                   "whole way)");
  b.add_output<decl::Float>("Distance"_ustr)
      .description("How far the object can go, in world units");
  b.add_output<decl::Condition>("Started Overlapping"_ustr)
      .description("True when the object already touches something where it is now");
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Optional trigger; unconnected, the sweep is made whenever an output is read");
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object whose own collision shape is moved (it needs physics; a triangle "
                   "mesh shape cannot be moved); empty: this tree's object. Nothing moves");
  b.add_input<decl::Vector>("Destination"_ustr)
      .description("Where the object's origin would go: a world position, or with Local a point "
                   "in its own space (0, 2, 0: two units ahead)");
  b.add_input<decl::Bool>("Local"_ustr)
      .description("Destination (and Rotation) in the object's own space");
  b.add_input<decl::Rotation>("Rotation"_ustr)
      .description("How the object is turned for the whole sweep (Local: added to its own turn)");
  b.add_input<decl::List>("Ignore Objects"_ustr)
      .description("Objects the sweep passes through; the object's own children with physics "
                   "are passed anyway");
  b.add_input<decl::String>("Property"_ustr)
      .description(std::string("Only report objects with this game property; empty accepts any "
                               "object. ") +
                   logic::game_property_filter_note)
      .custom_draw(logic::game_property_filter_field());
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description("Collision layers filter");
  logic::add_ray_filters_panel(b);
  logic::add_visualize_panel(b);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "rotation_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = CurrentRotation;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  if (node->custom1 != CurrentRotation && node->custom1 != CustomRotation) {
    node->custom1 = CurrentRotation;
  }
  if (bNodeSocket *socket = node->input_by_identifier("Rotation"_ustr)) {
    bke::node_set_socket_availability(*tree, *socket, node->custom1 == CustomRotation);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeSweepObject"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_sweep_object_cc

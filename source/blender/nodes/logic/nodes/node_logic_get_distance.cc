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

namespace blender::nodes::node_logic_get_distance_cc {
namespace decl = blender::nodes::logic::decl;

enum EndpointType { Object = 0, Position };

static void node_update(bNodeTree *tree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object A"_ustr)
      .description("First object origin; uses the logic-tree owner when empty");
  b.add_input<decl::Vector>("Position A"_ustr)
      .description("First world-space position");
  b.add_input<decl::Object>("Object B"_ustr).description("Second object origin");
  b.add_input<decl::Vector>("Position B"_ustr)
      .description("Second world-space position");
  b.add_output<decl::Float>("Distance"_ustr)
      .description("Straight-line distance between A and B");
  b.add_output<decl::Float>("Squared Distance"_ustr)
      .description("Distance without the square root, useful for fast range comparisons");
  b.add_output<decl::Condition>("Valid"_ustr)
      .description("True when both endpoints resolved to finite positions");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "endpoint_a_type", UI_ITEM_NONE, "A", ICON_NONE);
  layout.prop(ptr, "endpoint_b_type", UI_ITEM_NONE, "B", ICON_NONE);
}

static void node_init(bNodeTree *tree, bNode *node)
{
  node->custom1 = Object;
  node->custom2 = Object;
  node_update(tree, node);
}

static void node_update(bNodeTree *tree, bNode *node)
{
  node->custom1 = node->custom1 == Position ? Position : Object;
  node->custom2 = node->custom2 == Position ? Position : Object;
  auto available = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  available("Object A"_ustr, node->custom1 == Object);
  available("Position A"_ustr, node->custom1 == Position);
  available("Object B"_ustr, node->custom2 == Object);
  available("Position B"_ustr, node->custom2 == Position);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetDistance"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_distance_cc

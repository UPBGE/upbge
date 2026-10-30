/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include <algorithm>

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_get_average_center_position_cc {
namespace decl = blender::nodes::logic::decl;

enum SourceType { VectorList = 0, ObjectList, Collection };

static void node_update(bNodeTree *tree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::List>("Vectors"_ustr)
      .description("World-space position vectors to average; invalid entries are ignored");
  b.add_input<decl::List>("Objects"_ustr)
      .description("Objects whose finite world-space origins will be averaged");
  b.add_input<decl::Collection>("Collection"_ustr)
      .description("Collection whose direct active game objects will be averaged");
  b.add_output<decl::Vector>("Center"_ustr)
      .description("Average world-space position, or zero when no valid position exists");
  b.add_output<decl::Int>("Count"_ustr)
      .description("Number of valid positions included in the average");
  b.add_output<decl::Condition>("Valid"_ustr)
      .description("True when at least one valid position was included");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "source_type", UI_ITEM_NONE, "Source", ICON_NONE);
}

static void node_init(bNodeTree *tree, bNode *node)
{
  node->custom1 = VectorList;
  node_update(tree, node);
}

static void node_update(bNodeTree *tree, bNode *node)
{
  node->custom1 = std::clamp<int>(node->custom1, VectorList, Collection);
  auto available = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  available("Vectors"_ustr, node->custom1 == VectorList);
  available("Objects"_ustr, node->custom1 == ObjectList);
  available("Collection"_ustr, node->custom1 == Collection);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetAverageCenterPosition"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_average_center_position_cc

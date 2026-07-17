/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include <algorithm>

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_typecast_cc {
namespace decl = blender::nodes::logic::decl;

static void node_update(bNodeTree *ntree, bNode *node)
{
  static const char *outputs[] = {"Integer", "Boolean", "String", "Float"};
  const int active_output = std::clamp<int>(node->custom1, 0, 3);
  for (int index = 0; index < 4; index++) {
    if (bNodeSocket *socket = node->output_by_identifier(UString(outputs[index]))) {
      bke::node_set_socket_availability(*ntree, *socket, index == active_output);
    }
  }
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Generic>(""_ustr, "Value"_ustr);
  b.add_output<decl::Int>("Integer"_ustr);
  b.add_output<decl::Bool>("Boolean"_ustr);
  b.add_output<decl::String>("String"_ustr);
  b.add_output<decl::Float>("Float"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "to_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeTypecast"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_typecast_cc

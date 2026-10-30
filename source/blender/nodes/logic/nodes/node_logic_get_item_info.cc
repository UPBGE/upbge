/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_item_info_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Collection>("Library"_ustr)
      .description("Collection with one object per item, named like the item");
  b.add_input<decl::String>("Item"_ustr).description("Item name, or an inventory slot");
  b.add_output<decl::Condition>("Found"_ustr);
  b.add_output<decl::Object>("Object"_ustr).description("The item's object: its mesh is the icon");
  b.add_output<decl::String>("Name"_ustr).description("Its \"name\" property, else the item name");
  b.add_output<decl::Int>("Max Stack"_ustr).description("Its \"max_stack\" property, else 0");
  b.add_output<decl::String>("Category"_ustr).description("Its \"category\" property");
  b.add_output<decl::String>("Description"_ustr)
      .description("Its \"description\" property, such as a tooltip's text");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetItemInfo"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_item_info_cc

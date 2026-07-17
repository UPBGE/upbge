/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_formatted_string_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Format String"_ustr).default_value("A is {} and B is {}");
  b.add_input<decl::String>("A"_ustr).default_value("Hello");
  b.add_input<decl::String>("B"_ustr).default_value("World");
  b.add_input<decl::String>("C"_ustr).default_value("");
  b.add_input<decl::String>("D"_ustr).default_value("");
  b.add_output<decl::String>("String"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeFormattedString"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_formatted_string_cc
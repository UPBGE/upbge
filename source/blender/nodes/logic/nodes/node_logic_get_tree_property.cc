/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_tree_property_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description("The tree property to read: each object running this tree has its own. Pick "
                   "one this tree's Set or Toggle Tree Property nodes write, or type any name. "
                   "Red when none of them writes it: it then always gives the default")
      .custom_draw(logic::tree_property_field(true));
  b.add_output<decl::Generic>("Value"_ustr)
      .description("The value last set, of any type; before it is set, the default of the input "
                   "it feeds");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype,
                       "LogicNativeGetTreeProperty"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_tree_property_cc
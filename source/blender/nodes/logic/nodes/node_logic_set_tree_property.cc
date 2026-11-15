/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_tree_property_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Writes the value at once: the nodes after it in this step read the new one");
  b.add_input<decl::String>("Property"_ustr)
      .default_value("property")
      .description("The tree property to write: a value only this tree reads, one for each "
                   "object running it, kept until the game ends (Save Game does not keep it). "
                   "Pick a name this tree uses, or type a new one")
      .custom_draw(logic::tree_property_field(false));
  b.add_input<decl::Generic>("Value"_ustr)
      .description("Any value, such as a number, text, vector, object or list");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype,
                       "LogicNativeSetTreeProperty"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_tree_property_cc
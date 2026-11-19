/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Get Children (2026-11-17): every child of an object as a list, and how many (a Loop From List
 * over a turret's guns, a car's wheels, the parts of a menu), All Descendants their children too.
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_children_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Parent"_ustr)
      .description("The object whose children are listed (empty: the object running the tree)");
  b.add_input<decl::Bool>("All Descendants"_ustr)
      .default_value(false)
      .description("Also the children's children, at any depth (each child followed by its own "
                   "children)");
  b.add_output<decl::List>("Children"_ustr)
      .description("The children, in the order Get Child's Index counts them");
  b.add_output<decl::Int>("Count"_ustr).description("How many objects the list holds");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetChildren"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_children_cc

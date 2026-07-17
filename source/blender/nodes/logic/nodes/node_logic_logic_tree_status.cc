/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_logic_tree_status_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description(
      "Object that owns the logic tree; defaults to the owner when unset");
  b.add_input<decl::String>("Tree Name"_ustr).default_value("").description(
      "Compiled logic tree ID name (matches the LogicNodeTree data-block name)");
  b.add_output<decl::Bool>("Running"_ustr).description(
      "True when the tree is installed, enabled, and ticking on this object");
  b.add_output<decl::Bool>("Stopped"_ustr).description(
      "Negation of Running for the same object and tree name");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeLogicTreeStatus"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_logic_tree_status_cc

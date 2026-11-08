/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Get Child (LogicNativeGetChildObject, NOD_logic_merged.hh), each
 * the node it was before 2026-11-08: GetChild, GetChildByName. They are not node types of their
 * own: logic_option_register keeps them for the merged node, which forwards its declaration,
 * buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_child_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Parent"_ustr, "Parent Object"_ustr)
      .description("Object to read child objects from");
  b.add_input<decl::Int>("Index"_ustr)
      .default_value(0)
      .min(0)
      .description("Zero-based child index");
  b.add_output<decl::Object>("Child"_ustr, "Child Object"_ustr)
      .description("Resolved child object");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetChild"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_child_cc

namespace blender::nodes::node_logic_get_child_by_name_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Parent"_ustr);
  b.add_input<decl::Object>("Child"_ustr);
  b.add_output<decl::Object>("Child"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeGetChildByName"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_child_by_name_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_ui_focus_cc {
namespace decl = blender::nodes::logic::decl;
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Registered focusable object; empty clears focus. Outside an open menu this "
                   "also lets keyboard and gamepad control the UI");
  b.add_input<decl::Int>("User"_ustr).default_value(0).min(0).max(7);
  b.add_input<decl::String>("Input Context"_ustr);
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Runs after focus was changed or cleared");
  b.add_output<decl::Execution>("Failed"_ustr)
      .description("Runs if the target is not a registered focusable in this context");
  b.add_output<decl::Int>("Result"_ustr);
}
static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeSetUIFocus"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_set_ui_focus_cc

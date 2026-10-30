/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_ui_focus_cc {
namespace decl = blender::nodes::logic::decl;
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("User"_ustr).default_value(0).min(0).max(7);
  b.add_input<decl::String>("Input Context"_ustr);
  b.add_output<decl::Object>("Object"_ustr)
      .description("Currently focused object for this user and input context");
}
static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeGetUIFocus"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_get_ui_focus_cc

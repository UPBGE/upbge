/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_ui_interactable_cc {
namespace decl = blender::nodes::logic::decl;
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description("UI Element to change; empty uses the owner");
  b.add_input<decl::Bool>("Interactable"_ustr)
      .default_value(true)
      .description("Off keeps the object visible but ignores pointer, focus and drag");
  b.add_output<decl::Execution>("Done"_ustr);
}
static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeSetUIInteractable"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_set_ui_interactable_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_rotate_toward_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::Vector>("Target"_ustr).default_value({1.0f, 0.0f, 0.0f});
  b.add_input<decl::Float>("Factor"_ustr).default_value(1.0f).min(0.0f).max(1.0f);
  b.add_input<decl::Int>("Rot Axis"_ustr).default_value(2).min(0).max(2);
  b.add_input<decl::Int>("Front"_ustr, "Front Axis"_ustr).default_value(1).min(0).max(5);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRotateToward"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_rotate_toward_cc

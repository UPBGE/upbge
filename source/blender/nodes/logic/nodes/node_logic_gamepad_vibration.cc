/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_gamepad_vibration_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Int>("Index"_ustr).default_value(0).min(0);
  b.add_input<decl::Float>("Left"_ustr).default_value(0.0f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>("Right"_ustr).default_value(0.0f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>("Time"_ustr).default_value(1.0f).min(0.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype,
                       "LogicNativeGamepadVibration"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_gamepad_vibration_cc

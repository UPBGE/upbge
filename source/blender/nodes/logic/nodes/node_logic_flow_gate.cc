/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_flow_gate_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Enter"_ustr)
      .is_default_link_socket()
      .description("Execution pulse to pass when the gate is open");
  b.add_input<decl::Execution>("Toggle"_ustr).description("Invert the gate state");
  b.add_input<decl::Execution>("Open"_ustr).description("Open the gate");
  b.add_input<decl::Execution>("Close"_ustr).description("Close the gate");
  b.add_input<decl::Bool>("Start Open"_ustr)
      .default_value(false)
      .description("Initial state sampled once when the gate is first evaluated");
  b.add_output<decl::Execution>("Exit"_ustr)
      .is_default_link_socket()
      .description("Pulse emitted for every Enter pulse accepted while open");
  b.add_output<decl::Bool>("Is Open"_ustr).description("Current persistent gate state");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeFlowGate"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_flow_gate_cc

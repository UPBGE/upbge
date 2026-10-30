/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_flip_flop_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .is_default_link_socket()
      .description("Incoming execution pulse");
  b.add_input<decl::Execution>("Reset"_ustr)
      .description("Make the next Flow pulse use A");
  b.add_output<decl::Execution>("A"_ustr)
      .is_default_link_socket()
      .description("Receives the first, third, and subsequent odd-numbered Flow pulses");
  b.add_output<decl::Execution>("B"_ustr)
      .description("Receives the second, fourth, and subsequent even-numbered Flow pulses");
  b.add_output<decl::Bool>("Is A"_ustr).description("Whether A was selected most recently");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeFlipFlop"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_flip_flop_cc

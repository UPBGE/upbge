/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_loop_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description(
      "Starts the loop; pulses received while a multi-tick loop is active are ignored");
  b.add_input<decl::Int>("Count"_ustr)
      .default_value(1)
      .min(0)
      .description("Number of iterations, sampled when the loop starts");
  b.add_input<decl::Int>("Ticks to Complete"_ustr)
      .default_value(1)
      .min(1)
      .description(
          "Logic ticks over which to distribute the iterations; 1 requests synchronous "
          "completion, subject to the runtime runaway-loop safety budget");
  b.add_output<decl::Execution>("Loop"_ustr).description("Pulses once for each iteration");
  b.add_output<decl::Execution>("Completed"_ustr).description(
      "Pulses once after all iterations, zero iterations, or Break Loop");
  b.add_output<decl::Int>("Index"_ustr).description("Zero-based index of the current iteration");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeLoop"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_loop_cc

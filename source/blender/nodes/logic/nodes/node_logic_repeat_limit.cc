/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_repeat_limit_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .is_default_link_socket()
      .description("Execution pulse to admit while capacity remains");
  b.add_input<decl::Execution>("Reset"_ustr).description("Clear the accepted pulse count");
  b.add_input<decl::Int>("Limit"_ustr)
      .default_value(1)
      .min(0)
      .description("Maximum number of accepted Flow pulses after the last reset");
  b.add_output<decl::Execution>("Accepted"_ustr)
      .is_default_link_socket()
      .description("Runs for each Flow pulse admitted before the limit");
  b.add_output<decl::Execution>("Blocked"_ustr)
      .description("Runs for each Flow pulse rejected at or above the limit");
  b.add_output<decl::Int>("Remaining"_ustr)
      .description("Number of additional Flow pulses currently allowed");
  b.add_output<decl::Bool>("Is Exhausted"_ustr)
      .description("Whether the accepted count is at or above the current Limit");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRepeatLimit"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_repeat_limit_cc

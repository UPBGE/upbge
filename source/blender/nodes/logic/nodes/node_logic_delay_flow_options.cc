/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Delay (LogicNativeDelayFlow, NOD_logic_merged.hh), each the node
 * it was before 2026-11-08: Delay, Timer. They are not node types of their own:
 * logic_option_register keeps them for the merged node, which forwards its declaration, buttons,
 * init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_delay_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).is_default_link_socket();
  b.add_input<decl::Float>("Delay"_ustr).default_value(0.0f).min(0.0f);
  b.add_input<decl::Bool>("Ignore Timescale"_ustr).default_value(false);
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr).is_default_link_socket();
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeDelay"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_delay_cc

namespace blender::nodes::node_logic_timer_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Set Timer"_ustr);
  b.add_input<decl::Float>("Seconds"_ustr).default_value(0.0f).min(0.0f);
  b.add_input<decl::Bool>("Ignore Timescale"_ustr).default_value(false);
  b.add_output<decl::Execution>("When Elapsed"_ustr, "Out"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeTimer"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_timer_cc

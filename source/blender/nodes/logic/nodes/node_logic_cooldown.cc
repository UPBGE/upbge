/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_cooldown_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .is_default_link_socket()
      .description("Attempts to start the cooldown");
  b.add_input<decl::Execution>("Reset"_ustr)
      .description(
          "Ends the cooldown without Completed; processed before Flow in the same evaluation");
  b.add_input<decl::Float>("Duration"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description(
          "Sampled when Flow is accepted; non-positive or non-finite values bypass cooldown");
  b.add_input<decl::Bool>("Ignore Timescale"_ustr)
      .default_value(false)
      .description("Sampled when Flow is accepted; uses unscaled fixed time when enabled");

  b.add_output<decl::Execution>("Accepted"_ustr)
      .is_default_link_socket()
      .description("Emits when Flow arrives while the cooldown is ready");
  b.add_output<decl::Execution>("Blocked"_ustr)
      .description("Emits when Flow arrives while the cooldown is active");
  b.add_output<decl::Execution>("Completed"_ustr)
      .description(
          "Emits only when a positive-duration cooldown expires naturally, not on Reset");
  b.add_output<decl::Float>("Remaining"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("Time left; zero while ready or after Reset");
  b.add_output<decl::Float>("Progress"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .description("Normalized completion; zero when accepted and one while ready");
  b.add_output<decl::Bool>("Is Ready"_ustr)
      .default_value(true)
      .description("True initially, after Reset or expiry, and for bypass durations");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeCooldown"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_cooldown_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_boolean_edge_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Condition>("Condition"_ustr)
      .default_value(false)
      .description(
          "Sampled once per fixed tick; the initial previous value is false");
  b.add_output<decl::Execution>("Rising"_ustr)
      .is_default_link_socket()
      .description(
          "Emits on false-to-true; an initially true Condition emits on the first sample");
  b.add_output<decl::Execution>("Falling"_ustr)
      .description("Emits when Condition changes from true to false");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeBooleanEdge"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_boolean_edge_cc

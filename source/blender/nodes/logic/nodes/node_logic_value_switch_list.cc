/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_value_switch_list_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("if A"_ustr).default_value(false);
  b.add_input<decl::Generic>(""_ustr, "Value A"_ustr);
  b.add_input<decl::Bool>("elif B"_ustr).default_value(false);
  b.add_input<decl::Generic>(""_ustr, "Value B"_ustr);
  b.add_input<decl::Bool>("elif C"_ustr).default_value(false);
  b.add_input<decl::Generic>(""_ustr, "Value C"_ustr);
  b.add_input<decl::Bool>("elif D"_ustr).default_value(false);
  b.add_input<decl::Generic>(""_ustr, "Value D"_ustr);
  b.add_input<decl::Bool>("elif E"_ustr).default_value(false);
  b.add_input<decl::Generic>(""_ustr, "Value E"_ustr);
  b.add_input<decl::Bool>("elif F"_ustr).default_value(false);
  b.add_input<decl::Generic>(""_ustr, "Value F"_ustr);
  b.add_output<decl::Generic>("Result"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeValueSwitchList"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_value_switch_list_cc

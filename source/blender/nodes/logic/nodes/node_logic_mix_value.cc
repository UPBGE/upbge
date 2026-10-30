/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_mix_value_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Generic>("A"_ustr).description("First interpolated value");
  b.add_input<decl::Generic>("B"_ustr).description("Second interpolated value");
  b.add_input<decl::Float>("Factor"_ustr)
      .default_value(0.5f)
      .description("Unclamped blend factor; values outside 0..1 extrapolate");
  b.add_output<decl::Generic>("Result"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeMixValue"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_mix_value_cc

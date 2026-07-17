/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_evaluate_curve_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Curve"_ustr).description("Curve object to sample along its path");
  b.add_input<decl::Float>("Factor"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .description("Normalized position along the curve (0 = start, 1 = end)");
  b.add_output<decl::Vector>("Vector"_ustr, "Point"_ustr)
      .description("World-space point on the curve");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeEvaluateCurve"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_evaluate_curve_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_euler_cc {
namespace decl = blender::nodes::decl;
namespace logic_decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("X"_ustr)
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .description("Angle around the X axis in the XYZ Euler rotation");
  b.add_input<decl::Float>("Y"_ustr)
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .description("Angle around the Y axis in the XYZ Euler rotation");
  b.add_input<decl::Float>("Z"_ustr)
      .default_value(0.0f)
      .subtype(PROP_ANGLE)
      .description("Angle around the Z axis in the XYZ Euler rotation");
  b.add_output<logic_decl::Rotation>("Rotation"_ustr)
      .description("Rotation constructed from the XYZ Euler angles");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeEuler"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_euler_cc

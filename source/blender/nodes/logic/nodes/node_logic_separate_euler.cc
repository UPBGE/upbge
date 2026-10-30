/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_separate_euler_cc {
namespace decl = blender::nodes::decl;
namespace logic_decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<logic_decl::Rotation>("Rotation"_ustr)
      .description("Rotation to express as equivalent XYZ Euler angles");
  b.add_output<decl::Float>("X"_ustr)
      .subtype(PROP_ANGLE)
      .description("Equivalent angle around the X axis in XYZ Euler order");
  b.add_output<decl::Float>("Y"_ustr)
      .subtype(PROP_ANGLE)
      .description("Equivalent angle around the Y axis in XYZ Euler order");
  b.add_output<decl::Float>("Z"_ustr)
      .subtype(PROP_ANGLE)
      .description("Equivalent angle around the Z axis in XYZ Euler order");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSeparateEuler"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_separate_euler_cc

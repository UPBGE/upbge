/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_screen_to_world_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Camera"_ustr).description(
      "Projection camera; uses the active camera when unset");
  b.add_input<decl::Float>("Screen X"_ustr)
      .default_value(0.5f)
      .description("Normalized horizontal screen coordinate");
  b.add_input<decl::Float>("Screen Y"_ustr)
      .default_value(0.5f)
      .description("Normalized vertical screen coordinate");
  b.add_input<decl::Float>("Depth"_ustr)
      .default_value(1.0f)
      .description("Distance from the camera along the projected ray");
  b.add_output<decl::Vector>("World Position"_ustr).description(
      "Projected world-space position at the requested depth");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeScreenToWorld"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_screen_to_world_cc
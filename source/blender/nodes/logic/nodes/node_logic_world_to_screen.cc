/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_world_to_screen_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Point"_ustr)
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .description("World-space point to project");
  b.add_input<decl::Object>("Camera"_ustr).description(
      "Projection camera; uses the active camera when unset");
  b.add_output<decl::Vector>("On Screen"_ustr).description(
      "Normalized screen coordinates in X/Y with Z fixed to zero");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeWorldToScreen"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_world_to_screen_cc
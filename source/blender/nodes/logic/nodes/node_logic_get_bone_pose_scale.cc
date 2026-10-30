/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_bone_pose_scale_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_output<decl::Vector>("Scale"_ustr).description(
      "Native Blender pose-channel scale in the bone's local axes");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetBonePoseScale"_ustr);
  /* Wide enough for its socket names. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_bone_pose_scale_cc

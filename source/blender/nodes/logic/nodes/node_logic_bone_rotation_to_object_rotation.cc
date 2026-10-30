/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_bone_rotation_to_object_rotation_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Rotation>("Bone Rotation"_ustr)
      .description("Native Blender bone orientation, normally from Armature Space or World Space");
  b.add_output<decl::Rotation>("Object Rotation"_ustr)
      .description("Object orientation with +Z along bone +Y, +X along bone +X, and +Y opposite "
                   "bone +Z");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeBoneRotationToObjectRotation"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_bone_rotation_to_object_rotation_cc

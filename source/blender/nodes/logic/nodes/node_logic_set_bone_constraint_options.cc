/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Set Bone Constraint (LogicNativeSetBoneConstraint,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: SetBoneConstraintInfluence,
 * SetBoneConstraintTarget, SetBoneConstraintAttribute. They are not node types of their own:
 * logic_option_register keeps them for the merged node, which forwards its declaration, buttons,
 * init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_bone_constraint_influence_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Armature object; defaults to the owner when unset");
  b.add_input<decl::String>("Bone Name"_ustr).default_value("Bone");
  b.add_input<decl::String>("Constraint Name"_ustr).default_value("Constraint");
  b.add_input<decl::Float>("Influence"_ustr).default_value(1.0f).min(0.0f).max(1.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetBoneConstraintInfluence"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_bone_constraint_influence_cc

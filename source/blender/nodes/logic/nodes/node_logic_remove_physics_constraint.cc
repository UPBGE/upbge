/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_remove_physics_constraint_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("First"_ustr, "Object"_ustr).description(
      "First rigid body that owns the constraints; defaults to the owner");
  b.add_input<decl::Bool>("Remove All"_ustr).description(
      "Remove all rigid body constraints owned by the first object");
  b.add_input<decl::String>("Name"_ustr).default_value("constraint").description(
      "Name of the constraint to remove when Remove All is disabled");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(
      &ntype, "LogicNativeRemovePhysicsConstraint"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_remove_physics_constraint_cc

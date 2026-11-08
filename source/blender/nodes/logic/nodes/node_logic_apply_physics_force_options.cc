/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Apply Force (LogicNativeApplyPhysicsForce, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: ApplyForce, ApplyTorque, ApplyImpulse. They are not node
 * types of their own: logic_option_register keeps them for the merged node, which forwards its
 * declaration, buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_apply_force_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr).description(
      "Target object; defaults to the owner when unset");
  b.add_input<decl::Vector>("Force"_ustr).description("Force vector");
  b.add_input<decl::Bool>("Local"_ustr).description("Apply force in local space");
  b.add_output<decl::Execution>("Done"_ustr);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeApplyForce"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_apply_force_cc

namespace blender::nodes::node_logic_apply_torque_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr).description(
      "Target object; defaults to the owner when unset");
  b.add_input<decl::Vector>("Torque"_ustr).description("Torque vector");
  b.add_input<decl::Bool>("Local"_ustr).description("Apply torque in local space");
  b.add_output<decl::Execution>("Done"_ustr);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeApplyTorque"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_apply_torque_cc

namespace blender::nodes::node_logic_apply_impulse_cc {

namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr).description(
      "Target object; defaults to the owner when unset");
  b.add_input<decl::Vector>("Attach"_ustr).description("World-space attachment offset");
  b.add_input<decl::Vector>("Impulse"_ustr).description("World-space impulse vector");
  b.add_output<decl::Execution>("Done"_ustr);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeApplyImpulse"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_apply_impulse_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "DNA_node_types.h"
#include "DNA_rigidbody_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_add_physics_constraint_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("First"_ustr, "Object"_ustr).description(
      "First rigid body object to constrain; defaults to the owner when it is a rigid body");
  b.add_input<decl::Object>("Second"_ustr, "Target"_ustr).description(
      "Second rigid body object to constrain");
  b.add_input<decl::Object>("Constraint Object"_ustr).description(
      "Object whose world transform supplies the constraint frame; defaults to the owner when "
      "the owner is not one of the constrained rigid bodies");
  b.add_input<decl::String>("Name"_ustr).default_value("constraint");
  b.add_input<decl::Bool>("Use World Space"_ustr);
  b.add_input<decl::Vector>("Pivot"_ustr);
  b.add_input<decl::Vector>("Rotation"_ustr);
  b.add_input<decl::Bool>("Enabled"_ustr).default_value(true);
  b.add_input<decl::Bool>("Disable Collisions"_ustr).default_value(true);
  b.add_input<decl::Bool>("Breakable"_ustr);
  b.add_input<decl::Float>("Breaking Threshold"_ustr).default_value(10.0f).min(0.0f);
  b.add_input<decl::Bool>("Override Iterations"_ustr);
  b.add_input<decl::Int>("Velocity Solver Iterations"_ustr)
      .default_value(10)
      .min(1)
      .max(255)
      .description("Jolt velocity solver iterations requested by this constraint (1 to 255)");
  b.add_input<decl::Int>("Position Solver Iterations"_ustr)
      .default_value(2)
      .min(1)
      .max(255)
      .description("Jolt position solver iterations requested by this constraint (1 to 255)");
  b.add_input<decl::Bool>("Use Linear Limit X"_ustr);
  b.add_input<decl::Bool>("Use Linear Limit Y"_ustr);
  b.add_input<decl::Bool>("Use Linear Limit Z"_ustr);
  b.add_input<decl::Vector>("Linear Lower"_ustr).default_value({-1.0f, -1.0f, -1.0f});
  b.add_input<decl::Vector>("Linear Upper"_ustr).default_value({1.0f, 1.0f, 1.0f});
  b.add_input<decl::Bool>("Use Angular Limit X"_ustr);
  b.add_input<decl::Bool>("Use Angular Limit Y"_ustr);
  b.add_input<decl::Bool>("Use Angular Limit Z"_ustr);
  b.add_input<decl::Vector>("Angular Lower"_ustr).default_value(
      {-0.7853981633974483f, -0.7853981633974483f, -0.7853981633974483f});
  b.add_input<decl::Vector>("Angular Upper"_ustr).default_value(
      {0.7853981633974483f, 0.7853981633974483f, 0.7853981633974483f});
  b.add_input<decl::Bool>("Use Spring X"_ustr);
  b.add_input<decl::Bool>("Use Spring Y"_ustr);
  b.add_input<decl::Bool>("Use Spring Z"_ustr);
  b.add_input<decl::Vector>("Spring Stiffness"_ustr)
      .default_value({10.0f, 10.0f, 10.0f})
      .min(0.0f)
      .description("Jolt linear spring stiffness in N/m");
  b.add_input<decl::Vector>("Spring Damping"_ustr)
      .default_value({0.5f, 0.5f, 0.5f})
      .min(0.0f)
      .description("Jolt linear spring damping in N s/m");
  b.add_input<decl::Bool>("Use Angular Spring X"_ustr);
  b.add_input<decl::Bool>("Use Angular Spring Y"_ustr);
  b.add_input<decl::Bool>("Use Angular Spring Z"_ustr);
  b.add_input<decl::Vector>("Angular Spring Stiffness"_ustr)
      .default_value({10.0f, 10.0f, 10.0f})
      .min(0.0f)
      .description("Jolt angular spring stiffness in N m/rad");
  b.add_input<decl::Vector>("Angular Spring Damping"_ustr)
      .default_value({0.5f, 0.5f, 0.5f})
      .min(0.0f)
      .description("Jolt angular spring damping in N m s/rad");
  b.add_input<decl::Bool>("Use Linear Motor"_ustr);
  b.add_input<decl::Float>("Linear Motor Target Velocity"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Linear Motor Max Impulse"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Bool>("Use Angular Motor"_ustr);
  b.add_input<decl::Float>("Angular Motor Target Velocity"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Angular Motor Max Impulse"_ustr).default_value(1.0f).min(0.0f);
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "constraint_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = RBC_TYPE_POINT;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (!ELEM(node->custom1,
            RBC_TYPE_POINT,
            RBC_TYPE_FIXED,
            RBC_TYPE_HINGE,
            RBC_TYPE_SLIDER,
            RBC_TYPE_PISTON,
            RBC_TYPE_6DOF,
            RBC_TYPE_6DOF_SPRING,
            RBC_TYPE_MOTOR))
  {
    node->custom1 = RBC_TYPE_POINT;
  }

  auto set_available = [&](const UString identifier, const bool available) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };

  const bool generic = ELEM(node->custom1, RBC_TYPE_6DOF, RBC_TYPE_6DOF_SPRING);
  const bool spring = node->custom1 == RBC_TYPE_6DOF_SPRING;
  const bool hinge = node->custom1 == RBC_TYPE_HINGE;
  const bool slider = node->custom1 == RBC_TYPE_SLIDER;
  const bool piston = node->custom1 == RBC_TYPE_PISTON;
  const bool motor = node->custom1 == RBC_TYPE_MOTOR;
  const bool uses_linear_limits = generic || slider || piston;
  const bool uses_angular_limits = generic || hinge || piston;

  set_available("Breakable"_ustr, !motor);
  set_available("Breaking Threshold"_ustr, !motor);
  set_available("Override Iterations"_ustr, true);
  set_available("Velocity Solver Iterations"_ustr, true);
  set_available("Position Solver Iterations"_ustr, true);

  set_available("Use Linear Limit X"_ustr, uses_linear_limits);
  set_available("Use Linear Limit Y"_ustr, generic);
  set_available("Use Linear Limit Z"_ustr, generic);
  set_available("Linear Lower"_ustr, uses_linear_limits);
  set_available("Linear Upper"_ustr, uses_linear_limits);

  set_available("Use Angular Limit X"_ustr, generic || piston);
  set_available("Use Angular Limit Y"_ustr, generic);
  set_available("Use Angular Limit Z"_ustr, generic || hinge);
  set_available("Angular Lower"_ustr, uses_angular_limits);
  set_available("Angular Upper"_ustr, uses_angular_limits);

  set_available("Use Spring X"_ustr, spring);
  set_available("Use Spring Y"_ustr, spring);
  set_available("Use Spring Z"_ustr, spring);
  set_available("Spring Stiffness"_ustr, spring);
  set_available("Spring Damping"_ustr, spring);
  set_available("Use Angular Spring X"_ustr, spring);
  set_available("Use Angular Spring Y"_ustr, spring);
  set_available("Use Angular Spring Z"_ustr, spring);
  set_available("Angular Spring Stiffness"_ustr, spring);
  set_available("Angular Spring Damping"_ustr, spring);

  set_available("Use Linear Motor"_ustr, motor);
  set_available("Linear Motor Target Velocity"_ustr, motor);
  set_available("Linear Motor Max Impulse"_ustr, motor);
  set_available("Use Angular Motor"_ustr, motor);
  set_available("Angular Motor Target Velocity"_ustr, motor);
  set_available("Angular Motor Max Impulse"_ustr, motor);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeAddPhysicsConstraint"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_add_physics_constraint_cc

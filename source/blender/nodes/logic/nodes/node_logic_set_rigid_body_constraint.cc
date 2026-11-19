/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Set Rigid Body Constraint (2026-11-17): changes a live rigid body constraint (made by Add Rigid
 * Body Constraints or in the Physics panel) of an object by its name: switched on or off, its
 * limits, motor, springs or breaking (custom1, LN_RigidBodyConstraintSetting), with the Add
 * node's inputs: a door that locks, a drawbridge's motor, a spring that stiffens. Jolt only.
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_rigid_body_constraint_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object holding the constraint (the first object of Add Rigid Body "
                   "Constraints); empty: the owner");
  b.add_input<decl::String>("Name"_ustr)
      .default_value("constraint")
      .description("The constraint's name (Add Rigid Body Constraints' Name, or the Physics "
                   "panel's constraint)");
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("On: the constraint holds the bodies; off: they move freely");
  b.add_input<decl::Bool>("Use Linear Limit X"_ustr)
      .description("Slider, Piston, Generic: limit sliding along X (off: free)");
  b.add_input<decl::Bool>("Use Linear Limit Y"_ustr).description("Generic: limit sliding along Y");
  b.add_input<decl::Bool>("Use Linear Limit Z"_ustr).description("Generic: limit sliding along Z");
  b.add_input<decl::Vector>("Linear Lower"_ustr).default_value({-1.0f, -1.0f, -1.0f});
  b.add_input<decl::Vector>("Linear Upper"_ustr).default_value({1.0f, 1.0f, 1.0f});
  b.add_input<decl::Bool>("Use Angular Limit X"_ustr)
      .description("Piston, Generic: limit turning around X");
  b.add_input<decl::Bool>("Use Angular Limit Y"_ustr)
      .description("Generic: limit turning around Y");
  b.add_input<decl::Bool>("Use Angular Limit Z"_ustr)
      .description("Hinge, Generic: limit turning around Z (a hinge's swing)");
  b.add_input<decl::Vector>("Angular Lower"_ustr)
      .default_value({-0.7853981633974483f, -0.7853981633974483f, -0.7853981633974483f})
      .description("Radians");
  b.add_input<decl::Vector>("Angular Upper"_ustr)
      .default_value({0.7853981633974483f, 0.7853981633974483f, 0.7853981633974483f})
      .description("Radians");
  b.add_input<decl::Bool>("Use Linear Motor"_ustr)
      .description("Motor: drive sliding along X (off: the motor stops)");
  b.add_input<decl::Float>("Linear Motor Target Velocity"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Linear Motor Max Impulse"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Bool>("Use Angular Motor"_ustr)
      .description("Motor: drive turning around X (off: the motor stops)");
  b.add_input<decl::Float>("Angular Motor Target Velocity"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Angular Motor Max Impulse"_ustr).default_value(1.0f).min(0.0f);
  b.add_input<decl::Bool>("Use Spring X"_ustr).description("Generic Spring: a spring along X");
  b.add_input<decl::Bool>("Use Spring Y"_ustr).description("Generic Spring: a spring along Y");
  b.add_input<decl::Bool>("Use Spring Z"_ustr).description("Generic Spring: a spring along Z");
  b.add_input<decl::Vector>("Spring Stiffness"_ustr)
      .default_value({10.0f, 10.0f, 10.0f})
      .min(0.0f)
      .description("Jolt linear spring stiffness in N/m");
  b.add_input<decl::Vector>("Spring Damping"_ustr)
      .default_value({0.5f, 0.5f, 0.5f})
      .min(0.0f)
      .description("Jolt linear spring damping in N s/m");
  b.add_input<decl::Bool>("Use Angular Spring X"_ustr)
      .description("Generic Spring: a spring around X");
  b.add_input<decl::Bool>("Use Angular Spring Y"_ustr)
      .description("Generic Spring: a spring around Y");
  b.add_input<decl::Bool>("Use Angular Spring Z"_ustr)
      .description("Generic Spring: a spring around Z");
  b.add_input<decl::Vector>("Angular Spring Stiffness"_ustr)
      .default_value({10.0f, 10.0f, 10.0f})
      .min(0.0f)
      .description("Jolt angular spring stiffness in N m/rad");
  b.add_input<decl::Vector>("Angular Spring Damping"_ustr)
      .default_value({0.5f, 0.5f, 0.5f})
      .min(0.0f)
      .description("Jolt angular spring damping in N m s/rad");
  b.add_input<decl::Bool>("Breakable"_ustr)
      .description("The constraint breaks when its force goes past the Breaking Threshold");
  b.add_input<decl::Float>("Breaking Threshold"_ustr).default_value(10.0f).min(0.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "setting", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

/** Only the chosen setting's inputs show. */
static void node_update(bNodeTree *ntree, bNode *node)
{
  const int setting = node->custom1;
  static const char *const limits[] = {"Use Linear Limit X",
                                       "Use Linear Limit Y",
                                       "Use Linear Limit Z",
                                       "Linear Lower",
                                       "Linear Upper",
                                       "Use Angular Limit X",
                                       "Use Angular Limit Y",
                                       "Use Angular Limit Z",
                                       "Angular Lower",
                                       "Angular Upper"};
  static const char *const motor[] = {"Use Linear Motor",
                                      "Linear Motor Target Velocity",
                                      "Linear Motor Max Impulse",
                                      "Use Angular Motor",
                                      "Angular Motor Target Velocity",
                                      "Angular Motor Max Impulse"};
  static const char *const springs[] = {"Use Spring X",
                                        "Use Spring Y",
                                        "Use Spring Z",
                                        "Spring Stiffness",
                                        "Spring Damping",
                                        "Use Angular Spring X",
                                        "Use Angular Spring Y",
                                        "Use Angular Spring Z",
                                        "Angular Spring Stiffness",
                                        "Angular Spring Damping"};
  static const char *const breaking[] = {"Breakable", "Breaking Threshold"};
  const auto show = [&](const auto &names, const bool available) {
    for (const char *name : names) {
      if (bNodeSocket *socket = node->input_by_identifier(UString(name))) {
        bke::node_set_socket_availability(*ntree, *socket, available);
      }
    }
  };
  if (bNodeSocket *socket = node->input_by_identifier("Enabled"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, setting == 0);
  }
  show(limits, setting == 1);
  show(motor, setting == 2);
  show(springs, setting == 3);
  show(breaking, setting == 4);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetRigidBodyConstraint"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_rigid_body_constraint_cc

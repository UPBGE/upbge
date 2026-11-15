/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Apply Angular Impulse (2026-11-11): one spin kick. The engine compiles it to Apply Impulse's
 * instruction (LN_ImpulseMode::Angular).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_apply_angular_impulse_cc {

namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object to spin; empty: the object running this tree");
  b.add_input<decl::Vector>("Angular Impulse"_ustr)
      .description(
          "The spin to add at once, around the world axes: the object turns around this vector "
          "(right-hand rule: thumb along it, fingers show the way), more for a longer vector "
          "and a lighter object");
  b.add_input<decl::Bool>("Local"_ustr)
      .description("Around the object's own axes instead of the world's");
  b.add_input<decl::Bool>("Ignore Mass"_ustr)
      .description(
          "Angular Impulse is a spin speed change in radians per second: light and heavy "
          "objects of any shape get the same spin");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .description(
          "Draw the spin in the step it happens: a blue arrow along the spin axis (1 m per "
          "radian per second of spin it gave), a circle around it and a green cross at the center "
          "of mass");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeApplyAngularImpulse"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_apply_angular_impulse_cc

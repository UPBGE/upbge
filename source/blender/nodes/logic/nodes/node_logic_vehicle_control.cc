/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_vehicle_control_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Vehicle"_ustr).description(
      "Vehicle chassis object to control; defaults to the owner when unset");
  b.add_input<decl::Float>("Throttle"_ustr).default_value(0.0f).description(
      "Forward or reverse drive input");
  b.add_input<decl::Float>("Brake"_ustr).default_value(0.0f).description(
      "Brake input; Jolt treats this as normalized [0, 1]");
  b.add_input<decl::Float>("Handbrake"_ustr).default_value(0.0f).description(
      "Handbrake input; Jolt treats this as normalized [0, 1]");
  b.add_input<decl::Float>("Steering"_ustr).default_value(0.0f).description(
      "Steering input applied across the vehicle");
  b.add_output<decl::Execution>("Out"_ustr);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeVehicleControl"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_vehicle_control_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Drive Vehicle (LogicNativeDriveVehicle, NOD_logic_merged.hh),
 * each the node it was before 2026-11-08: VehicleAccelerate, VehicleBrake, VehicleSteer. They are
 * not node types of their own: logic_option_register keeps them for the merged node, which
 * forwards its declaration, buttons, init and update to the option it holds
 * (node_logic_merged.cc).
 */

#include "node_logic_util.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_vehicle_accelerate_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Vehicle"_ustr).description(
      "Vehicle chassis object to affect; defaults to the owner when unset");
  b.add_input<decl::Int>("Wheels"_ustr).default_value(2).min(0).description(
      "How many wheels from the selected axis group to affect");
  b.add_input<decl::Float>("Power"_ustr).default_value(1.0f).description(
      "Normalized drive power to apply");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "vehicle_axis", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeVehicleAccelerate"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_vehicle_accelerate_cc

namespace blender::nodes::node_logic_vehicle_brake_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Vehicle"_ustr).description(
      "Vehicle chassis object to affect; defaults to the owner when unset");
  b.add_input<decl::Int>("Wheels"_ustr).default_value(2).min(0).description(
      "How many wheels from the selected axis group to affect");
  b.add_input<decl::Float>("Power"_ustr).default_value(1.0f).min(0.0f).description(
      "Normalized braking power to apply");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "vehicle_axis", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeVehicleBrake"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_vehicle_brake_cc

namespace blender::nodes::node_logic_vehicle_steer_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Vehicle"_ustr).description(
      "Vehicle chassis object to affect; defaults to the owner when unset");
  b.add_input<decl::Int>("Wheels"_ustr).default_value(2).min(0).description(
      "How many wheels from the selected axis group to affect");
  b.add_input<decl::Float>("Steer"_ustr).default_value(0.0f).description(
      "Normalized steering value to apply");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "vehicle_axis", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  /* Front: the wheels with steering. Rear (0, every node's until 2026-11-08) picks the wheels
   * without steering, whose steering angle is 0: a new Steer node turned nothing. */
  node->custom1 = 1;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeVehicleSteer"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_vehicle_steer_cc

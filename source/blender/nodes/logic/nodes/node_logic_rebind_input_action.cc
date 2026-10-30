/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_rebind_input_action_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::String>("Action"_ustr)
      .default_value("")
      .description("An action of the start scene's Game Input Map (Scene > Game Input Map)")
      .custom_draw(logic::draw_input_action_socket);
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .description("Player whose next control is taken; the keyboard and mouse are player 0's");
  b.add_input<decl::Float>("Timeout"_ustr)
      .default_value(5.0f)
      .min(0.0f)
      .subtype(PROP_TIME_ABSOLUTE)
      .description("Seconds to wait for a control; 0 waits until one is pressed or Esc");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("A control was pressed, or Esc or the timeout ended the wait");
  b.add_output<decl::Bool>("Success"_ustr).description("The action got the new control");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "rebind_device", UI_ITEM_NONE, "", ICON_NONE);
  /* Only for vector actions: which way the binding pushes. */
  layout.prop(ptr, "input_direction", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 1;
  node->custom2 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeRebindInputAction"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  /* Wide enough for an action name. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_rebind_input_action_cc

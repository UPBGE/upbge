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

namespace blender::nodes::node_logic_input_action_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Action"_ustr)
      .default_value("")
      .description("An action of the start scene's Game Input Map (Scene > Game Input Map)")
      .custom_draw(logic::draw_input_action_socket);
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .description("Player: 0 has the keyboard and mouse. Gamepads: Scene > Game Input Map (by "
                   "default player N has gamepad N)");
  logic::add_input_button_status_outputs(b);
  b.add_output<decl::Float>("Value"_ustr)
      .description("How far the action is pressed, from 0 to 1 (a vector action: its length)");
  b.add_output<decl::Vector>("Vector"_ustr)
      .description("A vector action's direction: X right, Y up, at most 1 long");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "input_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "use_ui_input", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeInputAction"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  /* Wide enough for an action name and Also When UI Uses It. */
  bke::node_type_size(ntype, 190, 140, NODE_DEFAULT_MAX_WIDTH);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_input_action_cc

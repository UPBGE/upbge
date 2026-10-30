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

namespace blender::nodes::node_logic_gamepad_button_cc {
namespace decl = blender::nodes::logic::decl;

static bool gamepad_button_shows_strength(const int16_t button_index)
{
  return button_index == 15 || button_index == 16;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("Index"_ustr).default_value(0).min(0);
  b.add_output<decl::Execution>("Triggered"_ustr, "Out"_ustr)
      .is_default_link_socket()
      .description(
          "Logic + Physics step pulse selected by the Pressed, While Held, or Released mode");
  b.add_output<decl::Condition>("Active"_ustr).description(
      "True while active; fixed flow reads the fixed-step input snapshot");
  b.add_output<decl::Condition>("Pressed"_ustr).description(
      "True once when pressed; frame and fixed flow consume independent accumulated edges");
  b.add_output<decl::Condition>("Released"_ustr).description(
      "True once when released; frame and fixed flow consume independent accumulated edges");
  b.add_output<decl::Float>("Strength"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "gamepad_button", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "input_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "use_ui_input", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *strength = node->output_by_identifier("Strength"_ustr);
  if (strength) {
    bke::node_set_socket_availability(
        *ntree, *strength, gamepad_button_shows_strength(node->custom2));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGamepadButton"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_gamepad_button_cc

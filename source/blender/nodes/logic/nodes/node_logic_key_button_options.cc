/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Key / Button (LogicNativeKeyButton, NOD_logic_merged.hh), each
 * the node it was before 2026-11-08: KeyboardKey, MouseButton, GamepadButton. They are not node
 * types of their own: logic_option_register keeps them for the merged node, which forwards its
 * declaration, buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BLI_string.h"
#include "RNA_access.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_keyboard_key_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Key"_ustr)
      .default_value("A")
      .custom_draw([](CustomSocketDrawParams &params) {
        if (params.socket.is_directly_linked()) {
          params.layout.label(params.label, ICON_NONE);
          return;
        }

        const auto *value = static_cast<const bNodeSocketValueString *>(
            params.socket.default_value);
        const char *label = (value != nullptr && value->value[0] != '\0') ? value->value :
                                                                          "Press & Choose";

        params.layout.alignment_set(ui::LayoutAlign::Expand);
        ui::Layout &row = params.layout.row(true);
        PointerRNA op_ptr = row.op("NODE_OT_logic_keyboard_key_selector", label, ICON_MOUSE_LMB);
        RNA_string_set(&op_ptr, "tree_name", params.tree.id.name + 2);
        RNA_int_set(&op_ptr, "node_identifier", params.node.identifier);
        RNA_string_set(&op_ptr, "socket_identifier", params.socket.identifier);
      });
  logic::add_input_button_status_outputs(b);
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

  logic_option_type_base(&ntype, "LogicNativeKeyboardKey"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_keyboard_key_cc

namespace blender::nodes::node_logic_mouse_button_cc {
namespace decl = blender::nodes::logic::decl;

static void sync_mouse_button_socket_default(bNode &node)
{
  blender::bNodeSocket *socket = node.input_by_identifier("Button"_ustr);
  if (socket == nullptr || socket->default_value == nullptr) {
    return;
  }

  const char *button_id = "LEFTMOUSE";
  switch (node.custom2) {
    case 1:
      button_id = "MIDDLEMOUSE";
      break;
    case 2:
      button_id = "RIGHTMOUSE";
      break;
    default:
      break;
  }

  STRNCPY(static_cast<bNodeSocketValueString *>(socket->default_value)->value, button_id);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Button"_ustr)
      .default_value("LEFTMOUSE")
      .custom_draw([](CustomSocketDrawParams &params) {
        if (params.socket.is_directly_linked()) {
          params.layout.label(params.label, ICON_NONE);
          return;
        }
        params.layout.alignment_set(ui::LayoutAlign::Expand);
        ui::Layout &row = params.layout.row(true);
        row.prop(&params.node_ptr, "mouse_button", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
      });
  logic::add_input_button_status_outputs(b);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "input_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "use_ui_input", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
  sync_mouse_button_socket_default(*node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeMouseButton"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_mouse_button_cc

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

  logic_option_type_base(&ntype, "LogicNativeGamepadButton"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_gamepad_button_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node_runtime.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_keyboard_key_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  /* Outputs, the Input Type, the Key, then Also When UI Uses It last. */
  b.use_custom_socket_order();
  logic::add_input_button_status_outputs(b);
  b.add_default_layout();
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
  b.add_input<decl::Float>("Time"_ustr)
      .default_value(0.3f)
      .min(0.0f)
      .description(logic::input_gesture_time_description);
  logic::add_use_ui_input_layout(b);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "input_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeKeyboardKey"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.updatefunc = logic::input_gesture_time_update;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_keyboard_key_cc

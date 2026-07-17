/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BLI_string.h"

#include "BKE_node_runtime.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

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
  b.add_input<decl::String>(""_ustr, "Button"_ustr)
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

  logic_node_type_base(&ntype, "LogicNativeMouseButton"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_mouse_button_cc

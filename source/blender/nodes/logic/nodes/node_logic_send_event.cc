/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BLI_listbase.h"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_send_event_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::String>("Subject"_ustr).default_value("");
  b.add_input<decl::Generic>("Content"_ustr);
  b.add_input<decl::Object>("Messenger"_ustr);
  b.add_input<decl::Object>("Target"_ustr);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "advanced", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_target", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *content = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 2));
  bNodeSocket *messenger = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 3));
  bNodeSocket *target = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 4));
  /* Bit 2 was a removed "same tick delivery" flag; strip it so it does not enable sockets. */
  node->custom1 = int16_t(node->custom1 & 1);
  const bool advanced = node->custom1 != 0;
  const bool use_target = node->custom2 != 0;
  bke::node_set_socket_availability(*ntree, *content, advanced);
  bke::node_set_socket_availability(*ntree, *messenger, advanced);
  bke::node_set_socket_availability(*ntree, *target, use_target);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSendEvent"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_send_event_cc

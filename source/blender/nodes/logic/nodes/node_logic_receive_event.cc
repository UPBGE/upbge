/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BLI_listbase.h"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_receive_event_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Subject"_ustr).default_value("");
  b.add_input<decl::Object>("Target"_ustr);
  b.add_output<decl::Execution>("Out"_ustr);
  b.add_output<decl::Condition>("Received"_ustr);
  b.add_output<decl::Generic>("Content"_ustr);
  b.add_output<decl::Object>("Messenger"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "use_target", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *target = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 1));
  bke::node_set_socket_availability(*ntree, *target, node->custom1 != 0);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeReceiveEvent"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_receive_event_cc

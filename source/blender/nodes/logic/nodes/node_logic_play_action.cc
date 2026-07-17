/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_play_action_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::Float>("Start Frame"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("End Frame"_ustr).default_value(250.0f);
  b.add_input<decl::Int>("Layer"_ustr).default_value(0).min(0).max(7);
  b.add_input<decl::Int>("Priority"_ustr).default_value(0);
  b.add_input<decl::Float>("Blend In"_ustr).default_value(0.0f).min(0.0f);
  b.add_input<decl::Float>("Speed"_ustr).default_value(1.0f);
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  layout.prop(ptr, "play_mode", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "blend_mode", UI_ITEM_NONE, "", ICON_NONE);
  template_id(&layout, C, ptr, "action", nullptr, nullptr, nullptr);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0; /* BL_Action::ACT_MODE_PLAY */
  node->custom2 = 0; /* BL_Action::ACT_BLEND_BLEND */
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativePlayAction"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_play_action_cc

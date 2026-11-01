/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_on_fixed_update_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Execution>("Out"_ustr)
      .description(
          "Once during every fixed Logic + Physics step (or every N-th with Every), before "
          "physics simulation");
  b.add_output<decl::Float>("Time Passed"_ustr)
      .description(
          "Game seconds since this node's route last ran: one step, or N steps with Every N. "
          "Multiply speeds by it in a route that runs every N steps");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "step_interval", UI_ITEM_NONE, "Every", ICON_NONE);
  /* Nodes saved before Every hold 0: every step, as 1. */
  if (static_cast<const bNode *>(ptr->data)->custom1 > 1) {
    layout.prop(ptr, "use_step_spread", UI_ITEM_NONE, "Spread Out", ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 1;
  node->custom2 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeOnFixedUpdate"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_on_fixed_update_cc

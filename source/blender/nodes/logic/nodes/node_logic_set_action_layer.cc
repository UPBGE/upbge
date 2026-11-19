/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Set Action Layer (2026-11-16): changes a playing action's Speed or Layer Weight (custom1 0 /
 * 1). It compiles to Set Action Frame's instruction with the attribute in its
 * secondary_int_value (BL_Action::SetSpeed / SetLayerWeight at the flush).
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_action_layer_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr);
  b.add_input<decl::Int>("Layer"_ustr).default_value(0).min(0).max(7);
  b.add_input<decl::Float>("Speed"_ustr)
      .default_value(1.0f)
      .description("The action's new speed (1 as made, 2 twice as fast, 0 holds its frame); the "
                   "frame shown stays where it is");
  b.add_input<decl::Float>("Layer Weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .description("How much of the layers below still shows: 0 none, 1 all (an armature's "
                   "action follows it at once; a shape-key action blends only when Play Action "
                   "gave it a weight)");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "attribute", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (bNodeSocket *speed = node->input_by_identifier("Speed"_ustr)) {
    bke::node_set_socket_availability(*ntree, *speed, node->custom1 == 0);
  }
  if (bNodeSocket *weight = node->input_by_identifier("Layer Weight"_ustr)) {
    bke::node_set_socket_availability(*ntree, *weight, node->custom1 == 1);
  }
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 0;
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetActionLayer"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_action_layer_cc

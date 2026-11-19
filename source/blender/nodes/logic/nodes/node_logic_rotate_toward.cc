/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_rotate_toward_cc {
namespace decl = blender::nodes::decl;
namespace logic_decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<logic_decl::Execution>("Flow"_ustr);
  b.add_input<logic_decl::Object>("Object"_ustr);
  b.add_input<logic_decl::Vector>("Target"_ustr).default_value({1.0f, 0.0f, 0.0f});
  b.add_input<logic_decl::Float>("Factor"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .description("Fraction of the required rotation applied on each fixed step");
  b.add_input<decl::Float>("Turn Speed"_ustr)
      .default_value(float(M_PI))
      .min(0.0f)
      .subtype(PROP_ANGLE)
      .description("How far it turns a second at most (180: half a turn a second); it stops "
                   "exactly on the target");
  b.add_input<logic_decl::Int>("Rot Axis"_ustr).default_value(2).min(0).max(2);
  b.add_input<logic_decl::Int>("Front"_ustr, "Front Axis"_ustr).default_value(1).min(0).max(5);
  b.add_output<logic_decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "turn", UI_ITEM_NONE, "", ICON_NONE);
}

/* custom1 (2026-11-16): 0 Factor (every older node), 1 Turn Speed. */
static void node_update(bNodeTree *ntree, bNode *node)
{
  if (bNodeSocket *factor = node->input_by_identifier("Factor"_ustr)) {
    bke::node_set_socket_availability(*ntree, *factor, node->custom1 != 1);
  }
  if (bNodeSocket *speed = node->input_by_identifier("Turn Speed"_ustr)) {
    bke::node_set_socket_availability(*ntree, *speed, node->custom1 == 1);
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

  logic_node_type_base(&ntype, "LogicNativeRotateToward"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_rotate_toward_cc

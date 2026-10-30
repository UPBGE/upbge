/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_map_range_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Value"_ustr)
      .default_value(0.0f)
      .description("Value to remap from the source range");
  b.add_input<decl::Float>("From Min"_ustr)
      .default_value(0.0f)
      .description("First endpoint of the source range");
  b.add_input<decl::Float>("From Max"_ustr)
      .default_value(1.0f)
      .description("Second endpoint of the source range");
  b.add_input<decl::Float>("To Min"_ustr)
      .default_value(0.0f)
      .description("Result when Value equals From Min");
  b.add_input<decl::Float>("To Max"_ustr)
      .default_value(1.0f)
      .description("Result when Value equals From Max");
  b.add_output<decl::Float>("Result"_ustr).description(
      "Linearly remapped value; equal source endpoints return To Min");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "use_clamp", UI_ITEM_NONE, "Clamp", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  /* Match Blender's Map Range default for newly created nodes. */
  node->custom1 = 1;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeMapRange"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_map_range_cc

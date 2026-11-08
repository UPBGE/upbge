/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Get Random List Item: custom2 the variation (0 Per Object, 1 Same for All), as Random Value.
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_list_random_item_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::List>("List"_ustr);
  b.add_input<decl::Int>("Seed"_ustr)
      .default_value(0)
      .description("Another number picks other items. Link a Loop's Index for a new pick in "
                   "every iteration");
  b.add_output<decl::Generic>("Value"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "variation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeListRandomItem"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_list_random_item_cc

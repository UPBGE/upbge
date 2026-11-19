/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Date and Time (2026-11-17): the computer's clock, read once a step: daily rewards, a day and
 * night cycle that follows the real day, a save file's date. Local time, or UTC (custom1 1).
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_date_time_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Year"_ustr);
  b.add_output<decl::Int>("Month"_ustr).description("1 January to 12 December");
  b.add_output<decl::Int>("Day"_ustr).description("The day of the month, from 1");
  b.add_output<decl::Int>("Hour"_ustr).description("0 to 23");
  b.add_output<decl::Int>("Minute"_ustr);
  b.add_output<decl::Int>("Second"_ustr);
  b.add_output<decl::Int>("Weekday"_ustr).description("0 Monday to 6 Sunday");
  b.add_output<decl::Float>("Day Fraction"_ustr)
      .description("How much of the day has passed, 0 at midnight to 1 (a day and night "
                   "cycle)");
  b.add_output<decl::Int>("Day Number"_ustr)
      .description("Days since 1 January 1970: a day differs from a saved one when a new day "
                   "began (daily rewards)");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "use_utc", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeDateTime"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_date_time_cc

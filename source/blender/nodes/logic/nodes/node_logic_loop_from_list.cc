/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_loop_from_list_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description(
      "Starts the loop. While a loop spread over several steps still runs, new pulses are "
      "ignored");
  b.add_input<decl::List>("List"_ustr).description(
      "List to snapshot when the loop starts; later list changes do not affect the active loop");
  /* (Shown as "Max Steps": the identifier stays, so saved files keep their values and links.) */
  b.add_input<decl::Int>("Max Steps"_ustr, "Ticks to Complete"_ustr)
      .default_value(1)
      .min(1)
      .description(
          "The most steps the list items are spread over (frames, when On Frame Update starts "
          "the loop). Each step runs at least one item, so fewer items than steps end early: 11 "
          "items over 120 steps end after 11 steps, and Completed pulses then. 1 runs them all "
          "at once (up to 10,000 a step; the rest go on in the next step)");
  b.add_output<decl::Execution>("Loop"_ustr).description("Pulses once for each list item");
  b.add_output<decl::Execution>("Completed"_ustr).description(
      "Pulses once after all items, an empty list, or Break Loop");
  b.add_output<decl::Int>("Index"_ustr).description("Zero-based index of the current list item");
  b.add_output<decl::Generic>("List Item"_ustr).description(
      "Current item from the snapshotted list");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeLoopFromList"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_loop_from_list_cc

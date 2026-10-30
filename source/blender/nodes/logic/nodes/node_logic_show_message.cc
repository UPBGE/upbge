/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_show_message_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("The text object showing the messages (or an object with a text child); "
                   "empty uses the owner");
  b.add_input<decl::String>("Text"_ustr)
      .description("Added as the newest line, at the bottom. The same text again moves to the "
                   "bottom and starts over. Braces work as in Set Text: {Use} shows an action's "
                   "key");
  b.add_input<decl::Int>("Count"_ustr)
      .default_value(-1)
      .min(-1)
      .description("How many of it, such as Inventory's Added for a pickup: the same text again "
                   "while its line shows adds to it, shown as \"Apple x5\" (nothing for 1) or "
                   "where the text says {count}; 0 adds no line (nothing was picked up). -1: a "
                   "message without a count");
  b.add_input<decl::Float>("Seconds"_ustr)
      .default_value(3.0f)
      .min(0.0f)
      .description("How long the line stays; 0 keeps it until newer lines push it out");
  b.add_input<decl::Int>("Lines"_ustr)
      .default_value(4)
      .min(1)
      .description("The most lines shown at once: a new line pushes out the oldest");
  b.add_input<decl::Int>("User"_ustr)
      .default_value(-1)
      .min(-1)
      .max(7)
      .description("-1: the object itself. 0 to 7: the player's own copy of a menu object, as "
                   "in a HUD opened for each player (nothing shows when the player has not "
                   "opened it)");
  b.add_output<decl::Execution>("Done"_ustr).description("Continues once the line was added");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeShowMessage"_ustr);
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_show_message_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * On Navigation Link (2026-11-18): pulses when an NPC starts crossing a navigation link (an
 * object with a nav_link game property naming the object at the other end: a jump, a ladder, a
 * door, a teleport), so the game plays its animation while the navigation carries it across;
 * On Link End (2026-11-19) when the crossing ended (a physics jump's landing). Bool::
 * NavigationState (LN_NAV_STATE_LINK_STARTED / _ENDED) as the events' conditions.
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_on_navigation_link_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr)
      .description("The NPC; empty: the tree's own object");
  b.add_output<decl::Execution>("On Link Start"_ustr)
      .description("Pulses in the Logic + Physics step after the NPC started crossing a link");
  b.add_output<decl::Execution>("On Link End"_ustr)
      .description("Pulses in the Logic + Physics step after the NPC's crossing ended: it came "
                   "to the link's end, or landed from a jump (on the end or, knocked off its "
                   "way, elsewhere: then it finds a new way)");
  b.add_output<decl::Object>("Link Object"_ustr)
      .description("The link's object (the one with the nav_link property; none for jumps the "
                   "navigation found by itself)");
  b.add_output<decl::String>("Link Kind"_ustr)
      .description("Its nav_link_kind property: jump (default), ladder, door, teleport or any "
                   "name the game gives; jump or drop for the jumps the navigation found");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeOnNavigationLink"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_on_navigation_link_cc

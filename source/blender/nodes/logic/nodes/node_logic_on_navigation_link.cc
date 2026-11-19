/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * On Navigation Link (2026-11-18): pulses when an NPC starts crossing a navigation link (an
 * object with a nav_link game property naming the object at the other end: a jump, a ladder, a
 * door, a teleport), so the game plays its animation while the navigation carries it across.
 * Bool::NavigationState (LN_NAV_STATE_LINK_STARTED) as the event's condition.
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
  b.add_output<decl::Object>("Link Object"_ustr)
      .description("The link's object (the one with the nav_link property)");
  b.add_output<decl::String>("Link Kind"_ustr)
      .description("Its nav_link_kind property: jump (default), ladder, door, teleport or any "
                   "name the game gives");
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

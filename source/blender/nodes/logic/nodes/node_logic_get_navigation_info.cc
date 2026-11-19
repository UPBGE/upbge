/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Get Navigation Info (2026-11-18): how an NPC's navigation went in the last step (Navigate and
 * Follow Path move it in the navigation step after the logic). Bool / Float /
 * Vector::NavigationState, Value::NavigationLinkObject, String::NavigationLinkKind.
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_get_navigation_info_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr)
      .description("The NPC (a Navigate or Follow Path moves it); empty: the tree's own object");
  b.add_output<decl::Bool>("Moving"_ustr).description("It walks toward its goal");
  b.add_output<decl::Bool>("Arrived"_ustr)
      .description("It is at its goal (within Reach Threshold), or as near as others let it");
  b.add_output<decl::Bool>("Has Path"_ustr).description("It follows a way on a navigation mesh");
  b.add_output<decl::Bool>("Path Complete"_ustr)
      .description("Its way reaches the goal (else it ends as near as it gets)");
  b.add_output<decl::Bool>("No Path"_ustr)
      .description("No navigation mesh point is near its goal (or none under it)");
  b.add_output<decl::Bool>("On Link"_ustr)
      .description("It crosses a navigation link (a jump, a ladder, a door, a teleport)");
  b.add_output<decl::Vector>("Next Corner"_ustr).description("The next corner of its way");
  b.add_output<decl::Vector>("Velocity"_ustr)
      .description("The velocity it was given (meters per second)");
  b.add_output<decl::Vector>("Goal"_ustr).description("Where it goes");
  b.add_output<decl::Float>("Remaining Distance"_ustr)
      .description("How far it still has to walk along its way");
  b.add_output<decl::Float>("Speed"_ustr).description("How fast it walks (meters per second)");
  b.add_output<decl::Object>("Link Object"_ustr)
      .description("The navigation link it crosses (the object with the nav_link property)");
  b.add_output<decl::String>("Link Kind"_ustr)
      .description("The link's kind: its nav_link_kind property (jump by default)");
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeGetNavigationInfo"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_navigation_info_cc

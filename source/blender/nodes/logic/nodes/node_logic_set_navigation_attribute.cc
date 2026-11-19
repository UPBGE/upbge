/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Set Navigation Attribute (2026-11-18): changes the scene's navigation while playing: an object
 * an obstacle or not (a door), what an NPC pays to walk an area or whether it may at all, the
 * navigation mesh made again in a box (level pieces changed). Op::NavigationAttribute, custom1
 * the LN_NavigationAttribute.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_navigation_attribute_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1: LN_NavigationAttribute. */
enum Attribute { Obstacle = 0, AreaCost, AreaClosed, RebuildArea };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Obstacle: the object (a door, a crate). Area Cost / Area Closed: the NPC. "
                   "Empty: the tree's own object");
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("Obstacle: NPCs walk around it (cut out of the scene's navigation mesh while "
                   "it stands still); off: they walk through it");
  b.add_input<decl::String>("Area"_ustr)
      .description("The area's name: the nav_area game property of the level objects it is "
                   "made of (ground: everything else)");
  b.add_input<decl::Float>("Cost"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("How much a meter of the area costs this NPC (1: as the ground, 5: it goes "
                   "far round)");
  b.add_input<decl::Bool>("Closed"_ustr)
      .default_value(true)
      .description("This NPC may not walk the area at all (a locked door's area, deep water)");
  b.add_input<decl::Vector>("Center"_ustr)
      .description("The middle of the box whose navigation mesh is made again");
  b.add_input<decl::Vector>("Size"_ustr)
      .default_value({10.0f, 10.0f, 10.0f})
      .description("The box's size (level pieces added, removed or moved by the game inside it "
                   "are taken in)");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "attribute", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Obstacle;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const int attribute = node->custom1;
  auto input = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  input("Object"_ustr, attribute != RebuildArea);
  input("Enabled"_ustr, attribute == Obstacle);
  input("Area"_ustr, attribute == AreaCost || attribute == AreaClosed);
  input("Cost"_ustr, attribute == AreaCost);
  input("Closed"_ustr, attribute == AreaClosed);
  input("Center"_ustr, attribute == RebuildArea);
  input("Size"_ustr, attribute == RebuildArea);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeSetNavigationAttribute"_ustr);
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_navigation_attribute_cc

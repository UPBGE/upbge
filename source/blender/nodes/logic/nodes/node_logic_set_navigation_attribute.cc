/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Set Navigation Attribute (2026-11-18): changes the scene's navigation while playing: an object
 * an obstacle or not (a door), what an NPC pays to walk an area or whether it may at all, the
 * navigation mesh made again in a box (level pieces changed), an NPC's jump limits (2026-11-19),
 * an object's Physics > Navigation settings: Vehicle, Link Enabled, Link End, Area (2026-11-20).
 * Op::NavigationAttribute, custom1 the LN_NavigationAttribute.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_navigation_attribute_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1: LN_NavigationAttribute (4 is Navigate's Stop, in no dropdown). */
enum Attribute {
  Obstacle = 0,
  AreaCost,
  AreaClosed,
  RebuildArea,
  JumpLimits = 5,
  Vehicle,
  LinkEnabled,
  LinkEnd,
  Area,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Obstacle, Vehicle, Area: the object (a door, a crate, a ship, a floor). "
                   "Link Enabled, Link End: the object whose link it is (Physics > Navigation > "
                   "Link). Area Cost, Area Closed, Jump Limits: the NPC. Empty: the tree's own "
                   "object");
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("Obstacle: NPCs walk around it (cut out of the scene's navigation mesh while "
                   "it stands still); off: they walk through it. Jump Limits: the NPC jumps "
                   "within these limits; off: as before them (a Character by its own jump, "
                   "another NPC takes every jump link). Vehicle: NPCs walk on it and ride "
                   "along; off: they stand on the world again. Link Enabled: the link is open; "
                   "off: closed (its When Closed: NPCs go round, or wait at it)");
  b.add_input<decl::String>("Area"_ustr)
      .description("The area's name: Physics > Navigation > Area of the level objects it is "
                   "made of (empty: ordinary ground). Area: the object's new area");
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
  b.add_input<decl::Float>("Jump Gap"_ustr)
      .default_value(3.0f)
      .min(0.0f)
      .description("The widest gap the NPC jumps across: the space between the two ledges "
                   "(0: it never jumps across a gap)");
  b.add_input<decl::Float>("Jump Up"_ustr)
      .default_value(1.5f)
      .min(0.0f)
      .description("How much higher than it walks up by itself (the scene's Max Climb) the NPC "
                   "jumps up to a ledge (0: never up)");
  b.add_input<decl::Float>("Drop Down"_ustr)
      .default_value(4.0f)
      .min(0.0f)
      .description("The deepest drop the NPC jumps down (0: it never drops off a ledge)");
  b.add_input<decl::Object>("End Object"_ustr)
      .description("Where the link goes now (a teleporter's destination, a gangway moved to "
                   "another ship). Empty: the object has no link");
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
  input("Enabled"_ustr,
        attribute == Obstacle || attribute == JumpLimits || attribute == Vehicle ||
            attribute == LinkEnabled);
  input("Area"_ustr, attribute == AreaCost || attribute == AreaClosed || attribute == Area);
  input("Cost"_ustr, attribute == AreaCost);
  input("Closed"_ustr, attribute == AreaClosed);
  input("Center"_ustr, attribute == RebuildArea);
  input("Size"_ustr, attribute == RebuildArea);
  input("Jump Gap"_ustr, attribute == JumpLimits);
  input("Jump Up"_ustr, attribute == JumpLimits);
  input("Drop Down"_ustr, attribute == JumpLimits);
  input("End Object"_ustr, attribute == LinkEnd);
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

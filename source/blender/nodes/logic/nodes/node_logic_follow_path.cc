/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Follow Path: walks its points in turn (places or objects) on the scene's navigation. Its Way
 * (custom1, 2026-11-19): Straight from point to point (every older node: on the Optional Navmesh
 * when one is given), or Navmesh (the Optional Navmesh, else the scene's, avoiding others).
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_follow_path_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1, LN_FOLLOW_PATH_WAY_*. */
enum Way { Straight = 0, Navmesh = 1 };

static void node_update(bNodeTree *ntree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  /* (2026-11-20, plan item 7: turning and the other objects in folded sections.) */
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Execution>("Point Reached"_ustr)
      .description("Pulses once when it reaches a point (Point Index: which), in the step "
                   "after, whether or not Flow runs then");
  b.add_output<decl::Int>("Point Index"_ustr)
      .description("The place in Path Points of the point it reached last (-1: none yet)");
  b.add_output<decl::Execution>("Finished"_ustr)
      .description("Pulses once when it reaches the last point (never with Loop)");
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::List>("Path Points"_ustr)
      .description("The points to walk to in turn: positions or objects (an object's point is "
                   "where it is, also while it moves); anything else is left out with a "
                   "warning");
  b.add_input<decl::Bool>("Loop"_ustr)
      .default_value(false)
      .description("After the last point, the first again (off: it stops at the last)");
  b.add_input<decl::Bool>("Continue"_ustr)
      .default_value(false)
      .description("When Flow runs again after a pause, it goes on to the point it walked to "
                   "(off: it starts again from the first point). A list of another length goes "
                   "on to that point where it is in the list, else from its first");
  b.add_input<decl::Float>("Lin Speed"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Movement speed in world units per second");
  b.add_input<decl::Float>("Reach Threshold"_ustr)
      .default_value(0.2f)
      .description("A point is reached when this near it (on the ground plane)");
  PanelDeclarationBuilder &turning = b.add_panel("Turning"_ustr).default_closed(true);
  turning.add_input<decl::Bool>("Look At"_ustr)
      .default_value(true)
      .panel_toggle()
      .description("Turn the Rotating Object toward the way it walks");
  turning.add_input<decl::Float>("Rot Speed"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Maximum look-at rotation speed in radians per second");
  turning.add_input<decl::Int>("Rot Axis"_ustr)
      .default_value(2)
      .description("The axis it turns around: 0 X, 1 Y, 2 Z");
  turning.add_input<decl::Int>("Front"_ustr)
      .default_value(1)
      .description("The axis that faces the way: 0 X, 1 Y, 2 Z, 3 -X, 4 -Y, 5 -Z");
  PanelDeclarationBuilder &other = b.add_panel("More"_ustr).default_closed(true);
  other.add_input<decl::Object>("Moving Object"_ustr)
      .description("The NPC that moves, or the owner when empty");
  other.add_input<decl::Object>("Rotating Object"_ustr)
      .description("The object that turns to face the way (the NPC itself when empty)");
  other.add_input<decl::Object>("Optional Navmesh"_ustr)
      .description("A navmesh object to walk on (Way Navmesh without one: the scene's)");
  other.add_input<decl::Bool>("Slide"_ustr)
      .default_value(false)
      .description("A Dynamic or Rigid Body NPC is moved exactly along its way, its position "
                   "set, instead of pushed by its velocity. Kinematic, Static and other NPCs "
                   "always slide so; a Character always walks");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "way", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Navmesh;
}

static bool socket_bool_default(const bNodeSocket *socket)
{
  if (socket == nullptr || socket->type != SOCK_BOOLEAN || socket->default_value == nullptr) {
    return false;
  }
  return static_cast<const bNodeSocketValueBoolean *>(socket->default_value)->value;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const bool look_at = socket_bool_default(node->input_by_identifier("Look At"_ustr));
  for (const StringRef socket_name : {"Rot Speed", "Rot Axis", "Front"}) {
    if (bNodeSocket *socket = node->input_by_identifier(UString(socket_name))) {
      bke::node_set_socket_availability(*ntree, *socket, look_at);
    }
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeFollowPath"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_follow_path_cc

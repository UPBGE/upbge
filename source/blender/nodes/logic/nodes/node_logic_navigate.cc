/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Navigate (rebuilt 2026-11-18 on the scene's navigation, KX_Navigation): moves an NPC along a
 * navmesh, avoiding the other NPCs, by its physics type (a Character walks, a body moved as
 * Dynamic gets a velocity, anything else slides along the navmesh). Its Mode (custom1) says
 * where: Go To a position or after an object, Wander, Flee, Pursue, Evade, Formation, Flock.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_navigate_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1, KX_NavRequest::Mode. */
enum Mode { GoTo = 0, Wander, Flee, Pursue, Evade, Formation, Flock };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Asks it to go. Without Until Arrived, asked every step: it walks while asked "
                   "and stops in the first step it is not (put a Branch or a State Machine in "
                   "front). With Until Arrived one pulse is enough");
  b.add_input<decl::Execution>("Stop"_ustr)
      .description("Ends its order: it stops (crossing a link, at the link's end). Needed to stop "
                   "an Until Arrived order before it arrives");
  b.add_input<decl::Object>("Moving Object"_ustr)
      .description("The NPC that moves, or the owner when empty");
  b.add_input<decl::Object>("Rotating Object"_ustr)
      .description("The object that turns to face the way (the NPC itself when empty: an "
                   "armature's parent, a body's mesh)");
  b.add_input<decl::Object>("Navmesh Object"_ustr)
      .description("A navigation mesh object (Build Navigation Mesh) to walk on. Empty: the "
                   "scene's own navigation mesh when Scene > Navigation Mesh > Build at Game "
                   "Start is on, else straight at the goal");
  b.add_input<decl::Vector>("Destination"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .description("Go To: where to go. Wander: the home it wanders around. Flee: what to get "
                   "away from when no Target Object is set");
  b.add_input<decl::Bool>("Move as Dynamic"_ustr)
      .default_value(false)
      .description("A dynamic body is moved by its velocity (it pushes and is pushed); off, it "
                   "slides along the navmesh as an object without physics. A Character always "
                   "walks");
  b.add_input<decl::Float>("Lin Speed"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Movement speed in world units per second");
  b.add_input<decl::Float>("Reach Threshold"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Arrived when this near the goal (on the ground plane)");
  b.add_input<decl::Bool>("Look At"_ustr)
      .default_value(true)
      .description("Turn the Rotating Object toward the way it walks");
  b.add_input<decl::Int>("Rot Axis"_ustr)
      .default_value(2)
      .description("The axis it turns around: 0 X, 1 Y, 2 Z");
  b.add_input<decl::Int>("Front"_ustr)
      .default_value(1)
      .description("The axis that faces the way: 0 X, 1 Y, 2 Z, 3 -X, 4 -Y, 5 -Z");
  b.add_input<decl::Float>("Rot Speed"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Maximum look-at rotation speed in radians per second");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .default_value(false)
      .description("Draws the way to the next corner and the goal");
  b.add_input<decl::Object>("Target Object"_ustr)
      .description("Go To: an object to go after (it may move). Flee: what to get away from. "
                   "Pursue / Evade: what to catch or escape (where it will be). Formation: "
                   "the leader. Flock: where the flock heads (empty: anywhere, together)");
  b.add_input<decl::Bool>("Avoid Others"_ustr)
      .default_value(true)
      .description("Steer around other NPCs and obstacles (the physics panel's Create "
                   "Obstacle) instead of walking into them");
  b.add_input<decl::Int>("Priority"_ustr)
      .default_value(50)
      .min(0)
      .max(100)
      .description("Others make way for an NPC of higher priority (0 to 100)");
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(10.0f)
      .min(0.0f)
      .description("Wander: how far from home. Flee / Evade: how far away to get. Flock: how "
                   "far the flock's members see each other");
  b.add_input<decl::Vector>("Offset"_ustr)
      .default_value({-2.0f, -2.0f, 0.0f})
      .description("Formation: its place in the leader's space (X right, Y forward)");
  b.add_input<decl::Vector>("Weights"_ustr)
      .default_value({1.0f, 1.0f, 1.0f})
      .description("Flock: X keeps apart, Y heads the same way as the others, Z stays together");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Execution>("When Reached"_ustr)
      .description("Pulses once when it arrives (Flee and Evade: far enough away; Wander never), "
                   "in the step after, also when nothing runs Navigate then (Until Arrived); "
                   "again after it walked off or got a new order");
  b.add_output<decl::List>("Next Point"_ustr)
      .description("The next corner of its way (from the last step)");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "use_until_arrived", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = GoTo;
  node->custom2 = 0;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  const int mode = node->custom1;
  auto input = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  input("Destination"_ustr, mode == GoTo || mode == Wander || mode == Flee);
  input("Target Object"_ustr, mode != Wander);
  input("Radius"_ustr, mode == Wander || mode == Flee || mode == Evade || mode == Flock);
  input("Offset"_ustr, mode == Formation);
  input("Weights"_ustr, mode == Flock);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeNavigate"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_type_size(ntype, 200, 160, NODE_DEFAULT_MAX_WIDTH);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_navigate_cc

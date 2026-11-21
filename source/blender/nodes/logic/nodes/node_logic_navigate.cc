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
/** The Avoidance input's values (the engine's LN_NAVIGATE_AVOIDANCE_*). */
enum Avoidance { AvoidOff = 0, AvoidPolite = 1, AvoidDense = 2 };
/** The Polite / Dense dropdown of nodes saved before 2026-11-20 (custom2 bit 1, Dense): node_update
 * moves it into the Avoidance input once. */
constexpr int16_t OldDenseBit = 1 << 1;

static void node_declare(NodeDeclarationBuilder &b)
{
  /* (2026-11-20, plan item 7: what most NPCs need in sight; turning, avoidance and the other
   * objects in folded sections.) */
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::Execution>("When Reached"_ustr)
      .description("Pulses once when this node's order arrives (Flee and Evade: far enough away; "
                   "Wander never), in the step after, also when nothing runs Navigate then "
                   "(Until Arrived); again after it walked off or got a new order. Another "
                   "Navigate's order arriving does not pulse it");
  b.add_output<decl::Execution>("When Failed"_ustr)
      .description("Pulses once when this node's order cannot be done (Get Navigation Info's "
                   "Reason says why: No Path, Unreachable, Off Navmesh, Stuck, Gave Up); again "
                   "for a new order or another reason");
  b.add_output<decl::List>("Next Point"_ustr)
      .description("The next corner of its way (from the last step)");
  b.add_default_layout();
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Asks it to go. Without Until Arrived, asked every step: it walks while asked "
                   "and stops in the first step it is not (put a Branch or a State Machine in "
                   "front). With Until Arrived one pulse is enough");
  b.add_input<decl::Execution>("Stop"_ustr)
      .description("Ends its order: it stops (crossing a link, at the link's end). Needed to stop "
                   "an Until Arrived order before it arrives");
  b.add_input<decl::Vector>("Destination"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .description("Go To: where to go. Wander: the home it wanders around. Flee: what to get "
                   "away from when no Target Object is set");
  b.add_input<decl::Object>("Target Object"_ustr)
      .description("Go To: an object to go after (it may move). Flee: what to get away from. "
                   "Pursue / Evade: what to catch or escape (where it will be). Formation: "
                   "the leader. Flock: where the flock heads (empty: anywhere, together)");
  b.add_input<decl::Float>("Lin Speed"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Movement speed in world units per second");
  b.add_input<decl::Float>("Reach Threshold"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Arrived when this near the goal (on the ground plane)");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .default_value(false)
      .description("Draws the way to the next corner and the goal");
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
  /* (2026-11-20, plan item 7: Avoidance a number, as Keep Distance's Mode, so a game property can
   * switch it while the game runs; it replaced the Avoid Others checkbox and the Polite / Dense
   * dropdown, custom2 bit 1, which node_update moves into it.) */
  PanelDeclarationBuilder &avoidance = b.add_panel("Avoidance"_ustr).default_closed(true);
  avoidance.add_input<decl::Int>("Avoidance"_ustr)
      .default_value(AvoidPolite)
      .min(AvoidOff)
      .max(AvoidDense)
      .description("How it deals with other NPCs, a number so that a game property can change it "
                   "while the game runs. 0 Off: it walks straight through everyone (the others "
                   "still avoid it). 1 Polite: it steps aside for the others ahead of time (calm "
                   "crowds). 2 Dense: it presses and pushes in packed crowds, the higher Priority "
                   "shoving the lower (doors, exits, hordes at a barricade). Below 0 counts as 0, "
                   "above 2 as 2");
  avoidance.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA * /*ptr*/) {
    layout.label(IFACE_("0 Off · 1 Polite · 2 Dense"), ICON_NONE);
  });
  avoidance.add_input<decl::Int>("Priority"_ustr)
      .default_value(50)
      .min(0)
      .max(100)
      .description("Others make way for an NPC of higher priority (0 to 100). Avoidance Dense: "
                   "its weight too, doubling every 12.5 (100 shoves a 50 aside 16 times as "
                   "hard)");
  PanelDeclarationBuilder &other = b.add_panel("More"_ustr).default_closed(true);
  other.add_input<decl::Object>("Moving Object"_ustr)
      .description("The NPC that moves, or the owner when empty");
  other.add_input<decl::Object>("Rotating Object"_ustr)
      .description("The object that turns to face the way (the NPC itself when empty: an "
                   "armature's parent, a body's mesh)");
  other.add_input<decl::Object>("Navmesh Object"_ustr)
      .description("A navigation mesh object (Build Navigation Mesh) to walk on. Empty: the "
                   "scene's own navigation mesh (Scene > Navigation Mesh > Build), else straight "
                   "at the goal");
  other.add_input<decl::Bool>("Slide"_ustr)
      .default_value(false)
      .description("A Dynamic or Rigid Body NPC is moved exactly along its way, its position "
                   "set, instead of pushed by its velocity (pushing and pushed). Kinematic, "
                   "Static and other NPCs always slide so; a Character always walks");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "use_until_arrived", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "movement", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  /* (Climb is the walkers': flyers and swimmers have none.) */
  if (RNA_enum_get(ptr, "movement") == 0) {
    layout.prop(ptr, "climb", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = GoTo;
  node->custom2 = 0;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  /* A node saved with the Dense dropdown (custom2 bit 1): Dense into the Avoidance input (the
   * sockets are made before this runs when a file is read), once. A user count is never touched
   * here (this also runs while a file is read). */
  if (node->custom2 & OldDenseBit) {
    bNodeSocket *avoidance = node->input_by_identifier("Avoidance"_ustr);
    if (avoidance != nullptr && avoidance->default_value != nullptr) {
      static_cast<bNodeSocketValueInt *>(avoidance->default_value)->value = AvoidDense;
      node->custom2 &= ~OldDenseBit;
    }
  }
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

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Aim for Arc (2026-11-11): the launch velocity that lands a throw on a target (grenades, AI
 * throws, mortars, basketball). Math only, no physics: LN_AimMath.h's LN_SolveArc, one PURE kind
 * per output (Vector::ArcLaunchVelocity, Bool::ArcCanReach, Float::ArcFlightTime).
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_aim_for_arc_cc {
namespace decl = blender::nodes::logic::decl;

/** custom1: LN_ArcSolveMode. custom2: gravity source. */
enum SolveBy { Speed = 0, ArcHeight };
enum GravitySource { SceneGravity = 0, CustomGravity };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Vector>("Launch Velocity"_ustr)
      .description("The velocity to throw with (world units per second): give it to Set Object "
                   "Attribute's World Linear Velocity, or as an impulse times the mass. Can Reach "
                   "off (Speed): the throw at that speed that goes farthest toward the target");
  b.add_output<decl::Condition>("Can Reach"_ustr)
      .description("True when the throw lands on the target (Speed: false when it is too far for "
                   "that speed)");
  b.add_output<decl::Float>("Flight Time"_ustr)
      .description("Seconds from the throw to the target; 0 when it cannot reach");
  b.add_default_layout();
  b.add_input<decl::Vector>("Start"_ustr).description("Where the throw starts (world)");
  b.add_input<decl::Vector>("Target"_ustr)
      .default_value(float3(10.0f, 0.0f, 0.0f))
      .description("Where the throw should land (world)");
  b.add_input<decl::Float>("Speed"_ustr)
      .default_value(15.0f)
      .min(0.0f)
      .description("Launch speed, world units per second");
  b.add_input<decl::Bool>("High Arc"_ustr)
      .default_value(false)
      .description("Of the two arcs that reach at this speed, the steeper and slower one (a "
                   "mortar, a lob over a wall); off: the flatter, faster one");
  b.add_input<decl::Float>("Arc Height"_ustr)
      .default_value(2.0f)
      .min(0.0f)
      .description("How far the top of the arc rises above the higher of Start and Target");
  b.add_input<decl::Vector>("Gravity"_ustr)
      .default_value(float3(0.0f, 0.0f, -9.81f))
      .description("The pull that bends the throw, in world units per second squared");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "solve_by", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "gravity_source", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = Speed;
  node->custom2 = SceneGravity;
}

static void node_update(bNodeTree *tree, bNode *node)
{
  if (node->custom1 != Speed && node->custom1 != ArcHeight) {
    node->custom1 = Speed;
  }
  if (node->custom2 != SceneGravity && node->custom2 != CustomGravity) {
    node->custom2 = SceneGravity;
  }
  auto available = [&](const UString identifier, const bool value) {
    if (bNodeSocket *socket = node->input_by_identifier(identifier)) {
      bke::node_set_socket_availability(*tree, *socket, value);
    }
  };
  available("Speed"_ustr, node->custom1 == Speed);
  available("High Arc"_ustr, node->custom1 == Speed);
  available("Arc Height"_ustr, node->custom1 == ArcHeight);
  available("Gravity"_ustr, node->custom2 == CustomGravity);
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeAimForArc"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_aim_for_arc_cc

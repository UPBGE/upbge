/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Explode or Implode Impulse (2026-11-11; idname LogicNativeExplosionImpulse): one blast pushes
 * every physics object inside a sphere away from its center, or pulls them toward it (the
 * engine's LN_OpCode::ExplosionImpulse).
 */

#include "node_logic_util.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_explosion_impulse_cc {

namespace decl = blender::nodes::logic::decl;

/* custom1 holds two dropdowns as bits (the engine's LN_EXPLOSION_FALLOFF_LINEAR and
 * LN_EXPLOSION_IMPLODE): bit 0 the Falloff (RNA `falloff`), bit 1 the Direction (RNA
 * `direction`). */
enum Falloff { Constant = 0, Linear = 1 };
enum Direction { Explode = 0, Implode = 2 };
/* Center (custom2, RNA `center`; the engine's LN_EXPLOSION_CENTER_*). Nodes made before the
 * dropdown (2026-11-11) hold Position. */
enum Center { Position = 0, Object = 1 };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr)
      .description(
          "The blast happens at this object's position (where it is at that moment); empty: "
          "the object running this tree. The blast leaves this object alone");
  b.add_input<decl::Vector>("Offset"_ustr)
      .description(
          "Where the blast happens from the Object, in the object's own space: it turns and "
          "scales with the object, as a child placed there would. (0, 1.5, 0): 1.5 m along the "
          "object's Y axis; (0, 0, 0): at the object itself");
  b.add_input<decl::Vector>("Origin"_ustr)
      .description("Center of the blast in world space, e.g. a Raycast's Hit Point");
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(5.0f)
      .min(0.001f)
      .description(
          "Objects touching this sphere are reached; the falloff measures to their centers of "
          "mass");
  b.add_input<decl::Float>("Strength"_ustr)
      .default_value(10.0f)
      .description(
          "The push at the center: an impulse (each object's speed changes by Strength / its "
          "mass), or a speed in m/s with Ignore Mass. Negative reverses the direction");
  b.add_input<decl::Float>("Upward Lift"_ustr)
      .min(0.0f)
      .description(
          "Lifts the objects (in meters): Explode pushes from this far below the center, "
          "Implode pulls toward this far above it, so objects fly up instead of sliding along "
          "the ground");
  b.add_input<decl::Bool>("Ignore Mass"_ustr)
      .description("Strength is a speed in m/s: light and heavy objects fly alike");
  b.add_input<decl::Object>("Ignore Object"_ustr)
      .description(
          "One more object the blast leaves alone (with the Object center, that object is "
          "already left alone); empty: none");
  b.add_input<decl::List>("Ignore Objects"_ustr)
      .description(
          "Objects the blast leaves alone (neither pushed nor in Objects), e.g. your own squad or "
          "the parts of the exploding vehicle; anything in the list that is not an object is "
          "skipped");
  b.add_input<decl::String>("Property"_ustr)
      .description(std::string("Only objects with this game property; empty: every object. ") +
                   logic::game_property_filter_note)
      .custom_draw(logic::game_property_filter_field());
  b.add_input<decl::CollisionLayers>("Mask"_ustr)
      .default_value(1023)
      .min(0)
      .max(1023)
      .description(
          "Collision layers the blast reaches (for whole groups such as players or enemies, set "
          "once instead of a list)");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .description(
          "Draw the blast in the step it happens: the sphere, and at each object a red arrow of "
          "the speed it gave (1 m per m/s) and a green cross at its center of mass");
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::List>("Objects"_ustr)
      .description(
          "Every object inside the blast, nearest first (at most 256), pushed or not (static "
          "objects and characters too): loop over it to deal damage");
  b.add_output<decl::List>("Strengths"_ustr)
      .description(
          "The push each of Objects gets, in the same order: Strength, times the falloff from "
          "the center to its center of mass (Linear: none at Radius). Damage = a Strength "
          "times your factor");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "direction", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "center", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "falloff", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  node->custom1 &= Linear | Implode;
  if (node->custom2 < Position || node->custom2 > Object) {
    node->custom2 = Position;
  }
  if (bNodeSocket *object = node->input_by_identifier("Object"_ustr)) {
    bke::node_set_socket_availability(*ntree, *object, node->custom2 == Object);
  }
  if (bNodeSocket *offset = node->input_by_identifier("Offset"_ustr)) {
    bke::node_set_socket_availability(*ntree, *offset, node->custom2 == Object);
  }
  if (bNodeSocket *origin = node->input_by_identifier("Origin"_ustr)) {
    bke::node_set_socket_availability(*ntree, *origin, node->custom2 == Position);
  }
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = Linear;
  node->custom2 = Object;
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeExplosionImpulse"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_type_size(ntype, 170, 140, NODE_DEFAULT_MAX_WIDTH);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_explosion_impulse_cc

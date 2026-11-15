/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_apply_impulse_cc {

namespace decl = blender::nodes::logic::decl;

/* Point Mode (custom1, RNA `point_mode`; the engine's LN_ImpulseMode). Nodes made before
 * 2026-11-11 hold World Offset, as they worked. */
enum PointMode { WorldOffset = 0, WorldPoint = 1, LocalPoint = 2 };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object to push; empty: the object running this tree");
  b.add_input<decl::Vector>("Point"_ustr, "Attach"_ustr)
      .description(
          "Where the push lands. Local Point: on the object, in its own axes from its origin, so "
          "it turns with the object ((0, 0, 0) = the origin). World Point: a position in the "
          "world, e.g. a Raycast's Hit Point. World Offset: an offset from the origin along the "
          "world axes that does not turn with the object. A push away from the center of mass "
          "also spins the object");
  b.add_input<decl::Vector>("Impulse"_ustr)
      .description(
          "Direction and strength of the push. It changes the speed at once by Impulse / mass "
          "(an impulse of 4 on a 2 kg object adds 2 m/s)");
  b.add_input<decl::Bool>("Local"_ustr)
      .description("Impulse along the object's own axes instead of the world's");
  b.add_input<decl::Bool>("Ignore Mass"_ustr)
      .description(
          "Impulse is a speed change in m/s: light and heavy objects get the same speed");
  b.add_input<decl::Bool>("Visualize"_ustr)
      .description(
          "Draw the push in the step it happens: a red cross where it lands, a red arrow of the "
          "speed it gave (1 m per m/s) and a green cross at the center of mass");
  b.add_output<decl::Execution>("Done"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "point_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = LocalPoint;
}

static void node_update(bNodeTree * /*tree*/, bNode *node)
{
  if (node->custom1 < WorldOffset || node->custom1 > LocalPoint) {
    node->custom1 = WorldOffset;
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeApplyImpulse"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_apply_impulse_cc

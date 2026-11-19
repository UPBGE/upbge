/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Oscillate (2026-11-17): a value that swings back and forth with the game's time (a pickup that
 * bobs, a light that pulses, a platform that sways, a moon that orbits). Wave (custom1): Sine 0,
 * Triangle 1, Square 2, Sawtooth 3 (LN_OscillateWave). Value = Center + Amplitude x the wave;
 * Bob = Axis x Amplitude x the wave (an offset to add to a position); Orbit = a point Amplitude
 * away from the axis going round it once a cycle. Time: the steps' game time (Float::GameTime),
 * so every object with the node swings together unless Random Phase starts each at its own point.
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_oscillate_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Frequency"_ustr)
      .default_value(1.0f)
      .description("Cycles a second (0.5: one swing back and forth every two seconds)");
  b.add_input<decl::Float>("Amplitude"_ustr)
      .default_value(1.0f)
      .description("How far it swings from the Center (Orbit: the radius)");
  b.add_input<decl::Float>("Center"_ustr)
      .default_value(0.0f)
      .description("Value: the middle it swings around");
  b.add_input<decl::Vector>("Axis"_ustr)
      .default_value(float3(0.0f, 0.0f, 1.0f))
      .description("Bob: the direction it moves along. Orbit: the axis it goes round");
  b.add_input<decl::Float>("Phase"_ustr)
      .default_value(0.0f)
      .description("Where in the cycle it starts, in cycles (0.25: a quarter later; Sine then "
                   "starts at its top)");
  b.add_input<decl::Bool>("Random Phase"_ustr)
      .default_value(false)
      .description("Each object starts at its own point of the cycle (pickups that do not all "
                   "bob together); the same in every play with the same game seed");
  b.add_output<decl::Float>("Value"_ustr).description("Center + Amplitude x the wave");
  b.add_output<decl::Vector>("Bob"_ustr)
      .description("Axis x Amplitude x the wave: add it to a start position");
  b.add_output<decl::Vector>("Orbit"_ustr)
      .description("A point Amplitude away from the Axis, once round it a cycle (around Z: "
                   "starts at +X, turns towards +Y): add it to a center");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "wave", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeOscillate"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_oscillate_cc

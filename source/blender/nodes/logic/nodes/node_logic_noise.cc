/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Noise (2026-11-16): smooth random values, -1 to 1, that change gradually with a position or a
 * time (gradient noise, LN_NoiseValue in LN_MathOps.h). custom1 the dimensions (1 W, 2 X Y,
 * 3 Vector, 4 Vector and W, as Blender's Noise Texture), custom2 the octaves (1 to 8).
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_noise_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value"_ustr)
      .description("A smooth random number from -1 to 1: near places (or times) give near "
                   "numbers");
  b.add_output<decl::Vector>("Vector"_ustr)
      .description("Three such numbers, each from -1 to 1 (X is Value): a wobble, a wind, a "
                   "wandering direction");
  b.add_input<decl::Vector>("Vector"_ustr).description(
      "Where the noise is read: an object's position gives each place its own value");
  b.add_input<decl::Float>("W"_ustr).description(
      "The fourth coordinate, usually the time (link Elapsed Time): the noise moves along it");
  b.add_input<decl::Float>("Scale"_ustr)
      .default_value(1.0f)
      .description("How quickly it changes: 1 changes about once a unit (or a second), 5 five "
                   "times as quickly");
  b.add_input<decl::Float>("Roughness"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .description("How strong each finer octave is against the one before (0: only the "
                   "first; 1: all as strong)");
  b.add_input<decl::Int>("Seed"_ustr)
      .default_value(0)
      .description("Another number gives another pattern (the same for every object)");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "noise_dimensions", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "octaves", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const int dimensions = node->custom1;
  const auto show = [&](bNodeSocket *socket, const bool available) {
    if (socket != nullptr) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  show(node->input_by_identifier("Vector"_ustr), dimensions != 1);
  show(node->input_by_identifier("W"_ustr), dimensions == 1 || dimensions == 4);
  show(node->input_by_identifier("Roughness"_ustr), node->custom2 > 1);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 3;
  node->custom2 = 2;
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeNoise"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_noise_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Smooth Value (2026-11-16): a number or a vector that follows its Target over time, kept per
 * object. custom1 the type (0 Float, 1 Vector), custom2 the mode (0 Smooth, 1 Constant Speed,
 * 2 Smooth Damp: LN_SmoothMode, LN_MathOps.h). Each type has its own Target and output (the
 * Float ones keep the plain identifiers), as Random Value.
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_smooth_value_cc {
namespace decl = blender::nodes::logic::decl;

enum class SmoothType : int16_t { Float = 0, Vector = 1 };
enum class SmoothMode : int16_t { Smooth = 0, ConstantSpeed = 1, SmoothDamp = 2 };

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value"_ustr).description(
      "The value following Target; it starts on Target the first time it is read");
  b.add_output<decl::Vector>("Value"_ustr, "Value_Vector"_ustr)
      .description("The vector following Target; it starts on Target the first time it is read");
  b.add_input<decl::Float>("Target"_ustr).description("The value to follow");
  b.add_input<decl::Vector>("Target"_ustr, "Target_Vector"_ustr)
      .description("The vector to follow (a position, a direction, a color as numbers)");
  b.add_input<decl::Float>("Speed"_ustr)
      .default_value(5.0f)
      .min(0.0f)
      .description("Smooth: how fast it closes the gap (5 closes 99% of it in a second, 1 closes "
                   "63%); Constant Speed: how many units it moves a second");
  b.add_input<decl::Float>("Smooth Time"_ustr)
      .default_value(0.3f)
      .min(0.0f)
      .description("About how many seconds it takes to reach Target; it keeps its speed when "
                   "Target moves and never overshoots");
  b.add_input<decl::Float>("Max Speed"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("The fastest it may move, in units a second; 0: no limit");
  b.add_input<decl::Bool>("Reset"_ustr)
      .default_value(false)
      .description("Jump to Target now (a respawn, a teleport, a new camera)");
  b.add_input<decl::Bool>("Ignore Timescale"_ustr)
      .default_value(false)
      .description("Move in real seconds, also in slow motion and while the time scale is 0");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const SmoothType type = SmoothType(node->custom1);
  const SmoothMode mode = SmoothMode(node->custom2);
  const auto show = [&](bNodeSocket *socket, const bool available) {
    if (socket != nullptr) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  show(node->output_by_identifier("Value"_ustr), type == SmoothType::Float);
  show(node->output_by_identifier("Value_Vector"_ustr), type == SmoothType::Vector);
  show(node->input_by_identifier("Target"_ustr), type == SmoothType::Float);
  show(node->input_by_identifier("Target_Vector"_ustr), type == SmoothType::Vector);
  show(node->input_by_identifier("Speed"_ustr), mode != SmoothMode::SmoothDamp);
  show(node->input_by_identifier("Smooth Time"_ustr), mode == SmoothMode::SmoothDamp);
  show(node->input_by_identifier("Max Speed"_ustr), mode == SmoothMode::SmoothDamp);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = int16_t(SmoothType::Float);
  node->custom2 = int16_t(SmoothMode::Smooth);
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSmoothValue"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_smooth_value_cc

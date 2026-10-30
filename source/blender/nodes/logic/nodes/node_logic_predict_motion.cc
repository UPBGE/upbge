/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_predict_motion_cc {
namespace decl = blender::nodes::logic::decl;

enum SourceType { Object = 0, Position };

static void node_update(bNodeTree *tree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr)
      .description("Samples the source position; normally trigger this once per update");
  b.add_input<decl::Execution>("Reset"_ustr)
      .description("Clears motion history so the next sample becomes a new baseline");
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object to track; uses the logic-tree owner when empty");
  b.add_input<decl::Vector>("Position"_ustr)
      .description("World-space position to track");
  b.add_input<decl::Float>("Prediction Time"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .description("Seconds ahead used for the predicted position");
  b.add_input<decl::Float>("Smoothing Time"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("Velocity smoothing time constant in seconds; zero disables smoothing");

  b.add_output<decl::Execution>("Updated"_ustr)
      .description("Continues after a Flow sample attempt");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("Most recent valid sampled world position");
  b.add_output<decl::Vector>("Velocity"_ustr)
      .description("Measured world-space velocity in units per second");
  b.add_output<decl::Vector>("Direction"_ustr)
      .description("Normalized velocity direction, or zero while stationary");
  b.add_output<decl::Float>("Speed"_ustr)
      .description("Magnitude of the measured velocity in units per second");
  b.add_output<decl::Vector>("Predicted Position"_ustr)
      .description("Position extrapolated by velocity for Prediction Time seconds");
  b.add_output<decl::Condition>("Valid"_ustr)
      .description(
          "True after two finite timed samples when all motion and prediction outputs are finite");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "source_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree *tree, bNode *node)
{
  node->custom1 = Object;
  node_update(tree, node);
}

static void node_update(bNodeTree *tree, bNode *node)
{
  node->custom1 = node->custom1 == Position ? Position : Object;
  if (bNodeSocket *socket = node->input_by_identifier("Object"_ustr)) {
    bke::node_set_socket_availability(*tree, *socket, node->custom1 == Object);
  }
  if (bNodeSocket *socket = node->input_by_identifier("Position"_ustr)) {
    bke::node_set_socket_availability(*tree, *socket, node->custom1 == Position);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativePredictMotion"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_predict_motion_cc

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Set Paused (LogicNativeSetPaused, NOD_logic_merged.hh), each the
 * node it was before 2026-11-08: SetGamePaused, SetPhysicsPaused. They are not node types of their
 * own: logic_option_register keeps them for the merged node, which forwards its declaration,
 * buttons, init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_game_paused_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Bool>("Paused"_ustr)
      .default_value(true)
      .description("Pause or resume game-time logic and physics without changing the time scale");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues after the game-pause request is queued for this update");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetGamePaused"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_game_paused_cc

namespace blender::nodes::node_logic_set_physics_paused_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Bool>("Paused"_ustr)
      .default_value(true)
      .description("Pause or resume physics simulation while fixed game logic continues");
  b.add_output<decl::Execution>("Done"_ustr)
      .description("Continues after the physics-pause request is queued for this update");
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetPhysicsPaused"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_physics_paused_cc

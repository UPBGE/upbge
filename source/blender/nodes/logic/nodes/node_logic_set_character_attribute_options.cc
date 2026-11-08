/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Set Character Attribute (LogicNativeSetCharacterAttribute,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: SetCharacterGravity,
 * SetCharacterJumpSpeed, SetCharacterMaxJumps. They are not node types of their own:
 * logic_option_register keeps them for the merged node, which forwards its declaration, buttons,
 * init and update to the option it holds (node_logic_merged.cc).
 */

#include "node_logic_util.hh"

namespace blender::nodes::node_logic_set_character_gravity_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Character object to affect; defaults to the owner when unset");
  b.add_input<decl::Vector>("Gravity"_ustr)
      .default_value({0.0f, 0.0f, -9.8f})
      .description("Character gravity vector");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetCharacterGravity"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_character_gravity_cc

namespace blender::nodes::node_logic_set_character_jump_speed_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Character object to affect; defaults to the owner when unset");
  b.add_input<decl::Float>("Force"_ustr).default_value(0.0f).min(0.0f).description(
      "Jump force applied by the character jump action");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetCharacterJumpSpeed"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_character_jump_speed_cc

namespace blender::nodes::node_logic_set_character_max_jumps_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Character object to affect; defaults to the owner when unset");
  b.add_input<decl::Int>("Max Jumps"_ustr).default_value(1).min(0).description(
      "Maximum jumps before the character must land again");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);

}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype,
                       "LogicNativeSetCharacterMaxJumps"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_character_max_jumps_cc

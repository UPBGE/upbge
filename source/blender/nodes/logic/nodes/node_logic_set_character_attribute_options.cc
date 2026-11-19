/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The options of the merged node Set Character Attribute (LogicNativeSetCharacterAttribute,
 * NOD_logic_merged.hh), each the node it was before 2026-11-08: SetCharacterGravity,
 * SetCharacterJumpSpeed, SetCharacterMaxJumps; since 2026-11-17 also SetCharacterMaxSlope,
 * SetCharacterFallSpeed, SetCharacterStepHeight and SetCharacterCrouch (no former node). They
 * are not node types of their own:
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
      "Upward speed (m/s) a jump starts with");
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

/* 2026-11-17: Max Slope, Fall Speed, Step Height and Crouch, options with no former node (the
 * engine compiles each as Set Jump Force's op with its setting: LN_CHARACTER_SETTING_*). */

namespace blender::nodes::node_logic_set_character_max_slope_cc {
namespace decl = blender::nodes::decl;
namespace logic_decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<logic_decl::Execution>("Flow"_ustr);
  b.add_input<logic_decl::Object>("Object"_ustr).description(
      "Character object to affect; defaults to the owner when unset");
  b.add_input<decl::Float>("Max Slope"_ustr)
      .default_value(float(M_PI_4))
      .min(0.0f)
      .max(float(M_PI_2))
      .subtype(PROP_ANGLE)
      .description("The steepest slope it walks up; a steeper one is a wall it slides down "
                   "(On Wall). 0: no limit, as in the Physics panel");
  b.add_output<logic_decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<logic_decl::Execution>("Done"_ustr).available(false);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetCharacterMaxSlope"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_character_max_slope_cc

namespace blender::nodes::node_logic_set_character_fall_speed_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Character object to affect; defaults to the owner when unset");
  b.add_input<decl::Float>("Fall Speed"_ustr)
      .default_value(55.0f)
      .min(0.0f)
      .description("The fastest it falls (m/s): a small one glides (a parachute, a feather "
                   "fall)");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetCharacterFallSpeed"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_character_fall_speed_cc

namespace blender::nodes::node_logic_set_character_step_height_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Character object to affect; defaults to the owner when unset");
  b.add_input<decl::Float>("Step Height"_ustr)
      .default_value(0.3f)
      .min(0.0f)
      .description("The highest step it walks up (m), as stairs; it also stays on the ground "
                   "walking down steps as high. 0: no stairs");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetCharacterStepHeight"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_character_step_height_cc

namespace blender::nodes::node_logic_set_character_crouch_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr).description(
      "Character object to affect; defaults to the owner when unset");
  b.add_input<decl::Bool>("Crouch"_ustr)
      .default_value(true)
      .description("On: crouch (a shorter shape, its feet where they were). Off: stand up "
                   "again, as soon as the standing shape has room (not under a low ceiling)");
  b.add_input<decl::Float>("Height"_ustr)
      .default_value(0.5f)
      .min(0.05f)
      .max(1.0f)
      .description("The crouched height as a part of the standing height (0.5: half as tall). "
                   "A capsule keeps its width");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_option_type_base(&ntype, "LogicNativeSetCharacterCrouch"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;

  logic_option_register(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_character_crouch_cc

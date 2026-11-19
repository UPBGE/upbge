/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_get_character_info_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Object"_ustr).description(
      "Character object to read; defaults to the owner when unset");
  /* What the character touched at the end of its last step (2026-11-17: On Wall to Wall Normal
   * are new; the older outputs follow). */
  b.add_output<decl::Bool>("On Ground"_ustr)
      .description("Whether the character stands on ground it can walk on (not too steep)");
  b.add_output<decl::Bool>("On Wall"_ustr)
      .description("Whether it touches a wall: a surface too steep to stand on (steeper than its "
                   "Max Slope), at the end of its last step. Wall Normal points away from it");
  b.add_output<decl::Bool>("On Ceiling"_ustr)
      .description("Whether it touches a ceiling: a surface above it, facing down (it bumped its "
                   "head), at the end of its last step");
  b.add_output<decl::Bool>("Crouched"_ustr)
      .description("Whether it has its crouched shape (Set Character Attribute's Crouch). It "
                   "stays crouched under something low until the standing shape has room");
  b.add_output<decl::Vector>("Ground Normal"_ustr)
      .description("The direction the ground under it faces (world space) while On Ground; zero "
                   "in the air");
  b.add_output<decl::Vector>("Ground Velocity"_ustr)
      .description("How fast the ground moves where it stands (m/s, world space: a moving "
                   "platform carries it) while On Ground; zero in the air");
  b.add_output<decl::Object>("Ground Object"_ustr)
      .description("The object it stands on while On Ground; none in the air");
  b.add_output<decl::Vector>("Wall Normal"_ustr)
      .description("The direction away from the walls it touches (world space, length 1: push it "
                   "off along it for a wall jump); zero off walls, or between two facing walls");
  b.add_output<decl::Vector>("Walk Direction"_ustr)
      .description("The walking velocity (m/s) the character was given (Local: in its own "
                   "space)");
  b.add_output<decl::Vector>("Gravity"_ustr).description("Character gravity vector");
  b.add_output<decl::Int>("Current Jump Count"_ustr).description("Current consumed jumps");
  b.add_output<decl::Int>("Max Jumps"_ustr).description("Configured maximum jumps");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "use_local_space", UI_ITEM_NONE, "Local", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = 1;
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeGetCharacterInfo"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_get_character_info_cc
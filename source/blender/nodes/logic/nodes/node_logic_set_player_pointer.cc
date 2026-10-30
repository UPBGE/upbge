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

namespace blender::nodes::node_logic_set_player_pointer_cc {
namespace decl = blender::nodes::logic::decl;

enum PointerMode {
  Surface = 0,
  /** The middle of the player's view, like a crosshair. */
  Aim = 1,
};

static void node_update(bNodeTree *ntree, bNode *node);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("The player whose pointer it is. Surface: player 0's mouse moves it too, and "
                   "then clicks as usual");
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("On: the player points with it (instead of the other kind). Off: the "
                   "player's pointer of this kind turns off, its cursor hides and a drag in "
                   "progress is cancelled; a pointer of the other kind stays on");
  b.add_input<decl::Object>("Surface"_ustr)
      .description("The screen it moves on, such as a computer's monitor in the game world: a "
                   "flat object (a plane or a thin box). It moves on the side facing the "
                   "player's camera and reaches the UI Elements in front of it. Empty: the "
                   "tree's own object. Linked to nothing: the pointer turns off");
  b.add_input<decl::Object>("Cursor"_ustr)
      .description("Moved to where the pointer points, just in front of the Surface's children "
                   "(its buttons and labels), so every player sees it; shown while the pointer "
                   "is on, hidden when it is off. Make it a child of the Surface, facing out "
                   "like the buttons, with no collision. Empty: no cursor");
  b.add_input<decl::String>("Move Action"_ustr)
      .description("A vector action of the Game Input Map that moves it, such as Move. Empty: "
                   "the player's left stick");
  b.add_input<decl::Float>("Speed"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("Surface widths a second at full tilt; slower for a small tilt, to aim at "
                   "small buttons");
  b.add_input<decl::Float>("Reach"_ustr)
      .default_value(3.0f)
      .min(0.0f)
      .description("How far the player reaches from their camera: UI Elements farther away, or "
                   "behind a wall, are not hit. The objects the camera hangs from (the player's "
                   "body) never hide what they aim at");
  b.add_input<decl::String>("Press Action"_ustr)
      .description("The action of the Game Input Map that presses what the pointer is on, such "
                   "as Use. Empty: Accept. View Center: its press goes to the UI only while the "
                   "player aims at a UI Element, else the game gets it as usual");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "pointer_mode", UI_ITEM_NONE, "Points With", ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = Surface;
  node_update(ntree, node);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const bool aim = node->custom1 == Aim;
  for (const UString name : {"Surface"_ustr, "Cursor"_ustr, "Move Action"_ustr, "Speed"_ustr}) {
    if (bNodeSocket *socket = node->input_by_identifier(name)) {
      bke::node_set_socket_availability(*ntree, *socket, !aim);
    }
  }
  if (bNodeSocket *reach = node->input_by_identifier("Reach"_ustr)) {
    bke::node_set_socket_availability(*ntree, *reach, aim);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetPlayerPointer"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_player_pointer_cc

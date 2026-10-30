/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_screen_anchor_cc {
namespace decl = blender::nodes::logic::decl;
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object to place: its origin moves to the anchored point; rotation and "
                   "scale stay. Empty: this tree's object");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("Empty automatically uses the object's overlay (menu) camera, else the main "
                   "camera, or with Area Player's View that player's camera");
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("The player whose view the object is anchored in (split screen). Nothing "
                   "happens while that player has no view on screen");
  b.add_input<decl::Vector>("Offset Pixels"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .description("Moves the object from its anchored point, in pixels: X to the right, Y down "
                   "(Z is not used). With Scale Offset these are pixels at Reference Resolution");
  b.add_input<decl::Vector>("Safe Margin Pixels"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .description("X is horizontal margin and Y is vertical margin");
  b.add_input<decl::Float>("Depth"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .description("How far from the camera to place the object, in meters, along the line from "
                   "the camera through the anchored point. With an orthographic camera it is "
                   "measured from the camera's Clip Start");
  b.add_input<decl::Vector>("Reference Resolution"_ustr)
      .default_value({1920.0f, 1080.0f, 0.0f})
      .description("The screen size (X by Y pixels) Offset Pixels are written for: with Scale "
                   "Offset they grow or shrink to the real size. Z is not used; 0 in X or Y "
                   "leaves the offset unscaled");
  b.add_input<decl::Bool>("Scale Offset"_ustr)
      .default_value(true)
      .description("Scale Offset Pixels by the real width and height of the window (or player's "
                   "view) over Reference Resolution's, so the offset fits any screen size. Off: "
                   "plain pixels. Safe Margin Pixels are never scaled");
  b.add_output<decl::Execution>("Done"_ustr);
}
static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "screen_anchor", UI_ITEM_NONE, "Anchor", ICON_NONE);
  layout.prop(ptr, "screen_area", UI_ITEM_NONE, "Area", ICON_NONE);
}
/* custom2: 0 the window, 1 a player's view (split screen). */
static void node_update(bNodeTree *ntree, bNode *node)
{
  if (bNodeSocket *user = node->input_by_identifier("User"_ustr)) {
    bke::node_set_socket_availability(*ntree, *user, node->custom2 == 1);
  }
}
static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 4;
  node->custom2 = 0;
  node_update(ntree, node);
}
static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeScreenAnchor"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_screen_anchor_cc

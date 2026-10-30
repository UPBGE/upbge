/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include <algorithm>

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_object_scroll_view_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Viewport Object"_ustr)
      .description("Object bounds that define the visible and interactive viewport; empty uses the owner");
  b.add_input<decl::Object>("Content Object"_ustr)
      .description("Root object moved in local space; its descendants are clipped by the viewport");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("Camera used only when Interaction View is Custom Camera");
  b.add_input<decl::Vector>("Minimum Offset"_ustr)
      .default_value({0.0f, -10.0f, 0.0f})
      .description("The smallest Offset on each axis: how far the content may move from where it "
                   "started, in its local position. At Start and Normalized 0 are at this end, "
                   "unless Wheel Step is negative there");
  b.add_input<decl::Vector>("Maximum Offset"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .description("The largest Offset on each axis. Where Minimum and Maximum Offset are equal, "
                   "the content does not scroll on that axis");
  b.add_input<decl::Vector>("Wheel Step"_ustr)
      .default_value({0.0f, 0.25f, 0.0f})
      .description("Local-space content movement for one mouse-wheel step");
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("Off: the view stops and lets go: the content goes back to where it started, "
                   "objects it hid show again and the outputs reset. On again, it starts over "
                   "from there");
  b.add_input<decl::Bool>("Horizontal"_ustr)
      .default_value(false)
      .description("Scroll along the content's local X (left and right), between Minimum and "
                   "Maximum Offset X. Off: no scrolling along X");
  b.add_input<decl::Bool>("Vertical"_ustr)
      .default_value(true)
      .description("Scroll along the content's local Y (up and down), between Minimum and "
                   "Maximum Offset Y. Off: no scrolling along Y");
  b.add_input<decl::Bool>("Cull Outside"_ustr)
      .default_value(true)
      .description("Hide whole content objects whose origins are outside the viewport");
  b.add_input<decl::Bool>("Focus Follow"_ustr)
      .default_value(true)
      .description("Scroll as needed to keep a focused descendant inside the viewport");
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("The player (0-7) this view scrolls for: their Scroll Up/Down (right stick) "
                   "scrolls it and Focus Follow follows their selection. Only player 0's views "
                   "take the mouse wheel; in an open menu, it is the menu's player");
  b.add_input<decl::Int>("Priority"_ustr)
      .default_value(0)
      .description("When views move the same Content Object, or overlap under the pointer for "
                   "the wheel, the one in the topmost open menu wins, then the one with the "
                   "higher Priority");
  b.add_input<decl::Float>("Padding"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("Widens the viewport's edges by this much (in the Viewport Object's own "
                   "units) for what counts as inside: objects whose origin is that close outside "
                   "still show and take clicks, and the wheel still scrolls there");
  b.add_input<decl::Object>("Scrollbar"_ustr)
      .description("A bar whose first mesh child is its handle. The handle shows the scroll "
                   "position and how much is in view; drag it or click the bar to scroll. Both "
                   "hide while everything fits");

  b.add_output<decl::Execution>("On Scroll"_ustr);
  b.add_output<decl::Condition>("At Start"_ustr);
  b.add_output<decl::Condition>("At End"_ustr);
  b.add_output<decl::Vector>("Offset"_ustr);
  b.add_output<decl::Vector>("Normalized"_ustr)
      .description("Zero-to-one progress on each enabled axis");
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "interaction_view", UI_ITEM_NONE, "Interaction View", ICON_NONE);
}

static void node_init(bNodeTree *, bNode *node)
{
  node->custom1 = 0;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  node->custom1 = std::clamp<int>(node->custom1, 0, 3);
  if (bNodeSocket *camera = node->input_by_identifier("Camera"_ustr)) {
    bke::node_set_socket_availability(*ntree, *camera, node->custom1 == 3);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeObjectScrollView"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_object_scroll_view_cc

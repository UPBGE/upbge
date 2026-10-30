/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_set_camera_viewport_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Camera"_ustr)
      .description("The camera to draw in this part of the window (a minimap, a mirror). "
                   "Empty: the active camera. A camera shown as a split screen player's view "
                   "keeps that view");
  b.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("Off: the camera stops drawing there");
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("The player whose view it is drawn in (a minimap per player). Not drawn while "
                   "that player has no view on screen");
  b.add_input<decl::Float>("Left"_ustr)
      .default_value(0.7f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("From the area's left edge, as a part of its width (0 to 1)");
  b.add_input<decl::Float>("Top"_ustr)
      .default_value(0.05f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("From the area's top edge, as a part of its height (0 to 1)");
  b.add_input<decl::Float>("Width"_ustr)
      .default_value(0.25f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("As a part of the area's width (0 to 1)");
  b.add_input<decl::Float>("Height"_ustr)
      .default_value(0.25f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("As a part of the area's height (0 to 1)");
  b.add_input<decl::Bool>("On Top"_ustr)
      .default_value(true)
      .description("Draw it over the other views, such as split screen views");
  b.add_output<decl::Execution>("Done"_ustr, "Out"_ustr);
  b.add_output<decl::Execution>("Done"_ustr).available(false);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "viewport_area", UI_ITEM_NONE, "Area", ICON_NONE);
}

/* custom1: 0 the window, 1 a player's view (split screen). */
static void node_update(bNodeTree *ntree, bNode *node)
{
  if (bNodeSocket *user = node->input_by_identifier("User"_ustr)) {
    bke::node_set_socket_availability(*ntree, *user, node->custom1 == 1);
  }
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = 0;
  node_update(ntree, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeSetCameraViewport"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_set_camera_viewport_cc

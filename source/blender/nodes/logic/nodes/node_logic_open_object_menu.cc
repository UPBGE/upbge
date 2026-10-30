/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BKE_node.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_open_object_menu_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Execution>("Flow"_ustr).description("Incoming execution pulse");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("Dedicated inactive overlay camera; empty reuses the established overlay camera");
  b.add_input<decl::Collection>("Collection"_ustr)
      .description("Same-scene collection containing the object menu");
  b.add_input<decl::String>("Input Context"_ustr)
      .description("Empty inherits the active context when the open transaction commits");
  b.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("The player whose menu it is: its UI Elements and Grid Views are that "
                   "player's, and that player's Back closes it (0 has the keyboard and mouse; "
                   "gamepads: Scene > Game Input Map). When another player has this collection "
                   "open, this player gets a copy of their own. In a menu's own logic: that "
                   "menu's player");
  b.add_input<decl::Object>("Owner"_ustr)
      .description("Whose menu it is, such as that player's character: Get Menu Player gives it "
                   "to the menu's logic, and the menu's widgets and {property} texts read its "
                   "properties. Empty: the object whose logic opens it (from inside another "
                   "menu, that menu's owner)");
  b.add_output<decl::Execution>("Opened"_ustr)
      .description("Runs after the runtime confirms that the menu is open");
  b.add_output<decl::Execution>("Failed"_ustr)
      .description("Runs after the runtime rejects or fails the request");
  b.add_output<decl::Execution>("Closed"_ustr)
      .description("Toggle: runs when the menu was open, so this closed it");
  b.add_output<decl::Int>("Menu"_ustr)
      .description("Stable session handle used to close this exact menu instance");
  b.add_output<decl::Int>("Result"_ustr)
      .description("Stable result code for the completed operation; zero is Success");
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "menu_open_mode", UI_ITEM_NONE, "Open Mode", ICON_NONE);
  layout.prop(ptr, "menu_presentation", UI_ITEM_NONE, "Presentation", ICON_NONE);
  layout.prop(ptr, "menu_routing", UI_ITEM_NONE, "Routing", ICON_NONE);
  layout.prop(ptr, "menu_back_behavior", UI_ITEM_NONE, "Back", ICON_NONE);
  layout.prop(ptr, "menu_area", UI_ITEM_NONE, "Area", ICON_NONE);
  layout.prop(ptr, "use_pause_game", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_toggle", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

/* The custom2 bits of Pause Game and Toggle (LN_UI_MENU_NODE_* in the game engine). */
static constexpr int16_t NODE_TOGGLES = 1 << 7;

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (bNodeSocket *closed = node->output_by_identifier("Closed"_ustr)) {
    bke::node_set_socket_availability(*ntree, *closed, (node->custom2 & NODE_TOGGLES) != 0);
  }
}

static void node_init(bNodeTree *, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeOpenObjectMenu"_ustr);
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_open_object_menu_cc

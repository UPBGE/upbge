/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include "BLT_translation.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_on_ui_event_cc {
namespace decl = blender::nodes::logic::decl;
/* The ui_event enum values in rna_nodetree.cc. */
static constexpr int16_t EVENT_DROP_RECEIVED = 12;
static constexpr int16_t EVENT_DROP_ENTER = 16;
static constexpr int16_t EVENT_DROP_EXIT = 17;
static constexpr int16_t EVENT_REFUSED_START = 20;
static constexpr int16_t EVENT_REFUSED_END = 21;
/* Drop Received names the dropped element; Drop Enter, Drop Exit and Refused Start/End the
 * dragged one. */
static const char *target_label(const bNode &node, const char *target, const char *dropped,
                                const char *dragged)
{
  if (node.custom1 == EVENT_DROP_RECEIVED) {
    return IFACE_(dropped);
  }
  if (ELEM(node.custom1, EVENT_DROP_ENTER, EVENT_DROP_EXIT, EVENT_REFUSED_START, EVENT_REFUSED_END))
  {
    return IFACE_(dragged);
  }
  return IFACE_(target);
}
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Action"_ustr)
      .description("Only UI Elements with this Action name, or with one of several names "
                   "separated by commas (\"weapon, armor\"); empty reacts to every UI Element");
  b.add_output<decl::Execution>("Out"_ustr)
      .description("Runs when the selected event happens to a matching UI Element: once for each "
                   "such event of a frame (two players, overlapping elements), each time with its "
                   "own outputs; the topmost element's event runs last. A tree in a player's menu "
                   "hears only that player's events (split screen)");
  b.add_output<decl::Object>("Object"_ustr)
      .description("The UI Element the event happened to; Highlight: empty when the highlight "
                   "left the elements of this Action");
  b.add_output<decl::String>("Action"_ustr)
      .description("Action name of that UI Element, ready for a Switch Flow (String)");
  b.add_output<decl::Object>("Target"_ustr)
      .description("Drop: the UI Element it was dropped on, else the game-world object under "
                   "the pointer; empty when dropped on other UI. Drop Received: the UI Element "
                   "that was dropped. Drop Enter, Drop Exit: the one being dragged")
      .label_fn([](bNode node) { return target_label(node, "Target", "Dropped", "Dragged"); });
  b.add_output<decl::Vector>("Point"_ustr)
      .description("World-space point: the hit point, the dragged position, or where it was "
                   "dropped; where the stack landed when a Grid View dropped it into the world");
  b.add_output<decl::Vector>("Normal"_ustr)
      .description("World-space surface normal at Point: of the UI Element, or of the "
                   "game-world surface a Drop Outside landed on (to place or align what it "
                   "spawns)");
  b.add_output<decl::Generic>("Item"_ustr)
      .description("The grid item the UI Element shows (an inventory slot, a recipe line...); "
                   "empty when it is not a Grid View slot. Changed: the widget's new value. Item "
                   "Shown: what the slot shows now (empty when it emptied)");
  b.add_output<decl::Int>("Index"_ustr)
      .description("That item's index in the Grid View's list, ready for the Inventory node's "
                   "Slot; -1 when it is not a Grid View slot");
  b.add_output<decl::Int>("Amount"_ustr)
      .description("How many of the item: what a drag carries (Drag Start to Drop, Drop "
                   "Received, Drop Enter, Drop Exit: of the dragged item), part of the stack "
                   "while the grid's Split Action is held, else all of it; other events: how many "
                   "the element's item has (0: no item)");
  b.add_output<decl::Generic>("Target Item"_ustr)
      .description("The grid item the target shows (Drop: the slot it was dropped on; Drop "
                   "Received: the dropped slot; Drop Enter, Drop Exit: the dragged slot)")
      .label_fn([](bNode node) {
        return target_label(node, "Target Item", "Dropped Item", "Dragged Item");
      });
  b.add_output<decl::Int>("Target Index"_ustr)
      .description("That item's index in its Grid View's list; -1 when the target is not a Grid "
                   "View slot")
      .label_fn([](bNode node) {
        return target_label(node, "Target Index", "Dropped Index", "Dragged Index");
      });
  b.add_output<decl::String>("Target Action"_ustr)
      .description("The target's Action when it is a UI Element, so one tree tells grids apart "
                   "(Drop: where it was dropped, such as \"chest\"; Drop Received: where it came "
                   "from); empty otherwise")
      .label_fn([](bNode node) {
        return target_label(node, "Target Action", "Dropped Action", "Dragged Action");
      });
  b.add_output<decl::Int>("User"_ustr)
      .description("The player it happened for: the player whose menu holds the UI Element "
                   "(Open Object Menu's User); outside menus the player who used it (an Any "
                   "Player element: each player, also two in the same frame)");
  b.add_output<decl::Condition>("Accepted"_ustr)
      .description("Drop Enter, Drop Exit: this UI Element takes the dragged item (its Accept "
                   "Categories, and the dragged slot's when they would swap), so a preview can be "
                   "green or red. Drop: a UI Element took it. Drop Received: always");
  b.add_output<decl::String>("Outcome"_ustr)
      .description("What a Grid View with Handle Moves did with the event: \"Moved\", "
                   "\"Stacked\", \"Combined\", \"Swapped\" or \"No Room\" (Drop, Drop Received), "
                   "\"Dropped\" (Drop Outside, Drop, Secondary Click); empty when it did nothing");
}
static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "ui_event", UI_ITEM_NONE, "", ICON_NONE);
}
static void node_init(bNodeTree *, bNode *node)
{
  node->custom1 = 0;
}
static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeOnUIEvent"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_on_ui_event_cc

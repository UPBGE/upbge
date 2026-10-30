/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_logic_util.hh"

#include <algorithm>

#include "BKE_node.hh"
#include "BLT_translation.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_object_grid_view_cc {
namespace decl = blender::nodes::logic::decl;

static void node_declare(NodeDeclarationBuilder &b)
{
  /* Outputs, settings, inputs, then the folded sections. */
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("On Scroll"_ustr);
  b.add_output<decl::Execution>("On Rebind"_ustr)
      .description("Runs when visible slot-to-item assignments change");
  b.add_output<decl::Condition>("At Start"_ustr);
  b.add_output<decl::Condition>("At End"_ustr);
  b.add_output<decl::Int>("First Visible"_ustr);
  b.add_output<decl::Int>("Last Visible"_ustr);
  b.add_output<decl::Int>("Page"_ustr)
      .description("The page in view, counting from 1: a page is as many rows as the slots show; "
                   "at the end the last page is in view. Ready for a \"2 / 5\" label");
  b.add_output<decl::Int>("Page Count"_ustr)
      .description("How many pages the items fill; 1 when they all fit");
  b.add_output<decl::Vector>("Normalized"_ustr)
      .description("X is zero-to-one progress through the item list");
  b.add_default_layout();

  b.add_input<decl::Object>("Viewport Object"_ustr)
      .description("Object bounds that define the inventory viewport; empty uses the owner");
  b.add_input<decl::Object>("Camera"_ustr)
      .description("Camera used only when Interaction View is Custom Camera");
  b.add_input<decl::Generic>("Slots"_ustr)
      .description("Fixed list of authored slot objects reused for visible items, in item order: "
                   "row by row, or column by column when scrolling horizontally");
  b.add_input<decl::Object>("Object"_ustr)
      .description("The object whose game property (Property) holds the list. Empty: whose menu "
                   "this tree's object is in (Open Object Menu's Owner, the player), else this "
                   "tree's object");
  b.add_input<decl::String>("Property"_ustr)
      .description("The game property with the list (a List or Table), such as \"inventory\": "
                   "the grid shows it and, with Moving Items on, changes it itself by the list's "
                   "Inventory Rules (and its Item Library gives the icons). Empty: Items gives "
                   "the list");
  b.add_input<decl::Generic>("Items"_ustr)
      .description("The list to show while Property is empty; each visible slot receives one "
                   "stable item binding. Its icons come from the scene's Item Library");
  b.add_input<decl::Int>("Columns"_ustr)
      .default_value(1)
      .min(1)
      .description("How many slots make one row: the grid scrolls by whole rows of this many "
                   "items. Scrolling horizontally, how many columns the slots make. With Slot "
                   "Template, the copies' columns");
  b.add_input<decl::Int>("Wheel Rows"_ustr)
      .default_value(1)
      .min(1)
      .description("Rows (columns when scrolling horizontally) moved by one wheel notch")
      .label_fn([](bNode node) {
        return node.custom2 == 1 ? IFACE_("Wheel Columns") : IFACE_("Wheel Rows");
      });
  b.add_input<decl::Bool>("Show Counts"_ustr)
      .default_value(true)
      .description("Each slot's first text object shows its item's count when it is above 1");
  b.add_input<decl::Bool>("Show Names"_ustr)
      .default_value(false)
      .description("Each slot's first text object shows its item's name (the library object's "
                   "\"name\" property, else the item itself: a quest line, a dialogue choice); "
                   "with Show Counts, \" x3\" follows when it holds more than one");
  b.add_input<decl::Bool>("Show Empty"_ustr)
      .default_value(false)
      .description("Cells past the last item stay as empty slots, with nothing to drag: a drop "
                   "on one puts the stack at the end (also with a gamepad, into an empty "
                   "inventory); Click and Highlight there report no item");
  b.add_input<decl::Object>("Slot Template"_ustr)
      .description("With Rows above 0: the first slot, copied Rows × Columns times instead of "
                   "using Slots");
  b.add_input<decl::Int>("Rows"_ustr)
      .default_value(0)
      .min(0)
      .description("Rows of template copies. Scrolling horizontally without a template, 0 "
                   "means the slots divided by Columns");
  b.add_input<decl::Vector>("Spacing"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .description("Distance between copies along the template's X and Y; zero leaves a small "
                   "gap after the template's size");
  b.add_input<decl::Object>("Scrollbar"_ustr)
      .description("A bar whose first mesh child is its handle. The handle shows the scroll "
                   "position and how much is in view; drag it or click the bar to scroll. Both "
                   "hide while every item fits");

  PanelDeclarationBuilder &inventory = b.add_panel("Moving Items"_ustr).default_closed(true);
  inventory.add_input<decl::Bool>("Handle Moves"_ustr)
      .default_value(false)
      .panel_toggle()
      .description("The grid changes the list named by Object and Property itself, as the "
                   "Inventory node's Move To Slot does: an item dropped on another slot moves, "
                   "stacks, swaps or makes a recipe, also between grids of two objects (a chest "
                   "and a bag) when both grids handle moves. How it stacks, its Capacity, Keep "
                   "Places and Recipes are the list's Inventory Rules (its game property). On UI "
                   "Event still runs: its Outcome says what happened, so no route needs to move "
                   "the item");
  inventory.add_input<decl::String>("Split Action"_ustr)
      .description("A Game Input Map action (such as Ctrl): held while a stack is picked up "
                   "(dragged, Grab, Click to Pick Up) or dropped with Secondary Click, only Split "
                   "Amount of it goes; the rest stays in its slot. A part joins the same item, "
                   "fills an empty cell or makes a new stack, but never swaps. Empty: whole "
                   "stacks");
  inventory.add_input<decl::Int>("Split Amount"_ustr)
      .default_value(0)
      .min(0)
      .description("How many of a stack Split Action takes: 0 takes half (rounded up), else this "
                   "many (all of a smaller stack)");
  inventory.add_input<decl::Bool>("Swap"_ustr)
      .default_value(true)
      .description("Two different items change places (else the drop does nothing)");
  inventory.add_input<decl::Bool>("Drop Into World"_ustr)
      .default_value(false)
      .description("A stack dragged out of the menu and let go over the player's game view "
                   "drops into the world: a copy of its Item Library object, as Drop Item makes, "
                   "in front of the list's object (or under the pointer with At Pointer). Let "
                   "go over other UI, a minimap or another player's view, it goes back");
  inventory.add_input<decl::Bool>("At Pointer"_ustr)
      .default_value(false)
      .description("Drop Into World puts the stack on the floor below the pointer, wherever "
                   "the player points in the game world (letting go over nothing puts it back). "
                   "Off: it lands in front of the list's object, as Drop In Front does");
  inventory.add_input<decl::Bool>("Drop In Front"_ustr)
      .default_value(false)
      .description("Secondary Click (right click, or the gamepad's Secondary) on a stack drops "
                   "it in front of the list's object, on the floor and not through walls");
  inventory.add_input<decl::Float>("Distance"_ustr)
      .default_value(1.5f)
      .min(0.0f)
      .description("How far in front of the list's object drops land (Drop In Front, and Drop "
                   "Into World without At Pointer); closer when a wall is in the way, and half a "
                   "meter aside when a pickup already lies there");
  inventory.add_input<decl::Float>("Hover Height"_ustr)
      .default_value(0.0f)
      .description("Drops float this high above where they land (0 rests an item whose origin "
                   "is at its base on the floor)");

  PanelDeclarationBuilder &hints = b.add_panel("Drop Hints"_ustr).default_closed(true);
  hints.add_input<decl::Object>("Hint Text"_ustr)
      .description("A text object that says what letting go over one of this grid's slots "
                   "would do while an item is dragged there, emptied when the drag leaves; also "
                   "without Handle Moves");
  hints.add_input<decl::String>("Accept Hint"_ustr)
      .default_value("Put {name} here")
      .description("When the slot takes the dragged item. {name}, {count}, {description} and "
                   "other game properties of the item's library object are filled in");
  hints.add_input<decl::String>("Refuse Hint"_ustr)
      .default_value("{name} does not fit here")
      .description("When the slot's Accept Categories, or the Accept Categories of the list's "
                   "Inventory Rules, refuse it (it goes back)");

  PanelDeclarationBuilder &filter = b.add_panel("Filter"_ustr).default_closed(true);
  filter.add_input<decl::String>("Show Categories"_ustr)
      .description("Only items of these categories (comma separated, as Accept Categories: "
                   "the item's own \"category\", else its Item Library object's), for tabs. "
                   "The slots still are the list's own: moves, Use and On UI Event's Index "
                   "work as without it. Empty: every item");
  filter.add_input<decl::String>("Search"_ustr)
      .description("Only items whose name contains this text (any case), for a search field "
                   "(a Text widget's property). Empty: every item");

  PanelDeclarationBuilder &use = b.add_panel("Use"_ustr).default_closed(true);
  use.add_input<decl::String>("Use Action"_ustr)
      .description("A Game Input Map action: pressed while a slot holding an item is the "
                   "player's highlight (under the mouse, or selected), On UI Event Use runs for "
                   "that slot. Empty: none");
  use.add_input<decl::String>("Use Keys"_ustr)
      .description("Game Input Map actions, comma separated (such as \"Quick 1, Quick 2\"): the "
                   "first uses the item in the grid's first slot, the second the second... For a "
                   "hotbar in a HUD, while the player has no menu open (or this grid's menu)");

  PanelDeclarationBuilder &advanced = b.add_panel("Advanced"_ustr).default_closed(true);
  advanced.add_input<decl::Int>("First Item"_ustr)
      .default_value(0)
      .min(0)
      .description("The item (counting from 0) the first slot shows when the grid starts, and "
                   "again whenever this value changes; scrolling is free in between. It snaps to "
                   "the start of its row (column when scrolling horizontally) and stops where the "
                   "last item shows. With a Filter, it counts the shown items");
  advanced.add_input<decl::Bool>("Enabled"_ustr)
      .default_value(true)
      .description("Off: the grid stops and lets go: its slots show again as they last looked "
                   "but hold no item, Slot Template copies are removed and the outputs reset. On "
                   "again, it starts over at First Item");
  advanced.add_input<decl::Bool>("Cull Outside"_ustr)
      .default_value(true)
      .description("Hide each slot, with its icon and text, whose origin is outside the "
                   "viewport. Off: such slots still show, but outside the viewport they take no "
                   "clicks");
  advanced.add_input<decl::Bool>("Focus Follow"_ustr)
      .default_value(true)
      .description("Keep keyboard/gamepad focus on a bound visible slot while scrolling");
  advanced.add_input<decl::Int>("User"_ustr)
      .default_value(0)
      .min(0)
      .max(7)
      .description("The player (0-7) this grid scrolls for: their Scroll Up/Down (right stick) "
                   "scrolls it, and Focus Follow follows their selection. Only player 0's grids "
                   "take the mouse wheel; in an open menu, it is the menu's player");
  advanced.add_input<decl::Int>("Priority"_ustr)
      .default_value(0)
      .description("When grids use the same slot, or overlap under the pointer for the wheel, "
                   "the one in the topmost open menu wins, then the one with the higher "
                   "Priority");
  advanced.add_input<decl::Float>("Padding"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .description("Widens the viewport's edges by this much (in the Viewport Object's own "
                   "units) for what counts as inside: slots whose origin is that close outside "
                   "still show and take clicks, and the wheel still scrolls there");
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "interaction_view", UI_ITEM_NONE, "Interaction View", ICON_NONE);
  layout.prop(ptr, "scroll_direction", UI_ITEM_NONE, "Scroll", ICON_NONE);
}

static void node_init(bNodeTree *, bNode *node)
{
  node->custom1 = 0;
  node->custom2 = 0;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  node->custom1 = std::clamp<int>(node->custom1, 0, 3);
  node->custom2 = std::clamp<int>(node->custom2, 0, 1);
  if (bNodeSocket *camera = node->input_by_identifier("Camera"_ustr)) {
    bke::node_set_socket_availability(*ntree, *camera, node->custom1 == 3);
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  logic_node_type_base(&ntype, "LogicNativeObjectGridView"_ustr);
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_logic_object_grid_view_cc

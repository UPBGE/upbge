/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_util.hh"

#include <algorithm>

#include "BKE_node.hh"
#include "BLI_string_utf8.h"
#include "BLT_translation.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_logic_inventory_cc {
namespace decl = blender::nodes::logic::decl;

/* Matches the "operation" enum in rna_nodetree.cc and LN_InventoryOperation in the engine. */
static constexpr const char *operation_names[] = {"Add Item",
                                                  "Remove Item",
                                                  "Remove From Slot",
                                                  "Swap Slots",
                                                  "Split Stack",
                                                  "Sort Inventory",
                                                  "Move To Slot",
                                                  "Craft"};
enum Operation { ADD = 0, REMOVE_ITEM, REMOVE_FROM_SLOT, SWAP, SPLIT, SORT, MOVE_TO_SLOT, CRAFT };

static void node_declare(NodeDeclarationBuilder &b)
{
  /* Outputs, the operation, inputs, then the folded Override section. */
  b.use_custom_socket_order();
  b.add_output<decl::Execution>("Done"_ustr);
  b.add_output<decl::List>("Inventory"_ustr).description("The inventory after the change");
  b.add_output<decl::Int>("Leftover"_ustr).description("How many did not fit");
  b.add_output<decl::Int>("Removed"_ustr)
      .description("How many were taken. Craft: how many times it made the recipe. Add Item: "
                   "how many went in, such as a pickup message's Count")
      .label_fn([](bNode node) {
        return node.custom1 == CRAFT ? IFACE_("Crafted") :
               node.custom1 == ADD   ? IFACE_("Added") :
                                       IFACE_("Removed");
      });
  b.add_output<decl::String>("Made"_ustr)
      .description("The main item a recipe made; empty when none was made");
  b.add_output<decl::String>("Outcome"_ustr)
      .description("What happened: Moved, Stacked, Combined, Swapped (Move To Slot), Crafted "
                   "(Craft, also when it made fewer times than asked), No Room, Missing, or "
                   "Refused (the list's Inventory Rules do not accept the item); empty when "
                   "nothing changed, or Add Item put all of it in");
  b.add_default_layout();

  b.add_input<decl::Execution>("Flow"_ustr);
  b.add_input<decl::Object>("Object"_ustr)
      .description("Object with the inventory game property; empty uses this tree's object");
  b.add_input<decl::String>("Property"_ustr)
      .default_value("inventory")
      .description("The inventory game property (a Table or List of {item, count} slots): the "
                   "node changes it and writes it back at once");
  b.add_input<decl::String>("Item"_ustr)
      .description("Item name, an inventory slot, or an object: a pickup's item and count "
                   "properties");
  b.add_input<decl::Int>("Amount"_ustr)
      .default_value(1)
      .min(0)
      .description("How many; 0 means all: the whole slot, every one of the item, or the count "
                   "the added slot or pickup carries. Split Stack: how many go to the new slot "
                   "(at least 1, fewer than the slot holds). Craft: how many times, like crafting "
                   "one at a time; 0 makes as many as the ingredients allow")
      .label_fn([](bNode node) {
        return node.custom1 == CRAFT ? IFACE_("Times") : IFACE_("Amount");
      });
  b.add_input<decl::Int>("Slot"_ustr)
      .default_value(0)
      .min(0)
      .description("Index of the slot. Move To Slot: the dragged slot")
      .label_fn([](bNode node) {
        return node.custom1 == MOVE_TO_SLOT ? IFACE_("From Slot") : IFACE_("Slot");
      });
  b.add_input<decl::Int>("Other Slot"_ustr)
      .default_value(1)
      .min(0)
      .description("Index of the second slot. Move To Slot: the slot it was dropped on")
      .label_fn([](bNode node) {
        return node.custom1 == MOVE_TO_SLOT ? IFACE_("To Slot") : IFACE_("Other Slot");
      });
  b.add_input<decl::Bool>("By Count"_ustr)
      .default_value(false)
      .description("Sort by amount instead of item name");
  b.add_input<decl::Bool>("Descending"_ustr)
      .default_value(false)
      .description("Sort from the last to the first: Z to A, or the biggest amounts first");
  b.add_input<decl::String>("Recipe"_ustr)
      .description("One recipe line, such as \"2 Wood + Stone = Axe\" or \"Milk Bucket = Butter "
                   "+ Bucket\"");
  b.add_input<decl::Bool>("Swap"_ustr)
      .default_value(true)
      .description("Two items that neither stack nor make anything change places");
  b.add_input<decl::Object>("To Object"_ustr)
      .description("Move To Slot into another inventory (a chest, an equipment slot): the object "
                   "with it; empty uses Object");
  b.add_input<decl::String>("To Property"_ustr)
      .description("Move To Slot into another inventory: its game property; empty moves inside "
                   "this inventory. To Slot past its end (an empty place) puts the stack there; "
                   "recipes combine only inside one inventory");

  /* The list's own Inventory Rules (its game property) decide these, unless this call
   * overrides them. */
  PanelDeclarationBuilder &override_panel = b.add_panel("Override"_ustr).default_closed(true);
  override_panel.add_input<decl::Bool>("Override"_ustr)
      .default_value(false)
      .panel_toggle()
      .description("Use this node's own Max Stack, Stack, Capacity, Recipes, Library and Keep "
                   "Places for this call instead of the Inventory Rules of the list it changes "
                   "(Inventory Rules of its List or Table game property; none: the defaults). "
                   "The list's Accept Categories still apply");
  override_panel.add_input<decl::Int>("Max Stack"_ustr)
      .default_value(0)
      .min(0)
      .description("With Override: most items in one slot when the item's library object has no "
                   "max_stack property; 0 means no limit");
  override_panel.add_input<decl::Bool>("Stack"_ustr)
      .default_value(true)
      .description("With Override: put the same items in one slot; off gives every item its own "
                   "slot");
  override_panel.add_input<decl::Int>("Capacity"_ustr)
      .default_value(0)
      .min(0)
      .description("With Override: most slots in the inventory; 0 means no limit (up to 65536 "
                   "slots)");
  override_panel.add_input<decl::List>("Recipes"_ustr)
      .description("With Override: recipe lines such as \"Apple + Mushroom = Potion\": dropping "
                   "one of two items on the other makes the result");
  override_panel.add_input<decl::Collection>("Library"_ustr)
      .description("With Override: item library, where an item's max_stack property is its "
                   "stack limit (instead of Max Stack); empty uses the scene's Item Library");
  override_panel.add_input<decl::Bool>("Keep Places"_ustr)
      .default_value(false)
      .description("With Override: an emptied slot stays an empty place, so the slots after it "
                   "keep their places (an inventory players arrange, with gaps). Off: the slots "
                   "after it move up. Add Item always fills empty places first");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const int op = std::clamp<int>(node->custom1, ADD, CRAFT);
  const auto show_input = [&](const StringRef name, const bool available) {
    if (bNodeSocket *socket = node->input_by_identifier(UString{name})) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  const auto show_output = [&](const StringRef name, const bool available) {
    if (bNodeSocket *socket = node->output_by_identifier(UString{name})) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
  };
  show_input("Item", ELEM(op, ADD, REMOVE_ITEM));
  show_input("Amount", ELEM(op, ADD, REMOVE_ITEM, REMOVE_FROM_SLOT, SPLIT, CRAFT));
  /* Swap Slots follows no rule: no Override section. */
  show_input("Override", op != SWAP);
  show_input("Max Stack", ELEM(op, ADD, MOVE_TO_SLOT, CRAFT));
  show_input("Stack", ELEM(op, ADD, MOVE_TO_SLOT, CRAFT));
  show_input("Capacity", ELEM(op, ADD, SPLIT, MOVE_TO_SLOT, CRAFT));
  show_input("Slot", ELEM(op, REMOVE_FROM_SLOT, SWAP, SPLIT, MOVE_TO_SLOT));
  show_input("Other Slot", ELEM(op, SWAP, MOVE_TO_SLOT));
  show_input("By Count", op == SORT);
  show_input("Descending", op == SORT);
  show_input("Recipes", op == MOVE_TO_SLOT);
  show_input("To Object", op == MOVE_TO_SLOT);
  show_input("To Property", op == MOVE_TO_SLOT);
  show_input("Recipe", op == CRAFT);
  show_input("Library", ELEM(op, ADD, MOVE_TO_SLOT, CRAFT));
  show_input("Swap", op == MOVE_TO_SLOT);
  show_input("Keep Places", ELEM(op, REMOVE_ITEM, REMOVE_FROM_SLOT, SPLIT, SORT, MOVE_TO_SLOT, CRAFT));
  show_output("Leftover", op == ADD);
  show_output("Removed", ELEM(op, ADD, REMOVE_ITEM, REMOVE_FROM_SLOT, CRAFT));
  show_output("Made", ELEM(op, MOVE_TO_SLOT, CRAFT));
  show_output("Outcome", ELEM(op, ADD, MOVE_TO_SLOT, CRAFT));
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  node->custom1 = ADD;
  node_update(ntree, node);
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode *node,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(
      label, operation_names[std::clamp<int>(node->custom1, ADD, CRAFT)], label_maxncpy);
}

static void node_register()
{
  static bke::bNodeType ntype;

  logic_node_type_base(&ntype, "LogicNativeInventory"_ustr);
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.updatefunc = node_update;
  ntype.labelfunc = node_label;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_logic_inventory_cc

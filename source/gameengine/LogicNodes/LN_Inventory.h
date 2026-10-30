/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 *
 * Inventory rules shared by the inventory nodes and Object Grid View. An inventory is a list of
 * slots; a slot is a dictionary {"item": name, "count": amount}. A plain name in the list is one
 * item in its own slot.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "LN_Types.h"

namespace blender {
struct Collection;
}
class KX_GameObject;
class KX_Scene;

/** Item name and amount of one slot; false when the value is not a slot. */
bool LN_InventoryEntry(const LN_Value &slot, std::string &r_item, int32_t &r_count);
/** What a grid slot shows for \a slot: like #LN_InventoryEntry, but a recipe line shows what it
 * makes. */
bool LN_InventoryShownEntry(const LN_Value &slot, std::string &r_item, int32_t &r_count);
LN_Value LN_MakeInventoryEntry(const std::string &item, int32_t count);

/**
 * How an inventory keeps its slots. Packed: an emptied slot goes and the later ones move up, and a
 * stack dropped past the end goes to the end. Keep Places: an emptied slot stays as an empty place,
 * a stack dropped on a place past the end goes exactly there (the places before it stay empty), so
 * players can leave gaps and arrange items; trailing empty places are trimmed.
 */
enum class LN_InventoryLayout : uint8_t {
  Packed = 0,
  KeepPlaces,
};
/** An empty place: a value that is no slot (None), or a slot without an amount. */
bool LN_InventoryIsEmptyPlace(const LN_Value &value);
/** Removes the empty places at the end of the list. */
void LN_InventoryTrimEmptyPlaces(std::vector<LN_Value> &slots);

/** An item and how many of it. */
struct LN_RecipePart {
  std::string item;
  int32_t count = 1;
};

/**
 * A recipe written as one line of text: ingredients joined by "+", then "=" (or "->") and the
 * results, as in "Apple + Mushroom = Potion", "2 Wood + Stone = Axe" or "Milk Bucket = Butter +
 * Bucket". An amount is a number before the name ("2 Wood") or "x" and a number after it
 * ("Wood x2"); the same item written twice adds up. Parts keep their written order; the first
 * result is the main one. "@" and a name after the results is the station the recipe needs
 * ("2 Wood + Stone = Axe @ Workbench"); a recipe with a station is made in a crafting menu, not by
 * dropping one item on another.
 */
struct LN_Recipe {
  std::vector<LN_RecipePart> ingredients;
  /** Never empty in a parsed recipe. */
  std::vector<LN_RecipePart> results;
  std::string station;
};

/** False when \a text is not a recipe (no "=", no ingredient or result, an amount below 1). */
bool LN_ParseRecipe(const std::string &text, LN_Recipe &r_recipe);
/** Recipes of a list of recipe texts (or one text); values that are not recipes are skipped, with
 * their position kept in \a r_indices. */
void LN_ParseRecipes(const LN_Value &recipes,
                     std::vector<LN_Recipe> &r_recipes,
                     std::vector<int32_t> *r_indices = nullptr);
/** True when \a recipe uses exactly the items of \a available (any order) and needs no more of
 * each than it has. */
bool LN_RecipeMatches(const LN_Recipe &recipe, const std::vector<LN_RecipePart> &available);
/** How many times in a row the inventory holds the ingredients of \a recipe (0: not once). An
 * ingredient the recipe gives back, such as a tool, only has to be there. */
int32_t LN_RecipeCraftable(const std::vector<LN_Value> &slots, const LN_Recipe &recipe);

/** An item's stack limit (0: no limit). */
using LN_MaxStackFn = int32_t (*)(const std::string &item, const void *user_data);

/** Stack limits of an Item Library: an item's "max_stack" game property there, else
 * \a max_stack. Passed with #LN_InventoryStackLimit as the stack limit function. */
struct LN_InventoryStackLimits {
  KX_Scene *scene = nullptr;
  const blender::Collection *library = nullptr;
  int32_t max_stack = 0;
};
/** #LN_MaxStackFn of #LN_InventoryStackLimits. */
int32_t LN_InventoryStackLimit(const std::string &item, const void *limits);

/**
 * Adds \a amount of \a item. With \a stack, fills existing slots of the item first, up to
 * \a max_stack per slot (0: no limit); otherwise every item gets its own slot. New slots fill the
 * empty places first, then go at the end until \a capacity slots in total (0: no limit, up to
 * 65536 slots). Returns how many did not fit.
 */
int32_t LN_InventoryAdd(std::vector<LN_Value> &slots,
                        const std::string &item,
                        int32_t amount,
                        int32_t max_stack,
                        bool stack,
                        int32_t capacity);
/** Takes up to \a amount of \a item, newest slots first. Returns how many were taken. */
int32_t LN_InventoryRemoveItem(std::vector<LN_Value> &slots,
                               const std::string &item,
                               int32_t amount,
                               LN_InventoryLayout layout = LN_InventoryLayout::Packed);
/** Takes up to \a amount from one slot; an emptied slot goes (or stays as an empty place, see
 * #LN_InventoryLayout). Returns how many. */
int32_t LN_InventoryRemoveFromSlot(std::vector<LN_Value> &slots,
                                   int32_t slot,
                                   int32_t amount,
                                   LN_InventoryLayout layout = LN_InventoryLayout::Packed);
/** Total amount of \a item over all slots. */
int32_t LN_InventoryCount(const std::vector<LN_Value> &slots, const std::string &item);

enum class LN_InventorySortKey : int16_t {
  Item = 0,
  Count = 1,
};
/** Stable sort; values that are not slots keep their order after the slots (Keep Places: the
 * empty places go). */
void LN_InventorySort(std::vector<LN_Value> &slots,
                      LN_InventorySortKey key,
                      bool descending,
                      LN_InventoryLayout layout = LN_InventoryLayout::Packed);
/** Exchanges two slots; false when an index is out of range. */
bool LN_InventorySwap(std::vector<LN_Value> &slots, int32_t a, int32_t b);
/** Moves \a amount out of a slot into a new slot right after it (Keep Places: into the nearest
 * empty place after it, else before it, else at the end while \a capacity has room); false when it
 * cannot. */
bool LN_InventorySplit(std::vector<LN_Value> &slots,
                       int32_t slot,
                       int32_t amount,
                       LN_InventoryLayout layout = LN_InventoryLayout::Packed,
                       int32_t capacity = 0);

/** What Move To Slot or Craft did. */
enum class LN_InventoryOutcome : uint8_t {
  None = 0,
  /** Onto a value that is no slot (an empty place): the two changed places. */
  Moved,
  /** The same item: as many as fit joined the other stack; the rest stayed. */
  Stacked,
  /** A recipe made its result. */
  Combined,
  Swapped,
  /** A recipe was found but its result does not fit: nothing changed. */
  NoRoom,
  /** Craft: an ingredient is missing. */
  Missing,
  /** The list's Inventory Rules (Accept Categories) do not take the item: nothing changed. */
  Refused,
};
/** "Moved", "Stacked", "Combined" (Move To Slot) or "Crafted" (Craft), "Swapped", "No Room",
 * "Missing", "Refused"; empty for None. */
const char *LN_InventoryOutcomeName(LN_InventoryOutcome outcome, bool craft);

/**
 * Drops the slot \a from onto the slot \a to. With \a stack, the same item joins the stack up to
 * its limit and the rest stays. Otherwise a recipe without a station in \a recipes that uses
 * exactly the two slots' items (two of the same item too, when they do not stack) takes its
 * ingredients from them, the dragged one first; its results go where slots emptied, the
 * dropped-on one first, and the rest is added like Add Item. Two different items with no recipe
 * swap when \a swap. \a r_made is the main result of a recipe that was made.
 *
 * \a amount below the slot's count moves only that part of it (a split stack; 0: all of it): it
 * joins the same item, fills an empty place or becomes a new slot at the end (an empty cell past
 * the end, while \a capacity has room), and a recipe matches that many; the rest stays in its
 * slot. A part never swaps.
 */
LN_InventoryOutcome LN_InventoryMoveToSlot(std::vector<LN_Value> &slots,
                                           int32_t from,
                                           int32_t to,
                                           const std::vector<LN_Recipe> &recipes,
                                           LN_MaxStackFn max_stack,
                                           const void *max_stack_data,
                                           bool stack,
                                           int32_t capacity,
                                           bool swap,
                                           std::string &r_made,
                                           int32_t amount = 0,
                                           LN_InventoryLayout layout = LN_InventoryLayout::Packed);
/**
 * Move To Slot between two inventories (a bag and a chest, an equipment slot): drops the slot
 * \a from of \a source onto the slot \a to of \a destination. Past the end of \a destination (an
 * empty place, such as the list's background) the stack first joins that item's stacks there
 * (with \a stack, up to the limit) and the rest goes to the end, while \a capacity (of the
 * destination) has room. Onto the same item it joins the stack up to its limit (with \a stack) and
 * the rest stays; two different items change places when \a swap. Recipes only combine inside one
 * inventory. \a amount below the slot's count moves only that part of it (0: all of it), as for
 * #LN_InventoryMoveToSlot: the rest stays in \a source, and a part never swaps. Each list keeps its
 * own layout: a Keep Places destination takes the stack at \a to past its end too, as is.
 */
LN_InventoryOutcome LN_InventoryMoveToInventory(std::vector<LN_Value> &source,
                                                int32_t from,
                                                std::vector<LN_Value> &destination,
                                                int32_t to,
                                                LN_MaxStackFn max_stack,
                                                const void *max_stack_data,
                                                bool stack,
                                                int32_t capacity,
                                                bool swap,
                                                int32_t amount = 0,
                                                LN_InventoryLayout source_layout =
                                                    LN_InventoryLayout::Packed,
                                                LN_InventoryLayout destination_layout =
                                                    LN_InventoryLayout::Packed);
/** \a slot with \a count instead of its own amount (a plain name becomes a slot dictionary); any
 * other keys it has are kept. */
LN_Value LN_InventoryEntryWithCount(const LN_Value &slot, int32_t count);
/**
 * Makes \a recipe \a times times (0: as often as the ingredients allow), like crafting one at a
 * time: ingredients come from anywhere in the inventory (newest slots first), an ingredient it
 * gives back stays where it is, and the results are added like Add Item. It stops at the first
 * time that misses an ingredient or has no room for the results. Combined with \a r_crafted the
 * times it made it; or Missing / No Room when not even once, and nothing changes.
 */
LN_InventoryOutcome LN_InventoryCraft(std::vector<LN_Value> &slots,
                                      const LN_Recipe &recipe,
                                      LN_MaxStackFn max_stack,
                                      const void *max_stack_data,
                                      bool stack,
                                      int32_t capacity,
                                      int32_t times,
                                      int32_t &r_crafted,
                                      LN_InventoryLayout layout = LN_InventoryLayout::Packed);

/** The object named \a item in \a library (child collections included), active or hidden. */
KX_GameObject *LN_FindLibraryItem(KX_Scene &scene,
                                  const blender::Collection *library,
                                  const std::string &item);

/* -------------------------------------------------------------------- */
/** \name Inventory Rules
 *
 * A list's rules come from the scene's Inventory Rules set its game property picks (Scene > Items
 * and UI > Inventory Rules), whatever grid or node changes it. The functions below need only the
 * lists, their rules and plain values: no grid, no UI. A server could run them as they are.
 * \{ */

/** A fingerprint of a value: equal values have equal fingerprints (a moved stack is found again
 * by it). */
size_t LN_ValueFingerprint(const LN_Value &value);
/** Categories as typed ("Weapon, Shield": Accept Categories, or an item's category) to lowercase
 * names between commas (",weapon,shield,"); empty when it names none. */
std::string LN_NormalizedCategories(const std::string &text);
/** One of \a categories is in \a accepted (both from #LN_NormalizedCategories). */
bool LN_CategoriesOverlap(const std::string &accepted, const std::string &categories);
/** Where \a item is in \a list: \a index while it is still there, else the first place holding the
 * same value; -1 when it is gone. */
int32_t LN_InventoryFindPlace(const std::vector<LN_Value> &list,
                              int32_t index,
                              const LN_Value &item);

/** One set of the scene's Inventory Rules (#blender::GameInventoryRules), as the game reads it.
 * A `*_from` names a game property of the list's object that gives the value while playing. */
struct LN_InventoryRuleSet {
  int32_t capacity = 0;
  std::string capacity_from;
  bool stack = true;
  std::string stack_from;
  int32_t max_stack = 0;
  std::string max_stack_from;
  bool keep_places = false;
  std::string keep_places_from;
  std::string recipes;
  std::string accept_categories;
  std::string accept_categories_from;
  const blender::Collection *library = nullptr;
};

/** The rules one list follows at one moment. The defaults are those of a list that picks no set,
 * and the Inventory node's own defaults. */
struct LN_InventoryRules {
  /** Places the list holds at most; 0: no limit (up to 65536). */
  int32_t capacity = 0;
  bool stack = true;
  /** Most of an item one place holds when its library object has no max_stack (0: no limit). */
  int32_t max_stack = 0;
  bool keep_places = false;
  /** A game property of the list's object with recipe lines; empty: no recipes. */
  std::string recipes_property;
  /** Null: the scene's Item Library. */
  const blender::Collection *library = nullptr;
  /** Normalized (#LN_NormalizedCategories); empty: any item. */
  std::string accept_categories;

  LN_InventoryLayout Layout() const
  {
    return keep_places ? LN_InventoryLayout::KeepPlaces : LN_InventoryLayout::Packed;
  }
};

/** The rules of \a set (null: the defaults) for the list of \a owner: a value with a `*_from`
 * property reads that game property of \a owner when it has it (numbers rounded, at least 0;
 * switches on when nonzero; categories as text). Names of `*_from` properties \a owner does not
 * have go to \a r_missing. */
LN_InventoryRules LN_ResolveInventoryRules(const LN_InventoryRuleSet *set,
                                           KX_GameObject *owner,
                                           std::vector<std::string> *r_missing = nullptr);

/** An item's categories (normalized) in \a library. */
using LN_ItemCategoriesFn = std::string (*)(const LN_Value &item,
                                            const blender::Collection *library,
                                            const void *user_data);
/** The default #LN_ItemCategoriesFn (\a user_data: the KX_Scene): the item's own "category" (an
 * inventory slot dictionary), else the "category" game property of its object in \a library. */
std::string LN_InventoryItemCategories(const LN_Value &item,
                                       const blender::Collection *library,
                                       const void *scene);

/** What inventory changes look up about items: stack limits and categories, in the library of the
 * list's rules (the scene's Item Library when they name none). */
struct LN_InventoryLookup {
  KX_Scene *scene = nullptr;
  const blender::Collection *scene_library = nullptr;
  /** Null: #LN_InventoryItemCategories. */
  LN_ItemCategoriesFn categories = nullptr;
  const void *categories_data = nullptr;

  const blender::Collection *LibraryOf(const LN_InventoryRules &rules) const
  {
    return rules.library != nullptr ? rules.library : scene_library;
  }
  /** Stack limits of a list with \a rules, for #LN_InventoryStackLimit. */
  LN_InventoryStackLimits Limits(const LN_InventoryRules &rules) const;
  std::string Categories(const LN_Value &item, const LN_InventoryRules &rules) const;
  /** The list with \a rules takes \a item (its Accept Categories). */
  bool Accepts(const LN_InventoryRules &rules, const LN_Value &item) const;
};

/** A list with \a rules has room for \a item, and takes it: a stack of the same item with room
 * (Stack on), an empty place (Keep Places), or a new place within Capacity. */
bool LN_InventoryHasRoom(const std::vector<LN_Value> &list,
                         const LN_Value &item,
                         const LN_InventoryRules &rules,
                         const LN_InventoryLookup &lookup);

/** One move of a stack, as a player asked for it: a drag in a grid, the Inventory node's Move To
 * Slot (later maybe a message from a client). Plain values only, so it can be checked and applied
 * with the two lists and their rules alone. */
struct LN_InventoryMove {
  /** The place of the stack in the source list, and the stack as the player saw it there: it is
   * found again by its value when the list changed meanwhile (another player's move). */
  int32_t from = -1;
  LN_Value from_item;
  /** The place in the target list (past its end: an empty place), and what the player saw there
   * (none: an empty place). */
  int32_t to = -1;
  LN_Value to_item;
  /** How many: 0 or the whole count moves the stack; fewer moves a part, which never swaps. */
  int32_t amount = 0;
  /** Two different items may change places (the grid's or the node's Swap). */
  bool allow_swap = true;
  /** The slot the stack came from takes the other item back (its UI Element's Accept
   * Categories). */
  bool swap_back_allowed = true;
  /** Between two lists, a drop that cannot happen on its place puts the stack into the target
   * list as Add Item would (off for one-cell grids and the Inventory node). */
  bool into_list_fallback = true;
};

/**
 * Applies \a move from \a source (null: inside \a target) to \a target, each list by its own
 * rules: the target's Stack, Max Stack, Capacity, library and Recipes (\a recipes, only inside
 * one list) for the stack that goes there, each list's Keep Places for its own places, and each
 * list's Accept Categories for what enters it (Refused, nothing changes). When nothing can happen
 * on the target place, a stack from another list goes into the target list
 * (\a into_list_fallback), and a part inside one list takes the nearest free place. \a r_made is
 * the main result of a recipe that was made.
 */
LN_InventoryOutcome LN_InventoryApplyMove(const LN_InventoryMove &move,
                                          std::vector<LN_Value> &target,
                                          const LN_InventoryRules &target_rules,
                                          std::vector<LN_Value> *source,
                                          const LN_InventoryRules *source_rules,
                                          const std::vector<LN_Recipe> &recipes,
                                          const LN_InventoryLookup &lookup,
                                          std::string &r_made);

/** \} */

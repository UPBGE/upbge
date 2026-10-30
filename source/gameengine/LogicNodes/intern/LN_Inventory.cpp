/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup logicnodes
 */

#include "LN_Inventory.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string_view>

#include "DNA_collection_types.h"
#include "DNA_object_types.h"

#include "BL_SceneConverter.h"
#include "KX_GameObject.h"
#include "KX_Scene.h"

namespace {

constexpr const char *kItemKey = "item";
constexpr const char *kCountKey = "count";
/** Most slots an inventory grows to without a Capacity: keeps a typo, or crafting thousands with
 * Stack off, from freezing the game. */
constexpr int64_t kMaxInventorySlots = 1 << 16;

bool ValueAsCount(const LN_Value &value, int32_t &r_count)
{
  switch (value.type) {
    case LN_ValueType::Int:
      r_count = value.int_value;
      return true;
    case LN_ValueType::Float:
      if (!std::isfinite(value.float_value)) {
        return false;
      }
      r_count = int32_t(std::clamp(std::round(double(value.float_value)),
                                   double(INT32_MIN),
                                   double(INT32_MAX)));
      return true;
    default:
      return false;
  }
}

void SetCount(LN_Value &slot, const int32_t count)
{
  LN_Value &value = slot.dict_value[kCountKey];
  value = LN_Value();
  value.type = LN_ValueType::Int;
  value.exists = true;
  value.int_value = count;
}

/** Room left in a slot of \a item, or 0 when it is another item or not a stack. */
int32_t StackRoom(const LN_Value &slot, const std::string &item, const int32_t max_stack)
{
  std::string slot_item;
  int32_t count = 0;
  if (slot.type != LN_ValueType::Dict || !LN_InventoryEntry(slot, slot_item, count) ||
      slot_item != item)
  {
    return 0;
  }
  return max_stack > 0 ? std::max(0, max_stack - count) : INT32_MAX - std::max(0, count);
}

bool ValidIndex(const std::vector<LN_Value> &slots, const int32_t index)
{
  return index >= 0 && size_t(index) < slots.size();
}

/** Gives a slot a new amount; a plain name becomes a slot dictionary. */
void SetSlotCount(LN_Value &slot, const std::string &item, const int32_t count)
{
  if (slot.type == LN_ValueType::Dict) {
    SetCount(slot, count);
  }
  else {
    slot = LN_MakeInventoryEntry(item, count);
  }
}

bool KeepsPlaces(const LN_InventoryLayout layout)
{
  return layout == LN_InventoryLayout::KeepPlaces;
}

/** Takes the slot at \a index out: packed, the later slots move up; keeping places, it stays as an
 * empty place. */
void TakeOut(std::vector<LN_Value> &slots, const int32_t index, const LN_InventoryLayout layout)
{
  if (KeepsPlaces(layout)) {
    slots[size_t(index)] = LN_Value();
  }
  else {
    slots.erase(slots.begin() + index);
  }
}

/** Keep Places: empty places up to \a index, so it is a place of the list; false when it is past
 * \a capacity. */
bool PadToPlace(std::vector<LN_Value> &slots, const int32_t index, const int32_t capacity)
{
  if (index < 0 || int64_t(index) >= (capacity > 0 ? int64_t(capacity) : kMaxInventorySlots)) {
    return false;
  }
  if (size_t(index) >= slots.size()) {
    slots.resize(size_t(index) + 1);
  }
  return true;
}

/** The first empty place of the list, or -1. */
int32_t FirstEmptyPlace(const std::vector<LN_Value> &slots)
{
  for (size_t index = 0; index < slots.size(); ++index) {
    if (LN_InventoryIsEmptyPlace(slots[index])) {
      return int32_t(index);
    }
  }
  return -1;
}

/** How many places hold an item. */
int64_t FilledPlaces(const std::vector<LN_Value> &slots)
{
  return std::count_if(slots.begin(), slots.end(), [](const LN_Value &slot) {
    return !LN_InventoryIsEmptyPlace(slot);
  });
}

bool IsSpace(const char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string_view Trim(std::string_view text)
{
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

/** A whole number of at most 9 digits; -1 when \a text is not one. */
int32_t ParseAmount(const std::string_view text)
{
  if (text.empty() || text.size() > 9) {
    return -1;
  }
  int32_t amount = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return -1;
    }
    amount = amount * 10 + (c - '0');
  }
  return amount;
}

/** The length of the times sign ("x", "X" or "×") at \a at, or 0. */
size_t TimesSignLength(const std::string_view text, const size_t at)
{
  constexpr std::string_view times_sign = "\xC3\x97";
  if (at < text.size() && (text[at] == 'x' || text[at] == 'X')) {
    return 1;
  }
  return at < text.size() && text.substr(at, times_sign.size()) == times_sign ? times_sign.size() :
                                                                                 0;
}

/** An item or station name: not empty, and nothing that belongs to the recipe's own signs. */
bool ValidRecipeName(const std::string_view name)
{
  return !name.empty() && name.find_first_of("=@<>+,") == std::string_view::npos &&
         name.find("\xE2\x86\x92") == std::string_view::npos;
}

/** "Wood", "2 Wood", "2x Wood", "2 x Wood", "Wood x2", "Wood x 2" or "Wood ×2". */
bool ParseRecipePart(std::string_view text, LN_RecipePart &r_part)
{
  text = Trim(text);
  int32_t amount = 1;
  size_t digits = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    ++digits;
  }
  /* A leading amount ends at a space, or at a times sign with a space after it ("9mm" is a
   * name). */
  size_t name_start = digits;
  while (name_start < text.size() && IsSpace(text[name_start])) {
    ++name_start;
  }
  if (const size_t sign = TimesSignLength(text, name_start);
      sign > 0 && name_start + sign < text.size() && IsSpace(text[name_start + sign]))
  {
    name_start += sign;
  }
  if (digits > 0 && name_start > digits) {
    amount = ParseAmount(text.substr(0, digits));
    text = Trim(text.substr(name_start));
  }
  else if (const size_t space = text.find_last_of(" \t"); space != std::string_view::npos) {
    const std::string_view tail = text.substr(space + 1);
    std::string_view head = Trim(text.substr(0, space));
    int32_t trailing = -1;
    if (const size_t sign = TimesSignLength(tail, 0); sign > 0) {
      trailing = ParseAmount(tail.substr(sign));
    }
    else if (const size_t before = head.find_last_of(" \t");
             before != std::string_view::npos &&
             TimesSignLength(head, before + 1) == head.size() - before - 1)
    {
      trailing = ParseAmount(tail);
      if (trailing >= 0) {
        head = Trim(head.substr(0, before));
      }
    }
    if (trailing >= 0) {
      amount = trailing;
      text = head;
    }
  }
  r_part.item = std::string(text);
  r_part.count = amount;
  return amount > 0 && ValidRecipeName(text);
}

/** Where the "=" (or "=>", "->", "→") before the result is, and its length; npos when none. */
size_t FindRecipeArrow(const std::string_view text, size_t &r_length)
{
  constexpr std::string_view arrows[] = {"=", "->", "\xE2\x86\x92"};
  for (const std::string_view arrow : arrows) {
    if (const size_t position = text.find(arrow); position != std::string_view::npos) {
      r_length = arrow.size();
      if (arrow == "=" && text.substr(position + 1, 1) == ">") {
        r_length = 2;
      }
      return position;
    }
  }
  return std::string_view::npos;
}

/** Parts joined by "+" or ","; the same item written twice adds up. */
bool ParseRecipeParts(std::string_view text, std::vector<LN_RecipePart> &r_parts)
{
  while (true) {
    const size_t separator = text.find_first_of("+,");
    LN_RecipePart part;
    if (!ParseRecipePart(text.substr(0, separator), part)) {
      return false;
    }
    const auto same = std::find_if(r_parts.begin(), r_parts.end(), [&](const LN_RecipePart &other) {
      return other.item == part.item;
    });
    if (same != r_parts.end()) {
      same->count = int32_t(std::min<int64_t>(int64_t(same->count) + part.count, INT32_MAX));
    }
    else {
      r_parts.push_back(std::move(part));
    }
    if (separator == std::string_view::npos) {
      return true;
    }
    text.remove_prefix(separator + 1);
  }
}

}  // namespace

bool LN_ParseRecipe(const std::string &text, LN_Recipe &r_recipe)
{
  /* A line that is not a recipe leaves nothing half read. */
  r_recipe = LN_Recipe();
  LN_Recipe recipe;
  size_t arrow_length = 0;
  const std::string_view line = text;
  const size_t arrow = FindRecipeArrow(line, arrow_length);
  if (arrow == std::string_view::npos) {
    return false;
  }
  std::string_view results = line.substr(arrow + arrow_length);
  if (const size_t at = results.find('@'); at != std::string_view::npos) {
    const std::string_view station = Trim(results.substr(at + 1));
    if (!ValidRecipeName(station)) {
      return false;
    }
    recipe.station = std::string(station);
    results = results.substr(0, at);
  }
  if (!ParseRecipeParts(results, recipe.results) ||
      !ParseRecipeParts(line.substr(0, arrow), recipe.ingredients))
  {
    return false;
  }
  r_recipe = std::move(recipe);
  return true;
}

void LN_ParseRecipes(const LN_Value &recipes,
                     std::vector<LN_Recipe> &r_recipes,
                     std::vector<int32_t> *r_indices)
{
  r_recipes.clear();
  if (r_indices != nullptr) {
    r_indices->clear();
  }
  const auto add = [&](const LN_Value &value, const int32_t index) {
    LN_Recipe recipe;
    if (value.exists && value.type == LN_ValueType::String &&
        LN_ParseRecipe(value.string_value, recipe))
    {
      r_recipes.push_back(std::move(recipe));
      if (r_indices != nullptr) {
        r_indices->push_back(index);
      }
    }
  };
  if (recipes.exists && recipes.type == LN_ValueType::List) {
    for (size_t index = 0; index < recipes.list_value.size(); ++index) {
      add(recipes.list_value[index], int32_t(std::min<size_t>(index, INT32_MAX)));
    }
  }
  else {
    add(recipes, 0);
  }
}

const char *LN_InventoryOutcomeName(const LN_InventoryOutcome outcome, const bool craft)
{
  switch (outcome) {
    case LN_InventoryOutcome::None:
      return "";
    case LN_InventoryOutcome::Moved:
      return "Moved";
    case LN_InventoryOutcome::Stacked:
      return "Stacked";
    case LN_InventoryOutcome::Combined:
      return craft ? "Crafted" : "Combined";
    case LN_InventoryOutcome::Swapped:
      return "Swapped";
    case LN_InventoryOutcome::NoRoom:
      return "No Room";
    case LN_InventoryOutcome::Missing:
      return "Missing";
    case LN_InventoryOutcome::Refused:
      return "Refused";
  }
  return "";
}

bool LN_RecipeMatches(const LN_Recipe &recipe, const std::vector<LN_RecipePart> &available)
{
  std::vector<LN_RecipePart> have;
  for (const LN_RecipePart &part : available) {
    if (part.item.empty() || part.count <= 0) {
      continue;
    }
    const auto same = std::find_if(have.begin(), have.end(), [&](const LN_RecipePart &other) {
      return other.item == part.item;
    });
    if (same != have.end()) {
      same->count = int32_t(std::min<int64_t>(int64_t(same->count) + part.count, INT32_MAX));
    }
    else {
      have.push_back(part);
    }
  }
  if (recipe.ingredients.empty() || have.size() != recipe.ingredients.size()) {
    return false;
  }
  for (const LN_RecipePart &ingredient : recipe.ingredients) {
    const auto found = std::find_if(have.begin(), have.end(), [&](const LN_RecipePart &part) {
      return part.item == ingredient.item;
    });
    if (found == have.end() || found->count < ingredient.count) {
      return false;
    }
  }
  return true;
}

bool LN_InventoryEntry(const LN_Value &slot, std::string &r_item, int32_t &r_count)
{
  if (!slot.exists) {
    return false;
  }
  if (slot.type == LN_ValueType::String) {
    r_item = slot.string_value;
    r_count = 1;
    return !r_item.empty();
  }
  if (slot.type != LN_ValueType::Dict) {
    return false;
  }
  const auto item = slot.dict_value.find(kItemKey);
  if (item == slot.dict_value.end() || item->second.type != LN_ValueType::String ||
      item->second.string_value.empty())
  {
    return false;
  }
  r_item = item->second.string_value;
  const auto count = slot.dict_value.find(kCountKey);
  if (count == slot.dict_value.end() || !ValueAsCount(count->second, r_count)) {
    r_count = 1;
  }
  return true;
}

bool LN_InventoryShownEntry(const LN_Value &slot, std::string &r_item, int32_t &r_count)
{
  LN_Recipe recipe;
  if (slot.exists && slot.type == LN_ValueType::String &&
      LN_ParseRecipe(slot.string_value, recipe))
  {
    r_item = recipe.results.front().item;
    r_count = recipe.results.front().count;
    return true;
  }
  return LN_InventoryEntry(slot, r_item, r_count);
}

LN_Value LN_MakeInventoryEntry(const std::string &item, const int32_t count)
{
  LN_Value slot;
  slot.type = LN_ValueType::Dict;
  slot.exists = true;
  LN_Value &name = slot.dict_value[kItemKey];
  name.type = LN_ValueType::String;
  name.exists = true;
  name.string_value = item;
  SetCount(slot, count);
  return slot;
}

bool LN_InventoryIsEmptyPlace(const LN_Value &value)
{
  std::string item;
  int32_t count = 0;
  return !LN_InventoryEntry(value, item, count) || count <= 0;
}

void LN_InventoryTrimEmptyPlaces(std::vector<LN_Value> &slots)
{
  while (!slots.empty() && LN_InventoryIsEmptyPlace(slots.back())) {
    slots.pop_back();
  }
}

int32_t LN_InventoryAdd(std::vector<LN_Value> &slots,
                        const std::string &item,
                        const int32_t amount,
                        const int32_t max_stack,
                        const bool stack,
                        const int32_t capacity)
{
  if (item.empty() || amount <= 0) {
    return std::max(0, amount);
  }
  int32_t remaining = amount;
  if (stack) {
    for (LN_Value &slot : slots) {
      if (remaining == 0) {
        break;
      }
      const int32_t room = StackRoom(slot, item, max_stack);
      if (room > 0) {
        std::string slot_item;
        int32_t count = 0;
        LN_InventoryEntry(slot, slot_item, count);
        const int32_t added = std::min(room, remaining);
        SetCount(slot, count + added);
        remaining -= added;
      }
    }
  }
  const int32_t per_slot = !stack ? 1 : (max_stack > 0 ? max_stack : remaining);
  /* Empty places first (an inventory that keeps places has gaps), then at the end. */
  for (LN_Value &slot : slots) {
    if (remaining == 0) {
      break;
    }
    if (LN_InventoryIsEmptyPlace(slot)) {
      const int32_t added = std::min(per_slot, remaining);
      slot = LN_MakeInventoryEntry(item, added);
      remaining -= added;
    }
  }
  const size_t max_slots = size_t(capacity > 0 ? capacity : kMaxInventorySlots);
  while (remaining > 0 && slots.size() < max_slots) {
    const int32_t added = std::min(per_slot, remaining);
    slots.push_back(LN_MakeInventoryEntry(item, added));
    remaining -= added;
  }
  return remaining;
}

int32_t LN_InventoryRemoveItem(std::vector<LN_Value> &slots,
                               const std::string &item,
                               const int32_t amount,
                               const LN_InventoryLayout layout)
{
  int32_t removed = 0;
  /* Emptied slots go in one pass afterwards, so a big Stack off inventory stays quick. */
  std::vector<bool> emptied;
  for (int32_t index = int32_t(slots.size()) - 1; index >= 0 && removed < amount; --index) {
    std::string slot_item;
    int32_t count = 0;
    if (!LN_InventoryEntry(slots[size_t(index)], slot_item, count) || slot_item != item) {
      continue;
    }
    const int32_t taken = std::min(amount - removed, std::max(0, count));
    removed += taken;
    if (count - taken <= 0 && KeepsPlaces(layout)) {
      slots[size_t(index)] = LN_Value();
    }
    else if (count - taken <= 0) {
      emptied.resize(slots.size(), false);
      emptied[size_t(index)] = true;
    }
    else {
      SetSlotCount(slots[size_t(index)], item, count - taken);
    }
  }
  if (KeepsPlaces(layout)) {
    LN_InventoryTrimEmptyPlaces(slots);
  }
  if (!emptied.empty()) {
    size_t kept = 0;
    for (size_t index = 0; index < slots.size(); ++index) {
      if (emptied[index]) {
        continue;
      }
      if (kept != index) {
        slots[kept] = std::move(slots[index]);
      }
      kept++;
    }
    slots.resize(kept);
  }
  return removed;
}

int32_t LN_InventoryRemoveFromSlot(std::vector<LN_Value> &slots,
                                   const int32_t slot,
                                   const int32_t amount,
                                   const LN_InventoryLayout layout)
{
  std::string item;
  int32_t count = 0;
  if (amount <= 0 || !ValidIndex(slots, slot) ||
      !LN_InventoryEntry(slots[size_t(slot)], item, count))
  {
    return 0;
  }
  const int32_t removed = std::min(amount, std::max(0, count));
  if (count - removed <= 0) {
    TakeOut(slots, slot, layout);
    if (KeepsPlaces(layout)) {
      LN_InventoryTrimEmptyPlaces(slots);
    }
  }
  else {
    SetSlotCount(slots[size_t(slot)], item, count - removed);
  }
  return removed;
}

int32_t LN_InventoryCount(const std::vector<LN_Value> &slots, const std::string &item)
{
  int64_t total = 0;
  for (const LN_Value &slot : slots) {
    std::string slot_item;
    int32_t count = 0;
    if (LN_InventoryEntry(slot, slot_item, count) && slot_item == item) {
      total += std::max(0, count);
    }
  }
  return int32_t(std::min<int64_t>(total, INT32_MAX));
}

void LN_InventorySort(std::vector<LN_Value> &slots,
                      const LN_InventorySortKey key,
                      const bool descending,
                      const LN_InventoryLayout layout)
{
  struct Keyed {
    std::string item;
    int32_t count = 0;
    bool valid = false;
    LN_Value slot;
  };
  std::vector<Keyed> keyed;
  keyed.reserve(slots.size());
  for (LN_Value &slot : slots) {
    Keyed entry;
    entry.valid = LN_InventoryEntry(slot, entry.item, entry.count);
    entry.slot = std::move(slot);
    keyed.push_back(std::move(entry));
  }
  std::stable_sort(keyed.begin(), keyed.end(), [&](const Keyed &a, const Keyed &b) {
    if (a.valid != b.valid) {
      return a.valid;
    }
    if (!a.valid) {
      return false;
    }
    if (key == LN_InventorySortKey::Count && a.count != b.count) {
      return descending ? a.count > b.count : a.count < b.count;
    }
    if (a.item != b.item) {
      return descending && key == LN_InventorySortKey::Item ? a.item > b.item : a.item < b.item;
    }
    return false;
  });
  for (size_t index = 0; index < slots.size(); ++index) {
    slots[index] = std::move(keyed[index].slot);
  }
  if (KeepsPlaces(layout)) {
    LN_InventoryTrimEmptyPlaces(slots);
  }
}

bool LN_InventorySwap(std::vector<LN_Value> &slots, const int32_t a, const int32_t b)
{
  if (!ValidIndex(slots, a) || !ValidIndex(slots, b)) {
    return false;
  }
  std::swap(slots[size_t(a)], slots[size_t(b)]);
  return true;
}

bool LN_InventorySplit(std::vector<LN_Value> &slots,
                       const int32_t slot,
                       const int32_t amount,
                       const LN_InventoryLayout layout,
                       const int32_t capacity)
{
  std::string item;
  int32_t count = 0;
  if (!ValidIndex(slots, slot) || !LN_InventoryEntry(slots[size_t(slot)], item, count) ||
      amount <= 0 || amount >= count)
  {
    return false;
  }
  if (slots[size_t(slot)].type == LN_ValueType::String) {
    return false;
  }
  const bool full = int64_t(slots.size()) >=
                    (capacity > 0 ? int64_t(capacity) : int64_t(kMaxInventorySlots));
  /* The part keeps the stack's own values (durability...). */
  LN_Value part = LN_InventoryEntryWithCount(slots[size_t(slot)], amount);
  if (KeepsPlaces(layout)) {
    /* Nothing moves: the part takes the nearest empty place after the slot, else before it, else
     * a new place at the end. */
    int32_t place = -1;
    for (size_t index = size_t(slot) + 1; index < slots.size() && place < 0; ++index) {
      place = LN_InventoryIsEmptyPlace(slots[index]) ? int32_t(index) : -1;
    }
    for (int32_t index = slot - 1; index >= 0 && place < 0; --index) {
      place = LN_InventoryIsEmptyPlace(slots[size_t(index)]) ? index : -1;
    }
    if (place < 0) {
      if (full) {
        return false;
      }
      place = int32_t(slots.size());
      slots.emplace_back();
    }
    slots[size_t(place)] = std::move(part);
    SetCount(slots[size_t(slot)], count - amount);
    return true;
  }
  if (full) {
    return false;
  }
  SetCount(slots[size_t(slot)], count - amount);
  slots.insert(slots.begin() + slot + 1, std::move(part));
  return true;
}

LN_InventoryOutcome LN_InventoryMoveToSlot(std::vector<LN_Value> &slots,
                                           const int32_t from,
                                           const int32_t to,
                                           const std::vector<LN_Recipe> &recipes,
                                           const LN_MaxStackFn max_stack,
                                           const void *max_stack_data,
                                           const bool stack,
                                           const int32_t capacity,
                                           const bool swap,
                                           std::string &r_made,
                                           const int32_t amount,
                                           const LN_InventoryLayout layout)
{
  r_made.clear();
  std::string from_item;
  std::string to_item;
  int32_t from_count = 0;
  int32_t to_count = 0;
  if (!ValidIndex(slots, from) || to < 0 || from == to ||
      !LN_InventoryEntry(slots[size_t(from)], from_item, from_count) || from_count <= 0)
  {
    return LN_InventoryOutcome::None;
  }
  /* A split stack moves only part of it; the rest stays in its slot. */
  const int32_t moving = amount > 0 && amount < from_count ? amount : from_count;
  const bool part = moving < from_count;
  const auto split_off = [&]() {
    LN_Value split = LN_InventoryEntryWithCount(slots[size_t(from)], moving);
    SetSlotCount(slots[size_t(from)], from_item, from_count - moving);
    return split;
  };
  /* Keep Places: a place past the end is a place too, the ones before it empty. */
  if (KeepsPlaces(layout) && !ValidIndex(slots, to) && !PadToPlace(slots, to, capacity)) {
    return LN_InventoryOutcome::NoRoom;
  }
  if (!ValidIndex(slots, to)) {
    /* An empty cell past the end (a grid's Show Empty): the stack goes to the end, a part of it
     * into a new slot there. */
    if (part) {
      if (slots.size() >= size_t(capacity > 0 ? capacity : kMaxInventorySlots)) {
        return LN_InventoryOutcome::NoRoom;
      }
      LN_Value split = split_off();
      slots.push_back(std::move(split));
      return LN_InventoryOutcome::Moved;
    }
    if (from + 1 == int32_t(slots.size())) {
      return LN_InventoryOutcome::None;
    }
    std::rotate(slots.begin() + from, slots.begin() + from + 1, slots.end());
    return LN_InventoryOutcome::Moved;
  }
  const auto limit_of = [&](const std::string &item) {
    return max_stack != nullptr ? std::max(0, max_stack(item, max_stack_data)) : 0;
  };
  if (!LN_InventoryEntry(slots[size_t(to)], to_item, to_count) || to_count <= 0) {
    if (part) {
      slots[size_t(to)] = split_off();
      return LN_InventoryOutcome::Moved;
    }
    std::swap(slots[size_t(from)], slots[size_t(to)]);
    if (KeepsPlaces(layout)) {
      LN_InventoryTrimEmptyPlaces(slots);
    }
    return LN_InventoryOutcome::Moved;
  }

  const bool same_item = from_item == to_item;
  if (same_item && stack) {
    const int32_t limit = limit_of(to_item);
    const int32_t room = limit > 0 ? std::max(0, limit - to_count) :
                                     INT32_MAX - std::max(0, to_count);
    const int32_t moved = std::min(room, moving);
    if (moved > 0) {
      SetSlotCount(slots[size_t(to)], to_item, to_count + moved);
      if (from_count - moved > 0) {
        SetSlotCount(slots[size_t(from)], from_item, from_count - moved);
      }
      else {
        TakeOut(slots, from, layout);
        if (KeepsPlaces(layout)) {
          LN_InventoryTrimEmptyPlaces(slots);
        }
      }
      return LN_InventoryOutcome::Stacked;
    }
  }

  /* Two of the same item that do not stack can still make something ("2 Herb = Mixed Herb"). A
   * drop combines two items: a recipe of one ("Herb = Dried Herb") is for crafting. */
  const std::vector<LN_RecipePart> pair = {{from_item, moving}, {to_item, to_count}};
  for (const LN_Recipe &recipe : recipes) {
    int64_t uses = 0;
    for (const LN_RecipePart &ingredient : recipe.ingredients) {
      uses += ingredient.count;
    }
    if (!recipe.station.empty() || uses < 2 || !LN_RecipeMatches(recipe, pair)) {
      continue;
    }
    int32_t moving_left = moving;
    int32_t to_left = to_count;
    for (const LN_RecipePart &ingredient : recipe.ingredients) {
      /* The dragged slot gives first, so two of one item come from both slots. */
      const int32_t from_part = ingredient.item == from_item ?
                                    std::min(moving_left, ingredient.count) :
                                    0;
      moving_left -= from_part;
      if (ingredient.count > from_part) {
        to_left -= ingredient.count - from_part;
      }
    }
    /* What stays in the dragged slot: what the recipe did not use, and the part not dragged. */
    const int32_t from_left = from_count - (moving - moving_left);
    std::vector<LN_Value> result = slots;
    if (from_left > 0) {
      SetSlotCount(result[size_t(from)], from_item, from_left);
    }
    if (to_left > 0) {
      SetSlotCount(result[size_t(to)], to_item, to_left);
    }
    /* The results take the places of the slots they emptied, the dropped-on one first; the rest
     * is added like Add Item. */
    std::vector<int32_t> places;
    if (to_left <= 0) {
      places.push_back(to);
    }
    if (from_left <= 0) {
      places.push_back(from);
    }
    std::vector<int32_t> to_add;
    size_t used_places = 0;
    for (const LN_RecipePart &made : recipe.results) {
      const int32_t limit = limit_of(made.item);
      int32_t left = made.count;
      while (left > 0 && used_places < places.size()) {
        const int32_t placed = !stack ? 1 : (limit > 0 ? std::min(limit, left) : left);
        result[size_t(places[used_places++])] = LN_MakeInventoryEntry(made.item, placed);
        left -= placed;
      }
      to_add.push_back(left);
    }
    places.resize(used_places);
    /* Emptied slots no result took go (the later one first so the earlier index stays right), or
     * stay empty places. */
    for (const int32_t index : {std::max(from, to), std::min(from, to)}) {
      const int32_t left = index == from ? from_left : to_left;
      if (left <= 0 && std::find(places.begin(), places.end(), index) == places.end()) {
        TakeOut(result, index, layout);
      }
    }
    for (size_t index = 0; index < recipe.results.size(); ++index) {
      const std::string &item = recipe.results[index].item;
      if (to_add[index] > 0 &&
          LN_InventoryAdd(result, item, to_add[index], limit_of(item), stack, capacity) > 0)
      {
        /* A result does not fit: keep everything as it was. */
        return LN_InventoryOutcome::NoRoom;
      }
    }
    if (KeepsPlaces(layout)) {
      LN_InventoryTrimEmptyPlaces(result);
    }
    slots = std::move(result);
    r_made = recipe.results.front().item;
    return LN_InventoryOutcome::Combined;
  }

  /* A part has no slot of its own to swap into. */
  if (same_item || !swap || part) {
    return LN_InventoryOutcome::None;
  }
  std::swap(slots[size_t(from)], slots[size_t(to)]);
  return LN_InventoryOutcome::Swapped;
}

LN_Value LN_InventoryEntryWithCount(const LN_Value &slot, const int32_t count)
{
  std::string item;
  int32_t current = 0;
  if (!LN_InventoryEntry(slot, item, current)) {
    return slot;
  }
  LN_Value result = slot;
  SetSlotCount(result, item, count);
  return result;
}

LN_InventoryOutcome LN_InventoryMoveToInventory(std::vector<LN_Value> &source,
                                                const int32_t from,
                                                std::vector<LN_Value> &destination,
                                                const int32_t to,
                                                const LN_MaxStackFn max_stack,
                                                const void *max_stack_data,
                                                const bool stack,
                                                const int32_t capacity,
                                                const bool swap,
                                                const int32_t amount,
                                                const LN_InventoryLayout source_layout,
                                                const LN_InventoryLayout destination_layout)
{
  std::string from_item;
  int32_t from_count = 0;
  if (&source == &destination || !ValidIndex(source, from) ||
      !LN_InventoryEntry(source[size_t(from)], from_item, from_count) || from_count <= 0)
  {
    return LN_InventoryOutcome::None;
  }
  /* A split stack moves only part of it; the rest stays in the source. */
  const int32_t moving = amount > 0 && amount < from_count ? amount : from_count;
  const bool part = moving < from_count;
  /* Each list as its layout wants it afterwards (Keep Places: no empty places at the end). */
  const auto tidy = [&]() {
    if (KeepsPlaces(source_layout)) {
      LN_InventoryTrimEmptyPlaces(source);
    }
    if (KeepsPlaces(destination_layout)) {
      LN_InventoryTrimEmptyPlaces(destination);
    }
  };
  /* Keep Places: a place past the end is a place too, the ones before it empty. */
  if (KeepsPlaces(destination_layout) && to >= 0 && !ValidIndex(destination, to) &&
      !PadToPlace(destination, to, capacity))
  {
    return LN_InventoryOutcome::NoRoom;
  }
  std::string to_item;
  int32_t to_count = 0;
  if (!ValidIndex(destination, to) ||
      !LN_InventoryEntry(destination[size_t(to)], to_item, to_count) || to_count <= 0)
  {
    /* An empty place: the stack goes to the end, the place an item fills in a packed list. It
     * first joins the stacks of the same item there (with `stack`), as Add Item would. */
    const size_t max_slots = size_t(capacity > 0 ? capacity : kMaxInventorySlots);
    if (ValidIndex(destination, to)) {
      if (part) {
        destination[size_t(to)] = LN_InventoryEntryWithCount(source[size_t(from)], moving);
        SetSlotCount(source[size_t(from)], from_item, from_count - moving);
        tidy();
        return LN_InventoryOutcome::Moved;
      }
      destination[size_t(to)] = std::move(source[size_t(from)]);
      TakeOut(source, from, source_layout);
      tidy();
      return LN_InventoryOutcome::Moved;
    }
    int32_t left = moving;
    if (stack) {
      const int32_t limit = max_stack != nullptr ?
                                std::max(0, max_stack(from_item, max_stack_data)) :
                                0;
      for (LN_Value &slot : destination) {
        std::string item;
        int32_t count = 0;
        if (left > 0 && LN_InventoryEntry(slot, item, count) && item == from_item && count > 0) {
          const int32_t moved = std::min(
              left, limit > 0 ? std::max(0, limit - count) : INT32_MAX - count);
          if (moved > 0) {
            SetSlotCount(slot, item, count + moved);
            left -= moved;
          }
        }
      }
    }
    /* The rest: a Keep Places destination's first empty place, else a new slot at the end. */
    const int32_t free_place = KeepsPlaces(destination_layout) ? FirstEmptyPlace(destination) :
                                                                 -1;
    const auto put = [&](LN_Value value) {
      if (free_place >= 0) {
        destination[size_t(free_place)] = std::move(value);
      }
      else {
        destination.push_back(std::move(value));
      }
    };
    if (left > 0 && free_place < 0 && destination.size() >= max_slots) {
      if (left == moving) {
        return LN_InventoryOutcome::NoRoom;
      }
      SetSlotCount(source[size_t(from)], from_item, from_count - (moving - left));
      tidy();
      return LN_InventoryOutcome::Stacked;
    }
    if (part) {
      if (left > 0) {
        put(LN_InventoryEntryWithCount(source[size_t(from)], left));
      }
      SetSlotCount(source[size_t(from)], from_item, from_count - moving);
      tidy();
      return left > 0 ? LN_InventoryOutcome::Moved : LN_InventoryOutcome::Stacked;
    }
    if (left > 0) {
      if (left != from_count) {
        SetSlotCount(source[size_t(from)], from_item, left);
      }
      put(std::move(source[size_t(from)]));
    }
    TakeOut(source, from, source_layout);
    tidy();
    return left > 0 ? LN_InventoryOutcome::Moved : LN_InventoryOutcome::Stacked;
  }
  if (from_item == to_item) {
    if (!stack) {
      return LN_InventoryOutcome::None;
    }
    const int32_t limit = max_stack != nullptr ? std::max(0, max_stack(to_item, max_stack_data)) :
                                                 0;
    const int32_t room = limit > 0 ? std::max(0, limit - to_count) :
                                     INT32_MAX - std::max(0, to_count);
    const int32_t moved = std::min(room, moving);
    if (moved <= 0) {
      return LN_InventoryOutcome::None;
    }
    SetSlotCount(destination[size_t(to)], to_item, to_count + moved);
    if (from_count - moved > 0) {
      SetSlotCount(source[size_t(from)], from_item, from_count - moved);
    }
    else {
      TakeOut(source, from, source_layout);
    }
    tidy();
    return LN_InventoryOutcome::Stacked;
  }
  /* A part has no slot of its own to swap into. */
  if (!swap || part) {
    return LN_InventoryOutcome::None;
  }
  std::swap(source[size_t(from)], destination[size_t(to)]);
  return LN_InventoryOutcome::Swapped;
}

namespace {

/** One craft by what it uses up and what it adds. An item given back (the Saw of "Log + Saw =
 * 4 Plank + Saw") is only needed at hand: it is not taken and given again. */
struct RecipeBalance {
  struct Use {
    std::string item;
    int32_t needed = 0;
    int32_t used = 0;
  };
  std::vector<Use> uses;
  std::vector<LN_RecipePart> gains;
};

RecipeBalance BalanceOf(const LN_Recipe &recipe)
{
  const auto amount_in = [](const std::vector<LN_RecipePart> &parts, const std::string &item) {
    const auto part = std::find_if(
        parts.begin(), parts.end(), [&](const LN_RecipePart &other) { return other.item == item; });
    return part != parts.end() ? part->count : 0;
  };
  RecipeBalance balance;
  for (const LN_RecipePart &ingredient : recipe.ingredients) {
    balance.uses.push_back(
        {ingredient.item,
         ingredient.count,
         std::max(0, ingredient.count - amount_in(recipe.results, ingredient.item))});
  }
  for (const LN_RecipePart &made : recipe.results) {
    if (const int32_t gain = made.count - amount_in(recipe.ingredients, made.item); gain > 0) {
      balance.gains.push_back({made.item, gain});
    }
  }
  return balance;
}

/**
 * How many crafts in a row, up to \a wanted, still have room for their results: crafting one at a
 * time stops at the first that has none. It counts slots instead of trying, so large amounts stay
 * quick. A stack that runs out frees its slot, so room is not simply used up in order: between the
 * crafts where a stack runs out the slots only grow, and each such span is searched by halving.
 */
int32_t CraftsThatFit(const std::vector<LN_Value> &slots,
                      const RecipeBalance &balance,
                      const LN_MaxStackFn max_stack,
                      const void *max_stack_data,
                      const bool stack,
                      const int32_t capacity,
                      const int32_t wanted)
{
  const int64_t max_slots = capacity > 0 ? capacity : kMaxInventorySlots;
  /* Each used-up item's stacks, newest first, as the amount taken that empties them. A stack of
   * 0 or less goes as soon as taking reaches it. */
  struct Emptying {
    std::vector<int64_t> totals;
    int64_t used = 0;
  };
  std::vector<Emptying> emptying;
  std::vector<int64_t> span_starts = {1};
  for (const RecipeBalance::Use &use : balance.uses) {
    if (use.used <= 0) {
      continue;
    }
    Emptying stacks;
    stacks.used = use.used;
    int64_t total = 0;
    for (size_t index = slots.size(); index-- > 0;) {
      std::string item;
      int32_t count = 0;
      if (LN_InventoryEntry(slots[index], item, count) && item == use.item) {
        const int64_t empties_at = count > 0 ? total + count : total + 1;
        total += std::max(0, count);
        stacks.totals.push_back(empties_at);
        /* The craft that empties this stack. */
        const int64_t craft = (empties_at + use.used - 1) / use.used;
        if (craft > 1 && craft <= wanted) {
          span_starts.push_back(craft);
        }
      }
    }
    emptying.push_back(std::move(stacks));
  }
  struct Growing {
    int64_t room = 0;
    int64_t limit = 0;
    int64_t gain = 0;
  };
  std::vector<Growing> growing;
  for (const LN_RecipePart &gain : balance.gains) {
    Growing result;
    result.limit = max_stack != nullptr ? std::max(0, max_stack(gain.item, max_stack_data)) : 0;
    result.gain = gain.count;
    for (const LN_Value &slot : slots) {
      result.room += stack ? StackRoom(slot, gain.item, int32_t(result.limit)) : 0;
    }
    growing.push_back(result);
  }
  /* Results that need no new slot always fit, also in an inventory already over Capacity. New
   * slots fill empty places first, so only the places holding an item count. */
  const int64_t filled = FilledPlaces(slots);
  const auto fits = [&](const int64_t crafts) {
    int64_t count = filled;
    for (const Emptying &stacks : emptying) {
      count -= std::upper_bound(stacks.totals.begin(), stacks.totals.end(), crafts * stacks.used) -
               stacks.totals.begin();
    }
    int64_t new_slots = 0;
    for (const Growing &result : growing) {
      const int64_t amount = crafts * result.gain;
      if (!stack) {
        new_slots += amount;
      }
      else if (amount > result.room) {
        new_slots += result.limit > 0 ? (amount - result.room + result.limit - 1) / result.limit :
                                        1;
      }
    }
    return new_slots == 0 || count + new_slots <= max_slots;
  };
  std::sort(span_starts.begin(), span_starts.end());
  span_starts.erase(std::unique(span_starts.begin(), span_starts.end()), span_starts.end());
  for (size_t span = 0; span < span_starts.size(); ++span) {
    const int64_t first = span_starts[span];
    const int64_t last = span + 1 < span_starts.size() ? span_starts[span + 1] - 1 : wanted;
    if (!fits(first)) {
      return int32_t(first - 1);
    }
    if (fits(last)) {
      continue;
    }
    int64_t fitting = first;
    int64_t failing = last;
    while (failing - fitting > 1) {
      const int64_t middle = fitting + (failing - fitting) / 2;
      (fits(middle) ? fitting : failing) = middle;
    }
    return int32_t(fitting);
  }
  return wanted;
}

}  // namespace

int32_t LN_RecipeCraftable(const std::vector<LN_Value> &slots, const LN_Recipe &recipe)
{
  if (recipe.ingredients.empty() || recipe.results.empty()) {
    return 0;
  }
  int64_t times = INT32_MAX;
  for (const RecipeBalance::Use &use : BalanceOf(recipe).uses) {
    const int64_t have = std::max(0, LN_InventoryCount(slots, use.item));
    if (have < use.needed) {
      return 0;
    }
    if (use.used > 0) {
      times = std::min(times, (have - use.needed) / use.used + 1);
    }
  }
  return int32_t(times);
}

LN_InventoryOutcome LN_InventoryCraft(std::vector<LN_Value> &slots,
                                      const LN_Recipe &recipe,
                                      const LN_MaxStackFn max_stack,
                                      const void *max_stack_data,
                                      const bool stack,
                                      const int32_t capacity,
                                      const int32_t times,
                                      int32_t &r_crafted,
                                      const LN_InventoryLayout layout)
{
  r_crafted = 0;
  if (recipe.ingredients.empty() || recipe.results.empty() || times < 0) {
    return LN_InventoryOutcome::None;
  }
  int32_t wanted = LN_RecipeCraftable(slots, recipe);
  if (wanted == 0) {
    return LN_InventoryOutcome::Missing;
  }
  const RecipeBalance balance = BalanceOf(recipe);
  /* A recipe that uses nothing up (only tools) has no "as many as possible": once. */
  const bool uses_up = std::any_of(balance.uses.begin(),
                                   balance.uses.end(),
                                   [](const RecipeBalance::Use &use) { return use.used > 0; });
  if (times > 0 || !uses_up) {
    wanted = std::min(wanted, std::max(times, 1));
  }
  /* Every amount stays a valid number: so many times what a craft uses or adds. */
  for (const RecipeBalance::Use &use : balance.uses) {
    wanted = std::min(wanted, INT32_MAX / std::max(1, use.used));
  }
  for (const LN_RecipePart &gain : balance.gains) {
    wanted = std::min(wanted, INT32_MAX / gain.count);
  }
  const int32_t crafts = CraftsThatFit(
      slots, balance, max_stack, max_stack_data, stack, capacity, wanted);
  if (crafts == 0) {
    return LN_InventoryOutcome::NoRoom;
  }
  std::vector<LN_Value> result = slots;
  for (const RecipeBalance::Use &use : balance.uses) {
    if (use.used > 0) {
      LN_InventoryRemoveItem(result, use.item, crafts * use.used, layout);
    }
  }
  for (const LN_RecipePart &gain : balance.gains) {
    const int32_t limit = max_stack != nullptr ? std::max(0, max_stack(gain.item, max_stack_data)) :
                                                 0;
    if (LN_InventoryAdd(result, gain.item, crafts * gain.count, limit, stack, capacity) > 0) {
      /* Counted above, so this does not happen; never leave half a craft. */
      return LN_InventoryOutcome::NoRoom;
    }
  }
  if (KeepsPlaces(layout)) {
    LN_InventoryTrimEmptyPlaces(result);
  }
  slots = std::move(result);
  r_crafted = crafts;
  return LN_InventoryOutcome::Combined;
}

KX_GameObject *LN_FindLibraryItem(KX_Scene &scene,
                                  const blender::Collection *library,
                                  const std::string &item)
{
  if (library == nullptr || item.empty()) {
    return nullptr;
  }
  BL_SceneConverter *converter = scene.GetBlenderSceneConverter();
  if (converter == nullptr) {
    return nullptr;
  }
  const auto find = [&](const auto &self, const blender::Collection *collection) -> KX_GameObject * {
    for (const blender::CollectionObject &entry : collection->gobject) {
      if (entry.ob != nullptr && item == entry.ob->id.name + 2) {
        return converter->FindGameObject(entry.ob);
      }
    }
    for (const blender::CollectionChild &child : collection->children) {
      if (child.collection != nullptr) {
        if (KX_GameObject *found = self(self, child.collection)) {
          return found;
        }
      }
    }
    return nullptr;
  };
  return find(find, library);
}

int32_t LN_InventoryStackLimit(const std::string &item, const void *limits)
{
  const LN_InventoryStackLimits &stack_limits = *static_cast<const LN_InventoryStackLimits *>(
      limits);
  if (stack_limits.scene != nullptr && stack_limits.library != nullptr) {
    if (KX_GameObject *object = LN_FindLibraryItem(
            *stack_limits.scene, stack_limits.library, item))
    {
      if (EXP_Value *value = object->GetProperty("max_stack")) {
        const double number = value->GetNumber();
        return std::isfinite(number) ? int32_t(std::clamp(std::round(number), 0.0, 2147483647.0)) :
                                       0;
      }
    }
  }
  return stack_limits.max_stack;
}

/* -------------------------------------------------------------------- */
/** \name Inventory Rules
 * \{ */

namespace {

template<typename Value> void HashCombine(size_t &seed, const Value &value)
{
  seed ^= std::hash<Value>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6u) + (seed >> 2u);
}

size_t Fingerprint(const LN_Value &value, const int depth)
{
  size_t result = size_t(value.type);
  HashCombine(result, value.exists);
  if (depth > 16) {
    return result;
  }
  switch (value.type) {
    case LN_ValueType::Bool:
      HashCombine(result, value.bool_value);
      break;
    case LN_ValueType::Int:
      HashCombine(result, value.int_value);
      break;
    case LN_ValueType::Float:
      HashCombine(result, value.float_value);
      break;
    case LN_ValueType::Vector:
      HashCombine(result, value.vector_value.x());
      HashCombine(result, value.vector_value.y());
      HashCombine(result, value.vector_value.z());
      break;
    case LN_ValueType::Vector4:
      HashCombine(result, value.vector4_value.x());
      HashCombine(result, value.vector4_value.y());
      HashCombine(result, value.vector4_value.z());
      HashCombine(result, value.vector4_value.w());
      break;
    case LN_ValueType::Color:
      HashCombine(result, value.color_value.x());
      HashCombine(result, value.color_value.y());
      HashCombine(result, value.color_value.z());
      HashCombine(result, value.color_value.w());
      break;
    case LN_ValueType::Rotation: {
      const MT_Vector3 euler = value.RotationEulerRadians();
      HashCombine(result, euler.x());
      HashCombine(result, euler.y());
      HashCombine(result, euler.z());
      break;
    }
    case LN_ValueType::String:
      HashCombine(result, value.string_value);
      break;
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
      HashCombine(result, uint8_t(value.runtime_ref.kind));
      HashCombine(result, value.runtime_ref.slot);
      HashCombine(result, value.runtime_ref.generation);
      break;
    case LN_ValueType::List:
      for (const LN_Value &item : value.list_value) {
        HashCombine(result, Fingerprint(item, depth + 1));
      }
      break;
    case LN_ValueType::Dict:
      for (const auto &item : value.dict_value) {
        HashCombine(result, item.first);
        HashCombine(result, Fingerprint(item.second, depth + 1));
      }
      break;
    default:
      HashCombine(result, value.reference_name);
      break;
  }
  return result;
}

/** A game property's number for a count: rounded, at least 0. */
int32_t PropertyCount(EXP_Value *value, const int32_t fallback)
{
  const double number = value->GetNumber();
  return std::isfinite(number) ? int32_t(std::clamp(std::round(number), 0.0, 2147483647.0)) :
                                 fallback;
}

}  // namespace

size_t LN_ValueFingerprint(const LN_Value &value)
{
  return Fingerprint(value, 0);
}

std::string LN_NormalizedCategories(const std::string &text)
{
  std::string result;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    size_t first = start;
    size_t last = end;
    while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) {
      first++;
    }
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) {
      last--;
    }
    if (last > first) {
      if (result.empty()) {
        result = ",";
      }
      for (size_t index = first; index < last; ++index) {
        result += char(std::tolower(static_cast<unsigned char>(text[index])));
      }
      result += ",";
    }
    start = end + 1;
  }
  return result;
}

bool LN_CategoriesOverlap(const std::string &accepted, const std::string &categories)
{
  size_t start = 0;
  while (start + 1 < categories.size()) {
    const size_t end = categories.find(',', start + 1);
    if (end == std::string::npos) {
      break;
    }
    if (accepted.find(std::string_view(categories).substr(start, end - start + 1)) !=
        std::string::npos)
    {
      return true;
    }
    start = end;
  }
  return false;
}

int32_t LN_InventoryFindPlace(const std::vector<LN_Value> &list,
                              const int32_t index,
                              const LN_Value &item)
{
  const size_t fingerprint = LN_ValueFingerprint(item);
  if (index >= 0 && size_t(index) < list.size() &&
      LN_ValueFingerprint(list[size_t(index)]) == fingerprint)
  {
    return index;
  }
  for (size_t other = 0; other < list.size(); ++other) {
    if (LN_ValueFingerprint(list[other]) == fingerprint) {
      return int32_t(other);
    }
  }
  return -1;
}

LN_InventoryRules LN_ResolveInventoryRules(const LN_InventoryRuleSet *set,
                                           KX_GameObject *owner,
                                           std::vector<std::string> *r_missing)
{
  LN_InventoryRules rules;
  if (set == nullptr) {
    return rules;
  }
  rules.capacity = std::max(0, set->capacity);
  rules.stack = set->stack;
  rules.max_stack = std::max(0, set->max_stack);
  rules.keep_places = set->keep_places;
  rules.recipes_property = set->recipes;
  rules.library = set->library;
  rules.accept_categories = LN_NormalizedCategories(set->accept_categories);
  /* "Or from": a game property of the list's object gives the value while playing. */
  const auto read = [&](const std::string &name) -> EXP_Value * {
    if (name.empty() || owner == nullptr) {
      return nullptr;
    }
    EXP_Value *value = owner->GetProperty(name);
    if (value == nullptr && r_missing != nullptr) {
      r_missing->push_back(name);
    }
    return value;
  };
  if (EXP_Value *value = read(set->capacity_from)) {
    rules.capacity = PropertyCount(value, rules.capacity);
  }
  if (EXP_Value *value = read(set->max_stack_from)) {
    rules.max_stack = PropertyCount(value, rules.max_stack);
  }
  if (EXP_Value *value = read(set->stack_from)) {
    rules.stack = value->GetNumber() != 0.0;
  }
  if (EXP_Value *value = read(set->keep_places_from)) {
    rules.keep_places = value->GetNumber() != 0.0;
  }
  if (EXP_Value *value = read(set->accept_categories_from)) {
    rules.accept_categories = LN_NormalizedCategories(value->GetText());
  }
  return rules;
}

std::string LN_InventoryItemCategories(const LN_Value &item,
                                       const blender::Collection *library,
                                       const void *scene)
{
  /* An inventory slot's own category comes before its library object's. */
  if (item.exists && item.type == LN_ValueType::Dict) {
    const auto own = item.dict_value.find("category");
    if (own != item.dict_value.end() && own->second.exists &&
        own->second.type == LN_ValueType::String)
    {
      return LN_NormalizedCategories(own->second.string_value);
    }
  }
  std::string name;
  int32_t count = 0;
  if (scene == nullptr || !LN_InventoryEntry(item, name, count)) {
    return std::string();
  }
  KX_GameObject *object = LN_FindLibraryItem(
      *const_cast<KX_Scene *>(static_cast<const KX_Scene *>(scene)), library, name);
  EXP_Value *category = object != nullptr ? object->GetProperty("category") : nullptr;
  return category != nullptr ? LN_NormalizedCategories(category->GetText()) : std::string();
}

LN_InventoryStackLimits LN_InventoryLookup::Limits(const LN_InventoryRules &rules) const
{
  LN_InventoryStackLimits limits;
  limits.scene = scene;
  limits.library = LibraryOf(rules);
  limits.max_stack = rules.max_stack;
  return limits;
}

std::string LN_InventoryLookup::Categories(const LN_Value &item,
                                           const LN_InventoryRules &rules) const
{
  return categories != nullptr ? categories(item, LibraryOf(rules), categories_data) :
                                 LN_InventoryItemCategories(item, LibraryOf(rules), scene);
}

bool LN_InventoryLookup::Accepts(const LN_InventoryRules &rules, const LN_Value &item) const
{
  return rules.accept_categories.empty() ||
         LN_CategoriesOverlap(rules.accept_categories, Categories(item, rules));
}

bool LN_InventoryHasRoom(const std::vector<LN_Value> &list,
                         const LN_Value &item,
                         const LN_InventoryRules &rules,
                         const LN_InventoryLookup &lookup)
{
  std::string name;
  int32_t count = 0;
  if (!LN_InventoryEntry(item, name, count) || !lookup.Accepts(rules, item)) {
    return false;
  }
  const LN_InventoryStackLimits limits = lookup.Limits(rules);
  const int32_t limit = LN_InventoryStackLimit(name, &limits);
  for (const LN_Value &place : list) {
    std::string other;
    int32_t other_count = 0;
    if (LN_InventoryIsEmptyPlace(place)) {
      if (rules.keep_places) {
        return true;
      }
    }
    else if (rules.stack && LN_InventoryEntry(place, other, other_count) && other == name &&
             (limit <= 0 || other_count < limit))
    {
      return true;
    }
  }
  return rules.capacity <= 0 || int64_t(list.size()) < rules.capacity;
}

LN_InventoryOutcome LN_InventoryApplyMove(const LN_InventoryMove &move,
                                          std::vector<LN_Value> &target,
                                          const LN_InventoryRules &target_rules,
                                          std::vector<LN_Value> *source,
                                          const LN_InventoryRules *source_rules,
                                          const std::vector<LN_Recipe> &recipes,
                                          const LN_InventoryLookup &lookup,
                                          std::string &r_made)
{
  r_made.clear();
  const bool same_list = source == nullptr || source == &target;
  std::vector<LN_Value> &from_list = same_list ? target : *source;
  const LN_InventoryRules &from_rules = same_list || source_rules == nullptr ? target_rules :
                                                                               *source_rules;
  if (move.from < 0 || move.to < 0 || !move.from_item.exists) {
    return LN_InventoryOutcome::None;
  }
  /* Both places found again by their items: an earlier move of this frame (another player's)
   * may have changed the lists. An empty place stays a place past the end. */
  const int32_t from = LN_InventoryFindPlace(from_list, move.from, move.from_item);
  const int32_t to = move.to_item.exists ? LN_InventoryFindPlace(target, move.to, move.to_item) :
                                           move.to;
  if (from < 0 || to < 0) {
    return LN_InventoryOutcome::None;
  }
  /* Dropped on an empty place that an earlier move of this frame filled: not that place. */
  if (!move.to_item.exists && size_t(to) < target.size() &&
      !LN_InventoryIsEmptyPlace(target[size_t(to)]))
  {
    return LN_InventoryOutcome::None;
  }
  /* What enters a list must be what its rules take. */
  if (!same_list && !lookup.Accepts(target_rules, move.from_item)) {
    return LN_InventoryOutcome::Refused;
  }
  const LN_InventoryStackLimits limits = lookup.Limits(target_rules);
  /* In a swap the other item goes back where the stack came from: that slot and that list must
   * take it. */
  const bool swap = move.allow_swap &&
                    (!move.to_item.exists ||
                     (move.swap_back_allowed &&
                      (same_list || lookup.Accepts(from_rules, move.to_item))));
  LN_InventoryOutcome outcome = LN_InventoryOutcome::None;
  if (same_list) {
    outcome = LN_InventoryMoveToSlot(target,
                                     from,
                                     to,
                                     recipes,
                                     LN_InventoryStackLimit,
                                     &limits,
                                     target_rules.stack,
                                     target_rules.capacity,
                                     swap,
                                     r_made,
                                     move.amount,
                                     target_rules.Layout());
  }
  else {
    outcome = LN_InventoryMoveToInventory(*source,
                                          from,
                                          target,
                                          to,
                                          LN_InventoryStackLimit,
                                          &limits,
                                          target_rules.stack,
                                          target_rules.capacity,
                                          swap,
                                          move.amount,
                                          from_rules.Layout(),
                                          target_rules.Layout());
  }
  /* Nothing happened on the filled place (no swap, a full stack, a part, which never swaps). From
   * another list the stack goes into this one as Add Item would: its own kind's stacks first,
   * then the first free place, so an equipped item can always go back into a bag with room.
   * Inside one list a part takes the nearest free place, as Split Stack. */
  if (outcome == LN_InventoryOutcome::None && move.to_item.exists) {
    if (!same_list && move.into_list_fallback) {
      outcome = LN_InventoryMoveToInventory(*source,
                                            from,
                                            target,
                                            -1,
                                            LN_InventoryStackLimit,
                                            &limits,
                                            target_rules.stack,
                                            target_rules.capacity,
                                            false,
                                            move.amount,
                                            from_rules.Layout(),
                                            target_rules.Layout());
    }
    else if (same_list) {
      std::string name;
      int32_t count = 0;
      LN_InventoryEntry(target[size_t(from)], name, count);
      if (move.amount > 0 && move.amount < count) {
        outcome = LN_InventorySplit(
                      target, from, move.amount, target_rules.Layout(), target_rules.capacity) ?
                      LN_InventoryOutcome::Moved :
                      LN_InventoryOutcome::NoRoom;
      }
    }
  }
  return outcome;
}

/** \} */

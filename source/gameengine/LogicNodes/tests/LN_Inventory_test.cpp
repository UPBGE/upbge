/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "EXP_BoolValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "KX_GameObject.h"
#include "LN_Inventory.h"

namespace {

std::vector<std::pair<std::string, int32_t>> Contents(const std::vector<LN_Value> &slots)
{
  std::vector<std::pair<std::string, int32_t>> contents;
  for (const LN_Value &slot : slots) {
    std::string item;
    int32_t count = 0;
    EXPECT_TRUE(LN_InventoryEntry(slot, item, count));
    contents.emplace_back(item, count);
  }
  return contents;
}

using Contents_t = std::vector<std::pair<std::string, int32_t>>;

}  // namespace

TEST(LN_Inventory, StacksSameItemsUpToTheStackLimit)
{
  std::vector<LN_Value> slots;
  EXPECT_EQ(LN_InventoryAdd(slots, "Apple", 3, 4, true, 0), 0);
  EXPECT_EQ(LN_InventoryAdd(slots, "Sword", 1, 1, true, 0), 0);
  EXPECT_EQ(LN_InventoryAdd(slots, "Apple", 3, 4, true, 0), 0);
  /* The first Apple stack fills to 4, the rest starts a new stack after the Sword. */
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 4}, {"Sword", 1}, {"Apple", 2}}));
  EXPECT_EQ(LN_InventoryCount(slots, "Apple"), 6);
}

TEST(LN_Inventory, UnstackedGivesEveryItemItsOwnSlotAndCapacityLimitsSlots)
{
  std::vector<LN_Value> slots;
  EXPECT_EQ(LN_InventoryAdd(slots, "Gem", 3, 0, false, 2), 1);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Gem", 1}, {"Gem", 1}}));
  /* No limit: one stack holds everything. A full inventory still stacks. */
  std::vector<LN_Value> stacked;
  EXPECT_EQ(LN_InventoryAdd(stacked, "Gem", 250, 0, true, 1), 0);
  EXPECT_EQ(LN_InventoryAdd(stacked, "Apple", 1, 0, true, 1), 1);
  EXPECT_EQ(LN_InventoryAdd(stacked, "Gem", 5, 0, true, 1), 0);
  EXPECT_EQ(Contents(stacked), (Contents_t{{"Gem", 255}}));
}

TEST(LN_Inventory, RemovesNewestStacksFirstAndDropsEmptySlots)
{
  std::vector<LN_Value> slots;
  LN_InventoryAdd(slots, "Apple", 6, 4, true, 0);
  LN_InventoryAdd(slots, "Sword", 1, 0, true, 0);
  EXPECT_EQ(LN_InventoryRemoveItem(slots, "Apple", 3), 3);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 3}, {"Sword", 1}}));
  EXPECT_EQ(LN_InventoryRemoveFromSlot(slots, 1, 5), 1);
  EXPECT_EQ(LN_InventoryRemoveFromSlot(slots, 7, 1), 0);
  EXPECT_EQ(LN_InventoryRemoveItem(slots, "Gem", 1), 0);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 3}}));
}

TEST(LN_Inventory, SortsSwapsAndSplits)
{
  std::vector<LN_Value> slots;
  LN_InventoryAdd(slots, "Sword", 1, 0, true, 0);
  LN_InventoryAdd(slots, "Apple", 5, 0, true, 0);
  LN_InventoryAdd(slots, "Gem", 2, 0, true, 0);
  LN_InventorySort(slots, LN_InventorySortKey::Item, false);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 5}, {"Gem", 2}, {"Sword", 1}}));
  LN_InventorySort(slots, LN_InventorySortKey::Count, true);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 5}, {"Gem", 2}, {"Sword", 1}}));
  EXPECT_TRUE(LN_InventorySwap(slots, 0, 2));
  EXPECT_FALSE(LN_InventorySwap(slots, 0, 3));
  EXPECT_EQ(Contents(slots), (Contents_t{{"Sword", 1}, {"Gem", 2}, {"Apple", 5}}));
  EXPECT_TRUE(LN_InventorySplit(slots, 2, 2));
  EXPECT_FALSE(LN_InventorySplit(slots, 0, 1));
  EXPECT_EQ(Contents(slots), (Contents_t{{"Sword", 1}, {"Gem", 2}, {"Apple", 3}, {"Apple", 2}}));
}

TEST(LN_Inventory, PlainNamesCountAsOneItem)
{
  LN_Value name;
  name.type = LN_ValueType::String;
  name.exists = true;
  name.string_value = "Key";
  std::vector<LN_Value> slots = {name};
  EXPECT_EQ(LN_InventoryCount(slots, "Key"), 1);
  EXPECT_EQ(LN_InventoryRemoveItem(slots, "Key", 1), 1);
  EXPECT_TRUE(slots.empty());
}

namespace {

LN_Value Text(const std::string &text)
{
  LN_Value value;
  value.type = LN_ValueType::String;
  value.exists = true;
  value.string_value = text;
  return value;
}

std::vector<LN_Value> Slots(const Contents_t &contents)
{
  std::vector<LN_Value> slots;
  for (const auto &[item, count] : contents) {
    slots.push_back(LN_MakeInventoryEntry(item, count));
  }
  return slots;
}

std::vector<LN_Recipe> Recipes(const std::vector<std::string> &texts)
{
  LN_Value list;
  list.type = LN_ValueType::List;
  list.exists = true;
  for (const std::string &text : texts) {
    list.list_value.push_back(Text(text));
  }
  std::vector<LN_Recipe> recipes;
  LN_ParseRecipes(list, recipes);
  return recipes;
}

int32_t StackOfFive(const std::string & /*item*/, const void * /*data*/)
{
  return 5;
}

}  // namespace

TEST(LN_Inventory, ParsesRecipeTextWithAmounts)
{
  LN_Recipe recipe;
  ASSERT_TRUE(LN_ParseRecipe("Apple + Mushroom = Potion", recipe));
  EXPECT_EQ(recipe.ingredients.size(), 2u);
  EXPECT_EQ(recipe.ingredients[0].item, "Apple");
  EXPECT_EQ(recipe.ingredients[1].item, "Mushroom");
  EXPECT_EQ(recipe.results[0].item, "Potion");
  EXPECT_EQ(recipe.results[0].count, 1);

  /* Amounts before or after the name, names with spaces, and other arrows. */
  ASSERT_TRUE(LN_ParseRecipe("  2 Wood +Iron Ore x3 , Wood -> 2x Health Potion ", recipe));
  ASSERT_EQ(recipe.ingredients.size(), 2u);
  EXPECT_EQ(recipe.ingredients[0].item, "Wood");
  EXPECT_EQ(recipe.ingredients[0].count, 3);
  EXPECT_EQ(recipe.ingredients[1].item, "Iron Ore");
  EXPECT_EQ(recipe.ingredients[1].count, 3);
  EXPECT_EQ(recipe.results[0].item, "Health Potion");
  EXPECT_EQ(recipe.results[0].count, 2);
  ASSERT_TRUE(LN_ParseRecipe("Coin \xC3\x97" "10 => Gem", recipe));
  EXPECT_EQ(recipe.ingredients[0].count, 10);
  EXPECT_EQ(recipe.results[0].item, "Gem");
  /* A name may start with a number. */
  ASSERT_TRUE(LN_ParseRecipe("9mm Shell + Powder = 9mm Ammo", recipe));
  EXPECT_EQ(recipe.ingredients[0].item, "9mm Shell");
  EXPECT_EQ(recipe.ingredients[0].count, 1);

  /* A times sign may stand apart, and "×" also goes before the name. */
  ASSERT_TRUE(LN_ParseRecipe("2 x Wood + Stone x 3 = 2\xC3\x97 Plank", recipe));
  EXPECT_EQ(recipe.ingredients[0].item, "Wood");
  EXPECT_EQ(recipe.ingredients[0].count, 2);
  EXPECT_EQ(recipe.ingredients[1].item, "Stone");
  EXPECT_EQ(recipe.ingredients[1].count, 3);
  EXPECT_EQ(recipe.results[0].item, "Plank");
  EXPECT_EQ(recipe.results[0].count, 2);
  ASSERT_TRUE(LN_ParseRecipe("2 xylophone = Wood X Large", recipe));
  EXPECT_EQ(recipe.ingredients[0].item, "xylophone");
  EXPECT_EQ(recipe.results[0].item, "Wood X Large");

  for (const char *broken : {"Apple + Mushroom", "= Potion", "Apple + = Potion", "Apple =",
                             "0 Apple = Potion", "Apple = 0 Potion", "Wood x0 = Plank",
                             "A <= B", "A == B", "A = B + ", "A = B @ Bench @ Two"})
  {
    EXPECT_FALSE(LN_ParseRecipe(broken, recipe)) << broken;
  }
  /* A line that is not a recipe leaves nothing half read. */
  EXPECT_FALSE(LN_ParseRecipe("Apple + = Potion @ Kitchen", recipe));
  EXPECT_TRUE(recipe.ingredients.empty());
  EXPECT_TRUE(recipe.results.empty());
  EXPECT_TRUE(recipe.station.empty());
}

TEST(LN_Inventory, AGridShowsARecipeAsWhatItMakes)
{
  std::string item;
  int32_t count = 0;
  ASSERT_TRUE(LN_InventoryShownEntry(Text("2 Wood + Stone = 3 Arrow"), item, count));
  EXPECT_EQ(item, "Arrow");
  EXPECT_EQ(count, 3);
  /* Text that only looks like a recipe stays a name. */
  ASSERT_TRUE(LN_InventoryShownEntry(Text("A = "), item, count));
  EXPECT_EQ(item, "A = ");
  /* In an inventory a recipe line is just a name: removing it removes the line. */
  ASSERT_TRUE(LN_InventoryEntry(Text("Wood = 4 Plank"), item, count));
  EXPECT_EQ(item, "Wood = 4 Plank");
  EXPECT_EQ(count, 1);
  std::vector<LN_Value> slots = {Text("Wood = 4 Plank"), Text("Gem")};
  EXPECT_EQ(LN_InventoryRemoveFromSlot(slots, 0, 2), 1);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Gem", 1}}));
}

TEST(LN_Inventory, RecipeMatchesExactlyItsItemsInAnyOrder)
{
  LN_Recipe recipe;
  ASSERT_TRUE(LN_ParseRecipe("2 Wood + Stone = Axe", recipe));
  EXPECT_TRUE(LN_RecipeMatches(recipe, {{"Stone", 1}, {"Wood", 2}}));
  EXPECT_TRUE(LN_RecipeMatches(recipe, {{"Wood", 1}, {"Stone", 4}, {"Wood", 1}}));
  EXPECT_FALSE(LN_RecipeMatches(recipe, {{"Wood", 1}, {"Stone", 1}}));
  EXPECT_FALSE(LN_RecipeMatches(recipe, {{"Wood", 2}}));
  EXPECT_FALSE(LN_RecipeMatches(recipe, {{"Wood", 2}, {"Stone", 1}, {"Gem", 1}}));

  /* A list skips what is not a recipe but remembers where each recipe was. */
  LN_Value list;
  list.type = LN_ValueType::List;
  list.exists = true;
  list.list_value = {Text("nonsense"), Text("Apple + Mushroom = Potion")};
  std::vector<LN_Recipe> recipes;
  std::vector<int32_t> indices;
  LN_ParseRecipes(list, recipes, &indices);
  ASSERT_EQ(recipes.size(), 1u);
  EXPECT_EQ(indices, std::vector<int32_t>{1});
}

TEST(LN_Inventory, MoveToSlotStacksTheSameItemAndKeepsTheRest)
{
  std::string made;
  std::vector<LN_Value> slots = Slots({{"Apple", 4}, {"Gem", 1}, {"Apple", 3}});
  /* Stack limit 5: 2 of the 4 fit, 2 stay. */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 2, {}, StackOfFive, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 2}, {"Gem", 1}, {"Apple", 5}}));
  /* The target is full now: nothing happens. */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 2, {}, StackOfFive, nullptr, true, 0, true, made),
            LN_InventoryOutcome::None);
  /* Without a limit all of it moves and the emptied slot goes. */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 2, 0, {}, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 7}, {"Gem", 1}}));
  EXPECT_TRUE(made.empty());
}

TEST(LN_Inventory, MoveToSlotCombinesByRecipeOrSwaps)
{
  const std::vector<LN_Recipe> recipes = Recipes(
      {"Apple + Mushroom = Potion", "2 Wood + Stone = Axe"});
  std::string made;
  /* Both slots empty: the result takes the dropped-on slot. */
  std::vector<LN_Value> slots = Slots({{"Mushroom", 1}, {"Sword", 1}, {"Apple", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 2, 0, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(made, "Potion");
  EXPECT_EQ(Contents(slots), (Contents_t{{"Potion", 1}, {"Sword", 1}}));

  /* Only the dropped slot empties: the result takes its place. */
  slots = Slots({{"Wood", 2}, {"Gem", 1}, {"Stone", 3}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 2, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Axe", 1}, {"Gem", 1}, {"Stone", 2}}));

  /* Neither empties: the result is added like Add Item, and must fit. */
  slots = Slots({{"Wood", 5}, {"Stone", 2}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 1, 0, recipes, nullptr, nullptr, true, 2, true, made),
            LN_InventoryOutcome::NoRoom);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 5}, {"Stone", 2}}));
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 1, 0, recipes, nullptr, nullptr, true, 3, true, made),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 3}, {"Stone", 1}, {"Axe", 1}}));

  /* Not enough for the recipe, or no recipe: the slots swap, unless Swap is off. */
  slots = Slots({{"Wood", 1}, {"Stone", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 1, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Swapped);
  EXPECT_TRUE(made.empty());
  EXPECT_EQ(Contents(slots), (Contents_t{{"Stone", 1}, {"Wood", 1}}));
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 1, recipes, nullptr, nullptr, true, 0, false, made),
            LN_InventoryOutcome::None);
  /* A place that holds no item takes the slot. */
  slots.push_back(LN_Value());
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 2, recipes, nullptr, nullptr, true, 0, false, made),
            LN_InventoryOutcome::Moved);
  EXPECT_FALSE(slots[0].exists);
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 0, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::None);
  /* Past the end (an empty cell of a grid that shows them) the slot moves to the end, where the
   * last one already is; a negative place is none. */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 1, 9, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Moved);
  std::string last_item;
  int32_t last_count = 0;
  ASSERT_TRUE(LN_InventoryEntry(slots[2], last_item, last_count));
  EXPECT_EQ(last_item, "Wood");
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 2, 9, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::None);
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 1, -1, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::None);
}

TEST(LN_Inventory, MoveToInventoryMovesStacksOrSwapsBetweenTwoInventories)
{
  std::vector<LN_Value> bag = Slots({{"Apple", 4}, {"Sword", 1}, {"Gem", 2}});
  std::vector<LN_Value> chest = Slots({{"Apple", 3}});
  /* Onto the same item: as many as fit (limit 5) join it, the rest stays in the bag. */
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 0, chest, 0, StackOfFive, nullptr, true, 0, true),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 2}, {"Sword", 1}, {"Gem", 2}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Apple", 5}}));
  /* Onto an empty place (past the end): the whole stack moves. */
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 2, chest, 7, nullptr, nullptr, true, 0, true),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 2}, {"Sword", 1}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Apple", 5}, {"Gem", 2}}));
  /* A full destination (Capacity 2) takes nothing new, but two items still swap. */
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 1, chest, -1, nullptr, nullptr, true, 2, true),
            LN_InventoryOutcome::NoRoom);
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 1, chest, 1, nullptr, nullptr, true, 2, true),
            LN_InventoryOutcome::Swapped);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 2}, {"Gem", 2}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Apple", 5}, {"Sword", 1}}));
  /* Swap off, a full stack, or a bad slot: nothing changes. */
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 1, chest, 1, nullptr, nullptr, true, 0, false),
            LN_InventoryOutcome::None);
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 0, chest, 0, StackOfFive, nullptr, true, 0, true),
            LN_InventoryOutcome::None);
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 5, chest, 0, nullptr, nullptr, true, 0, true),
            LN_InventoryOutcome::None);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 2}, {"Gem", 2}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Apple", 5}, {"Sword", 1}}));
  /* Onto an empty place the stack first joins that item's stacks: a full one (limit 5) takes
   * none, so a new slot holds it ... */
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 0, chest, -1, StackOfFive, nullptr, true, 0, true),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Gem", 2}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Apple", 5}, {"Sword", 1}, {"Apple", 2}}));
  /* ... and without a limit the whole stack joins, leaving no new slot. */
  EXPECT_EQ(LN_InventoryMoveToInventory(chest, 2, bag, -1, nullptr, nullptr, true, 0, true),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(LN_InventoryMoveToInventory(chest, 0, bag, -1, nullptr, nullptr, true, 0, true),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Gem", 2}, {"Apple", 7}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Sword", 1}}));
  /* A full inventory still takes what fits on its stacks; the rest stays. */
  std::vector<LN_Value> pouch = Slots({{"Apple", 4}});
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 1, pouch, -1, StackOfFive, nullptr, true, 1, true),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Gem", 2}, {"Apple", 6}}));
  EXPECT_EQ(Contents(pouch), (Contents_t{{"Apple", 5}}));
}

TEST(LN_Inventory, ASplitStackMovesOnlyItsPartAndNeverSwaps)
{
  const std::vector<LN_Recipe> recipes = Recipes(
      {"Apple + Mushroom = Potion", "2 Wood + Stone = Axe"});
  std::string made;
  std::vector<LN_Value> slots = Slots({{"Apple", 5}, {"Gem", 1}});
  /* Past the end: the part becomes a new stack there, the rest stays. */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 9, {}, nullptr, nullptr, true, 0, true, made, 2),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 3}, {"Gem", 1}, {"Apple", 2}}));
  /* Onto the same item: the part joins it, up to the stack limit. */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 2, {}, StackOfFive, nullptr, true, 0, true, made, 2),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 1}, {"Gem", 1}, {"Apple", 4}}));
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 2, 0, {}, StackOfFive, nullptr, true, 0, true, made, 3),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 4}, {"Gem", 1}, {"Apple", 1}}));
  /* Onto another item: a part has no slot to swap into, so nothing changes. */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 1, {}, nullptr, nullptr, true, 0, true, made, 2),
            LN_InventoryOutcome::None);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 4}, {"Gem", 1}, {"Apple", 1}}));
  /* An amount of all of it, or more, moves the whole stack (which swaps). */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 1, {}, nullptr, nullptr, true, 0, true, made, 4),
            LN_InventoryOutcome::Swapped);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Gem", 1}, {"Apple", 4}, {"Apple", 1}}));
  /* A full inventory has no room for a new stack; an empty place takes the part. */
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 1, 9, {}, nullptr, nullptr, true, 3, true, made, 1),
            LN_InventoryOutcome::NoRoom);
  slots.push_back(LN_Value());
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 1, 3, {}, nullptr, nullptr, true, 0, true, made, 1),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Gem", 1}, {"Apple", 3}, {"Apple", 1}, {"Apple", 1}}));

  /* A recipe matches the part: one Apple of five makes a Potion, four stay. */
  slots = Slots({{"Apple", 5}, {"Mushroom", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 1, recipes, nullptr, nullptr, true, 0, true, made, 1),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(made, "Potion");
  EXPECT_EQ(Contents(slots), (Contents_t{{"Apple", 4}, {"Potion", 1}}));
  /* One Wood is not enough for an Axe (two): nothing happens. */
  slots = Slots({{"Wood", 5}, {"Stone", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 1, recipes, nullptr, nullptr, true, 0, true, made, 1),
            LN_InventoryOutcome::None);
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 1, recipes, nullptr, nullptr, true, 0, true, made, 2),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 3}, {"Axe", 1}}));

  /* The part keeps what else its slot says (a quality, durability...). */
  slots = Slots({{"Sword", 2}});
  LN_Value &quality = slots[0].dict_value["quality"];
  quality.type = LN_ValueType::String;
  quality.exists = true;
  quality.string_value = "rare";
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 5, {}, nullptr, nullptr, true, 0, true, made, 1),
            LN_InventoryOutcome::Moved);
  ASSERT_EQ(slots.size(), size_t(2));
  EXPECT_EQ(slots[1].dict_value["quality"].string_value, "rare");
}

TEST(LN_Inventory, ASplitStackMovesOnlyItsPartToAnotherInventory)
{
  std::vector<LN_Value> bag = Slots({{"Apple", 6}, {"Sword", 1}});
  std::vector<LN_Value> chest = Slots({{"Apple", 3}, {"Gem", 1}});
  /* Onto the same item: the part joins it; the rest stays in the bag. */
  EXPECT_EQ(
      LN_InventoryMoveToInventory(bag, 0, chest, 0, StackOfFive, nullptr, true, 0, true, 1),
      LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 5}, {"Sword", 1}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Apple", 4}, {"Gem", 1}}));
  /* Onto an empty place: it fills that item's stacks first (one fits), the rest is a new stack. */
  EXPECT_EQ(
      LN_InventoryMoveToInventory(bag, 0, chest, -1, StackOfFive, nullptr, true, 0, true, 3),
      LN_InventoryOutcome::Moved);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 2}, {"Sword", 1}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Apple", 5}, {"Gem", 1}, {"Apple", 2}}));
  /* Onto another item: no swap for a part. */
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 0, chest, 1, nullptr, nullptr, true, 0, true, 1),
            LN_InventoryOutcome::None);
  /* A full destination: what fits on its stacks goes, else nothing. */
  std::vector<LN_Value> pouch = Slots({{"Apple", 4}});
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 0, pouch, -1, StackOfFive, nullptr, true, 1, true, 1),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(LN_InventoryMoveToInventory(bag, 0, pouch, -1, StackOfFive, nullptr, true, 1, true, 1),
            LN_InventoryOutcome::NoRoom);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 1}, {"Sword", 1}}));
  EXPECT_EQ(Contents(pouch), (Contents_t{{"Apple", 5}}));
  /* A place in the list that holds no item takes the part. */
  std::vector<LN_Value> box = Slots({{"Gem", 1}});
  box.push_back(LN_Value());
  std::vector<LN_Value> sack = Slots({{"Coin", 10}});
  EXPECT_EQ(LN_InventoryMoveToInventory(sack, 0, box, 1, nullptr, nullptr, true, 0, true, 4),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Contents(sack), (Contents_t{{"Coin", 6}}));
  EXPECT_EQ(Contents(box), (Contents_t{{"Gem", 1}, {"Coin", 4}}));
}

/** A list with its empty places shown as {"", 0}. */
static Contents_t Places(const std::vector<LN_Value> &slots)
{
  Contents_t places;
  for (const LN_Value &slot : slots) {
    std::string item;
    int32_t count = 0;
    if (LN_InventoryIsEmptyPlace(slot) || !LN_InventoryEntry(slot, item, count)) {
      places.emplace_back("", 0);
    }
    else {
      places.emplace_back(item, count);
    }
  }
  return places;
}

TEST(LN_Inventory, KeepPlacesLeavesGapsAndPutsADropWhereItLands)
{
  constexpr LN_InventoryLayout keep = LN_InventoryLayout::KeepPlaces;
  std::string made;
  std::vector<LN_Value> slots = Slots({{"Apple", 2}, {"Gem", 1}, {"Coin", 5}});
  /* Onto a place past the end: there, the places before it empty; nothing moves up. */
  EXPECT_EQ(
      LN_InventoryMoveToSlot(slots, 0, 5, {}, nullptr, nullptr, true, 0, true, made, 0, keep),
      LN_InventoryOutcome::Moved);
  EXPECT_EQ(Places(slots),
            (Contents_t{{"", 0}, {"Gem", 1}, {"Coin", 5}, {"", 0}, {"", 0}, {"Apple", 2}}));
  /* An emptied slot stays an empty place. */
  EXPECT_EQ(LN_InventoryRemoveFromSlot(slots, 1, 1, keep), 1);
  EXPECT_EQ(Places(slots),
            (Contents_t{{"", 0}, {"", 0}, {"Coin", 5}, {"", 0}, {"", 0}, {"Apple", 2}}));
  /* Add fills the first empty place (also in a packed list, which has none). */
  EXPECT_EQ(LN_InventoryAdd(slots, "Sword", 1, 0, true, 0), 0);
  EXPECT_EQ(Places(slots),
            (Contents_t{{"Sword", 1}, {"", 0}, {"Coin", 5}, {"", 0}, {"", 0}, {"Apple", 2}}));
  /* Onto an empty place: the stack goes there and leaves one behind; the empty places at the end
   * go. */
  EXPECT_EQ(
      LN_InventoryMoveToSlot(slots, 5, 1, {}, nullptr, nullptr, true, 0, true, made, 0, keep),
      LN_InventoryOutcome::Moved);
  EXPECT_EQ(Places(slots), (Contents_t{{"Sword", 1}, {"Apple", 2}, {"Coin", 5}}));
  /* A part onto a place past the end; a place past Capacity has no room. */
  EXPECT_EQ(
      LN_InventoryMoveToSlot(slots, 2, 4, {}, nullptr, nullptr, true, 0, true, made, 3, keep),
      LN_InventoryOutcome::Moved);
  EXPECT_EQ(Places(slots),
            (Contents_t{{"Sword", 1}, {"Apple", 2}, {"Coin", 2}, {"", 0}, {"Coin", 3}}));
  EXPECT_EQ(
      LN_InventoryMoveToSlot(slots, 0, 9, {}, nullptr, nullptr, true, 6, true, made, 0, keep),
      LN_InventoryOutcome::NoRoom);
  /* Stacking empties the dragged slot: an empty place stays. */
  EXPECT_EQ(
      LN_InventoryMoveToSlot(slots, 2, 4, {}, nullptr, nullptr, true, 0, true, made, 0, keep),
      LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Places(slots), (Contents_t{{"Sword", 1}, {"Apple", 2}, {"", 0}, {"", 0}, {"Coin", 5}}));
  /* Remove Item, newest first: the emptied end goes. */
  EXPECT_EQ(LN_InventoryRemoveItem(slots, "Coin", 5, keep), 5);
  EXPECT_EQ(Places(slots), (Contents_t{{"Sword", 1}, {"Apple", 2}}));
  /* Split: the part takes the nearest empty place after the slot. */
  slots = Slots({{"Coin", 4}, {"Gem", 1}});
  slots.insert(slots.begin() + 1, LN_Value());
  EXPECT_TRUE(LN_InventorySplit(slots, 0, 1, keep));
  EXPECT_EQ(Places(slots), (Contents_t{{"Coin", 3}, {"Coin", 1}, {"Gem", 1}}));
  EXPECT_TRUE(LN_InventorySplit(slots, 0, 1, keep));
  EXPECT_EQ(Places(slots), (Contents_t{{"Coin", 2}, {"Coin", 1}, {"Gem", 1}, {"Coin", 1}}));
  EXPECT_FALSE(LN_InventorySplit(slots, 0, 1, keep, 4));
  /* Sort packs the items and the empty places go. */
  slots = Slots({{"Sword", 1}, {"Apple", 1}});
  slots.insert(slots.begin() + 1, LN_Value());
  LN_InventorySort(slots, LN_InventorySortKey::Item, false, keep);
  EXPECT_EQ(Places(slots), (Contents_t{{"Apple", 1}, {"Sword", 1}}));
  /* A recipe's emptied slot stays an empty place. */
  slots = Slots({{"Apple", 1}, {"Gem", 1}, {"Mushroom", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots,
                                   0,
                                   2,
                                   Recipes({"Apple + Mushroom = Potion"}),
                                   nullptr,
                                   nullptr,
                                   true,
                                   0,
                                   true,
                                   made,
                                   0,
                                   keep),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(Places(slots), (Contents_t{{"", 0}, {"Gem", 1}, {"Potion", 1}}));
}

TEST(LN_Inventory, KeepPlacesBetweenTwoInventoriesFollowsEachListsLayout)
{
  constexpr LN_InventoryLayout keep = LN_InventoryLayout::KeepPlaces;
  constexpr LN_InventoryLayout packed = LN_InventoryLayout::Packed;
  std::vector<LN_Value> bag = Slots({{"Sword", 1}, {"Apple", 2}, {"Gem", 1}});
  std::vector<LN_Value> chest = Slots({{"Coin", 5}});
  /* Into a place past the chest's end: there. The bag keeps an empty place. */
  EXPECT_EQ(LN_InventoryMoveToInventory(
                bag, 0, chest, 3, nullptr, nullptr, true, 0, true, 0, keep, keep),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Places(bag), (Contents_t{{"", 0}, {"Apple", 2}, {"Gem", 1}}));
  EXPECT_EQ(Places(chest), (Contents_t{{"Coin", 5}, {"", 0}, {"", 0}, {"Sword", 1}}));
  /* Onto the chest's background: its first empty place. */
  EXPECT_EQ(LN_InventoryMoveToInventory(
                bag, 2, chest, -1, nullptr, nullptr, true, 0, true, 0, keep, keep),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Places(bag), (Contents_t{{"", 0}, {"Apple", 2}}));
  EXPECT_EQ(Places(chest), (Contents_t{{"Coin", 5}, {"Gem", 1}, {"", 0}, {"Sword", 1}}));
  /* A packed source closes its gap; a full Keep Places destination has no place past Capacity. */
  std::vector<LN_Value> pouch = Slots({{"Coin", 1}, {"Gem", 1}});
  EXPECT_EQ(LN_InventoryMoveToInventory(
                pouch, 0, chest, 2, nullptr, nullptr, false, 0, true, 0, packed, keep),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Places(pouch), (Contents_t{{"Gem", 1}}));
  EXPECT_EQ(Places(chest), (Contents_t{{"Coin", 5}, {"Gem", 1}, {"Coin", 1}, {"Sword", 1}}));
  EXPECT_EQ(LN_InventoryMoveToInventory(
                pouch, 0, chest, 6, nullptr, nullptr, true, 5, true, 0, packed, keep),
            LN_InventoryOutcome::NoRoom);
  EXPECT_EQ(Places(pouch), (Contents_t{{"Gem", 1}}));
}

TEST(LN_Inventory, CraftTakesIngredientsFromAnywhereAndNeedsRoom)
{
  int32_t crafted = 0;
  LN_Recipe axe;
  ASSERT_TRUE(LN_ParseRecipe("3 Wood + Stone = 2 Axe", axe));
  std::vector<LN_Value> slots = Slots({{"Wood", 2}, {"Stone", 1}, {"Wood", 2}});
  EXPECT_EQ(LN_InventoryCraft(slots, axe, nullptr, nullptr, true, 0, 1, crafted),
            LN_InventoryOutcome::Combined);
  /* Newest Wood first; the Stone slot emptied and the Axes stack in a new slot. */
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 1}, {"Axe", 2}}));
  /* Missing an ingredient: nothing changes. */
  EXPECT_EQ(LN_InventoryCraft(slots, axe, nullptr, nullptr, true, 0, 1, crafted),
            LN_InventoryOutcome::Missing);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 1}, {"Axe", 2}}));
  /* The result must fit: a stack holds 5, so 2 more Axes at 4 need a second slot. The used-up
   * ingredients free their slots first. */
  slots = Slots({{"Wood", 3}, {"Stone", 1}, {"Axe", 4}});
  EXPECT_EQ(LN_InventoryCraft(slots, axe, StackOfFive, nullptr, true, 1, 1, crafted),
            LN_InventoryOutcome::NoRoom);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 3}, {"Stone", 1}, {"Axe", 4}}));
  EXPECT_EQ(LN_InventoryCraft(slots, axe, StackOfFive, nullptr, true, 2, 1, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Axe", 5}, {"Axe", 1}}));
  EXPECT_STREQ(LN_InventoryOutcomeName(LN_InventoryOutcome::Combined, true), "Crafted");
  EXPECT_STREQ(LN_InventoryOutcomeName(LN_InventoryOutcome::Combined, false), "Combined");
  EXPECT_STREQ(LN_InventoryOutcomeName(LN_InventoryOutcome::NoRoom, false), "No Room");
  EXPECT_STREQ(LN_InventoryOutcomeName(LN_InventoryOutcome::None, false), "");
}

TEST(LN_Inventory, ItemsThatDoNotStackCombineByASameItemRecipe)
{
  /* Resident Evil style: herbs do not stack (limit 1), but two make a mixed herb. */
  const std::vector<LN_Recipe> recipes = Recipes({"2 Herb = Mixed Herb"});
  const LN_MaxStackFn one = [](const std::string & /*item*/, const void * /*data*/) {
    return 1;
  };
  std::string made;
  std::vector<LN_Value> slots = Slots({{"Herb", 1}, {"Gem", 1}, {"Herb", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 2, 0, recipes, one, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(made, "Mixed Herb");
  EXPECT_EQ(Contents(slots), (Contents_t{{"Mixed Herb", 1}, {"Gem", 1}}));
  /* Items that stack stack first; with no recipe, two full stacks of one item stay. */
  slots = Slots({{"Herb", 1}, {"Herb", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 1, 0, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Herb", 2}}));
  slots = Slots({{"Gem", 1}, {"Gem", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 1, 0, recipes, one, nullptr, true, 0, true, made),
            LN_InventoryOutcome::None);
  /* A recipe of one item is for crafting: two full stacks dropped together stay. */
  slots = Slots({{"Herb", 5}, {"Herb", 5}});
  EXPECT_EQ(LN_InventoryMoveToSlot(
                slots, 1, 0, Recipes({"Herb = Dried Herb"}), StackOfFive, nullptr, true, 0, true,
                made),
            LN_InventoryOutcome::None);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Herb", 5}, {"Herb", 5}}));
}

TEST(LN_Inventory, ACombinedResultFollowsStackOff)
{
  std::string made;
  std::vector<LN_Value> slots = Slots({{"Wood", 1}, {"Saw", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(
                slots, 0, 1, Recipes({"Wood + Saw = 4 Plank"}), nullptr, nullptr, false, 0, true,
                made),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Plank", 1}, {"Plank", 1}, {"Plank", 1}, {"Plank", 1}}));
}

TEST(LN_Inventory, RecipesWithAStationAreNotMadeByDropping)
{
  LN_Recipe recipe;
  ASSERT_TRUE(LN_ParseRecipe("2 Wood + Stone = Axe @ Work Bench", recipe));
  EXPECT_EQ(recipe.station, "Work Bench");
  EXPECT_EQ(recipe.results[0].item, "Axe");
  EXPECT_FALSE(LN_ParseRecipe("Wood = Axe @", recipe));
  const std::vector<LN_Recipe> recipes = Recipes({"Wood + Stone = Axe @Bench"});
  std::string made;
  int32_t crafted = 0;
  std::vector<LN_Value> slots = Slots({{"Wood", 1}, {"Stone", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots, 0, 1, recipes, nullptr, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Swapped);
  /* Craft makes it: the station is for the game to check. */
  EXPECT_EQ(LN_InventoryCraft(slots, recipes[0], nullptr, nullptr, true, 0, 1, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Axe", 1}}));
}

TEST(LN_Inventory, ARecipeCanMakeSeveralThings)
{
  LN_Recipe recipe;
  ASSERT_TRUE(LN_ParseRecipe("Milk Bucket = Butter + Bucket", recipe));
  ASSERT_EQ(recipe.results.size(), 2u);
  EXPECT_EQ(recipe.results[0].item, "Butter");
  EXPECT_EQ(recipe.results[1].item, "Bucket");
  ASSERT_TRUE(LN_ParseRecipe("Log = 4 Plank, 2 Stick + Plank @ Saw", recipe));
  ASSERT_EQ(recipe.results.size(), 2u);
  EXPECT_EQ(recipe.results[0].count, 5);
  EXPECT_EQ(recipe.results[1].count, 2);
  EXPECT_EQ(recipe.station, "Saw");
  EXPECT_FALSE(LN_ParseRecipe("Milk Bucket = Butter +", recipe));

  /* A grid shows the first one. */
  std::string item;
  int32_t count = 0;
  ASSERT_TRUE(LN_InventoryShownEntry(Text("Log = 4 Plank + Stick"), item, count));
  EXPECT_EQ(item, "Plank");
  EXPECT_EQ(count, 4);

  /* Crafting adds all of them, or none when one does not fit. */
  int32_t crafted = 0;
  ASSERT_TRUE(LN_ParseRecipe("Milk Bucket = Butter + Bucket", recipe));
  std::vector<LN_Value> slots = Slots({{"Milk Bucket", 2}});
  EXPECT_EQ(LN_InventoryCraft(slots, recipe, nullptr, nullptr, true, 0, 0, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 2);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Butter", 2}, {"Bucket", 2}}));
  slots = Slots({{"Milk Bucket", 1}});
  EXPECT_EQ(LN_InventoryCraft(slots, recipe, nullptr, nullptr, true, 1, 1, crafted),
            LN_InventoryOutcome::NoRoom);
  EXPECT_EQ(crafted, 0);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Milk Bucket", 1}}));

  /* Dropping one on the other: the results take the emptied slots, the dropped-on one first. */
  std::string made;
  slots = Slots({{"Milk Bucket", 1}, {"Gem", 1}, {"Sugar", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(slots,
                                   0,
                                   2,
                                   Recipes({"Milk Bucket + Sugar = Butter + Bucket"}),
                                   nullptr,
                                   nullptr,
                                   true,
                                   0,
                                   true,
                                   made),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(made, "Butter");
  EXPECT_EQ(Contents(slots), (Contents_t{{"Bucket", 1}, {"Gem", 1}, {"Butter", 1}}));
}

TEST(LN_Inventory, CraftMakesARecipeManyTimes)
{
  LN_Recipe axe;
  ASSERT_TRUE(LN_ParseRecipe("2 Wood + Stone = Axe", axe));
  std::vector<LN_Value> slots = Slots({{"Wood", 10}, {"Stone", 3}});
  EXPECT_EQ(LN_RecipeCraftable(slots, axe), 3);
  int32_t crafted = 0;
  EXPECT_EQ(LN_InventoryCraft(slots, axe, nullptr, nullptr, true, 0, 2, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 2);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 6}, {"Stone", 1}, {"Axe", 2}}));
  /* More times than the ingredients allow: as many as they do. 0: as many as possible. */
  EXPECT_EQ(LN_InventoryCraft(slots, axe, nullptr, nullptr, true, 0, 5, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 1);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 4}, {"Axe", 3}}));
  EXPECT_EQ(LN_RecipeCraftable(slots, axe), 0);
  EXPECT_EQ(LN_InventoryCraft(slots, axe, nullptr, nullptr, true, 0, 0, crafted),
            LN_InventoryOutcome::Missing);
  EXPECT_EQ(crafted, 0);
  EXPECT_EQ(LN_InventoryCraft(slots, axe, nullptr, nullptr, true, 0, -1, crafted),
            LN_InventoryOutcome::None);

  /* Room runs out first: it makes the most that fit. One Plank per slot, 4 slots: the Log
   * stack keeps one, so 3 Planks. */
  LN_Recipe plank;
  ASSERT_TRUE(LN_ParseRecipe("Log = Plank", plank));
  slots = Slots({{"Log", 10}});
  EXPECT_EQ(LN_InventoryCraft(slots, plank, nullptr, nullptr, false, 4, 0, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 3);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Log", 7}, {"Plank", 1}, {"Plank", 1}, {"Plank", 1}}));
  EXPECT_EQ(LN_InventoryCraft(slots, plank, nullptr, nullptr, false, 4, 0, crafted),
            LN_InventoryOutcome::NoRoom);

  /* Huge amounts stay valid numbers. */
  LN_Recipe gem;
  ASSERT_TRUE(LN_ParseRecipe("Coin = 1000 Gem", gem));
  slots = Slots({{"Coin", 2000000000}});
  EXPECT_EQ(LN_InventoryCraft(slots, gem, nullptr, nullptr, true, 0, 0, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, INT32_MAX / 1000);
  EXPECT_EQ(LN_InventoryCount(slots, "Gem"), (INT32_MAX / 1000) * 1000);
}

TEST(LN_Inventory, AToolARecipeGivesBackIsOnlyNeededAtHand)
{
  LN_Recipe butter;
  ASSERT_TRUE(LN_ParseRecipe("Bucket + Milk = Butter + Bucket", butter));
  std::vector<LN_Value> slots = Slots({{"Bucket", 1}, {"Milk", 5}});
  EXPECT_EQ(LN_RecipeCraftable(slots, butter), 5);
  int32_t crafted = 0;
  EXPECT_EQ(LN_InventoryCraft(slots, butter, nullptr, nullptr, true, 0, 0, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 5);
  /* The Bucket stays in its slot. */
  EXPECT_EQ(Contents(slots), (Contents_t{{"Bucket", 1}, {"Butter", 5}}));
  /* Without the tool nothing is made. */
  slots = Slots({{"Milk", 5}});
  EXPECT_EQ(LN_RecipeCraftable(slots, butter), 0);
  EXPECT_EQ(LN_InventoryCraft(slots, butter, nullptr, nullptr, true, 0, 0, crafted),
            LN_InventoryOutcome::Missing);
}

TEST(LN_Inventory, CraftingManyIsLikeCraftingOneAtATime)
{
  int32_t crafted = 0;
  /* A full inventory: the first Plank has no room, so none is made, however many are asked. */
  const LN_MaxStackFn two = [](const std::string & /*item*/, const void * /*data*/) { return 2; };
  LN_Recipe plank;
  ASSERT_TRUE(LN_ParseRecipe("Wood = Plank", plank));
  std::vector<LN_Value> slots = Slots({{"Wood", 2}, {"Wood", 2}});
  for (const int32_t times : {1, 3, 0}) {
    EXPECT_EQ(LN_InventoryCraft(slots, plank, two, nullptr, true, 2, times, crafted),
              LN_InventoryOutcome::NoRoom);
    EXPECT_EQ(crafted, 0);
  }
  /* A stack that runs out frees its slot for the next results. One slot each (Stack off), room
   * for 3: Wood, Wood, Wood make 3 Planks one after another. */
  slots = Slots({{"Wood", 1}, {"Wood", 1}, {"Wood", 1}});
  EXPECT_EQ(LN_InventoryCraft(slots, plank, nullptr, nullptr, false, 3, 0, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 3);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Plank", 1}, {"Plank", 1}, {"Plank", 1}}));
  /* Two Planks each: the second craft has no room. */
  LN_Recipe planks;
  ASSERT_TRUE(LN_ParseRecipe("Wood = 2 Plank", planks));
  slots = Slots({{"Wood", 1}, {"Wood", 1}, {"Wood", 1}});
  EXPECT_EQ(LN_InventoryCraft(slots, planks, nullptr, nullptr, false, 4, 0, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 1);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Wood", 1}, {"Wood", 1}, {"Plank", 1}, {"Plank", 1}}));
}

TEST(LN_Inventory, CombinedResultsFillTheEmptiedSlotsFirst)
{
  const LN_MaxStackFn two = [](const std::string & /*item*/, const void * /*data*/) { return 2; };
  std::string made;
  std::vector<LN_Value> slots = Slots({{"Wood", 1}, {"Gem", 1}, {"Saw", 1}});
  EXPECT_EQ(LN_InventoryMoveToSlot(
                slots, 0, 2, Recipes({"Wood + Saw = 4 Plank"}), two, nullptr, true, 0, true, made),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Plank", 2}, {"Gem", 1}, {"Plank", 2}}));
}

TEST(LN_Inventory, AnInventoryWithoutCapacityStopsAtTheSlotLimit)
{
  std::vector<LN_Value> slots;
  EXPECT_EQ(LN_InventoryAdd(slots, "Coin", 100000, 0, false, 0), 100000 - 65536);
  EXPECT_EQ(slots.size(), 65536u);
  /* Removing many single slots stays quick and correct. */
  EXPECT_EQ(LN_InventoryRemoveItem(slots, "Coin", 65000), 65000);
  EXPECT_EQ(LN_InventoryCount(slots, "Coin"), 536);
  EXPECT_EQ(slots.size(), 536u);
}

TEST(LN_Inventory, CraftEdgeCases)
{
  int32_t crafted = 0;
  const LN_MaxStackFn ten = [](const std::string & /*item*/, const void * /*data*/) { return 10; };
  /* Already over Capacity (Split Stack can do that): results that need no new slot still fit. */
  LN_Recipe plank;
  ASSERT_TRUE(LN_ParseRecipe("Log = 4 Plank", plank));
  std::vector<LN_Value> slots = Slots({{"Log", 2}, {"Plank", 1}, {"Stone", 4}, {"Iron", 1}});
  ASSERT_TRUE(LN_InventorySplit(slots, 2, 2));
  EXPECT_EQ(LN_InventoryCraft(slots, plank, ten, nullptr, true, 4, 1, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 1);
  EXPECT_EQ(LN_InventoryCount(slots, "Plank"), 5);

  /* A slot of 0 goes when removing reaches it, as crafting one at a time does. */
  LN_Recipe a;
  ASSERT_TRUE(LN_ParseRecipe("4 B = 3 A", a));
  slots = Slots({{"A", 9}, {"B", 8}, {"B", 0}});
  EXPECT_EQ(LN_InventoryCraft(slots, a, nullptr, nullptr, false, 7, 2, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 2);

  /* A recipe that uses nothing up is made once for "as many as possible". */
  LN_Recipe same;
  ASSERT_TRUE(LN_ParseRecipe("Saw = Saw + Plank", same));
  slots = Slots({{"Saw", 1}});
  EXPECT_EQ(LN_InventoryCraft(slots, same, nullptr, nullptr, true, 0, 0, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 1);
  EXPECT_EQ(Contents(slots), (Contents_t{{"Saw", 1}, {"Plank", 1}}));
  EXPECT_EQ(LN_InventoryCraft(slots, same, nullptr, nullptr, true, 0, 3, crafted),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(crafted, 3);
}


/* -------------------------------------------------------------------- */
/** \name Inventory Rules
 * \{ */

namespace {

/** A slot with its own category (no Item Library needed). */
LN_Value CategorizedSlot(const std::string &item, const int32_t count, const std::string &category)
{
  LN_Value slot = LN_MakeInventoryEntry(item, count);
  slot.dict_value["category"] = Text(category);
  return slot;
}

/** The rules of a list with these settings, as a set picked by a list gives them. */
LN_InventoryRules Rules(const bool keep_places = false,
                        const int32_t capacity = 0,
                        const std::string &accept_categories = std::string())
{
  LN_InventoryRuleSet set;
  set.keep_places = keep_places;
  set.capacity = capacity;
  set.accept_categories = accept_categories;
  return LN_ResolveInventoryRules(&set, nullptr);
}

LN_InventoryMove Move(const std::vector<LN_Value> &source,
                      const int32_t from,
                      const std::vector<LN_Value> &target,
                      const int32_t to)
{
  LN_InventoryMove move;
  move.from = from;
  move.from_item = source[size_t(from)];
  move.to = to;
  if (to >= 0 && size_t(to) < target.size()) {
    move.to_item = target[size_t(to)];
  }
  return move;
}

}  // namespace

TEST(LN_Inventory, RulesReadTheirOrFromPropertiesOfTheListsObject)
{
  LN_InventoryRuleSet set;
  set.capacity = 10;
  set.capacity_from = "bag_size";
  set.stack = true;
  set.stack_from = "stack_items";
  set.max_stack = 3;
  set.keep_places_from = "keep";
  set.recipes = "recipes";
  set.accept_categories = "Food, Potion";
  set.accept_categories_from = "missing";

  /* No set: the defaults, the same as an Inventory node's own. */
  const LN_InventoryRules defaults = LN_ResolveInventoryRules(nullptr, nullptr);
  EXPECT_EQ(defaults.capacity, 0);
  EXPECT_TRUE(defaults.stack);
  EXPECT_FALSE(defaults.keep_places);
  EXPECT_TRUE(defaults.accept_categories.empty());

  /* No object: the typed values. */
  const LN_InventoryRules typed = LN_ResolveInventoryRules(&set, nullptr);
  EXPECT_EQ(typed.capacity, 10);
  EXPECT_EQ(typed.max_stack, 3);
  EXPECT_EQ(typed.recipes_property, "recipes");
  EXPECT_EQ(typed.accept_categories, ",food,potion,");

  /* The list's object gives the values it has; what it lacks keeps the typed value. */
  KX_GameObject owner;
  EXP_IntValue *bag_size = new EXP_IntValue(30);
  owner.SetProperty("bag_size", bag_size);
  bag_size->Release();
  EXP_BoolValue *stack_items = new EXP_BoolValue(false);
  owner.SetProperty("stack_items", stack_items);
  stack_items->Release();
  EXP_IntValue *keep = new EXP_IntValue(1);
  owner.SetProperty("keep", keep);
  keep->Release();
  std::vector<std::string> missing;
  const LN_InventoryRules read = LN_ResolveInventoryRules(&set, &owner, &missing);
  EXPECT_EQ(read.capacity, 30);
  EXPECT_FALSE(read.stack);
  EXPECT_TRUE(read.keep_places);
  EXPECT_EQ(read.accept_categories, ",food,potion,");
  EXPECT_EQ(missing, std::vector<std::string>{"missing"});
  owner.RemoveProperty("bag_size");
  owner.RemoveProperty("stack_items");
  owner.RemoveProperty("keep");
}

TEST(LN_Inventory, ApplyMoveInsideOneListStacksSwapsCombinesAndSplits)
{
  const LN_InventoryLookup lookup;
  const LN_InventoryRules rules = Rules();
  std::string made;
  /* The same item stacks. */
  std::vector<LN_Value> bag = Slots({{"Apple", 2}, {"Apple", 3}, {"Gem", 1}});
  EXPECT_EQ(LN_InventoryApplyMove(Move(bag, 0, bag, 1), bag, rules, nullptr, nullptr, {}, lookup, made),
            LN_InventoryOutcome::Stacked);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 5}, {"Gem", 1}}));
  /* Two items swap, unless Swap is off. */
  LN_InventoryMove no_swap = Move(bag, 0, bag, 1);
  no_swap.allow_swap = false;
  EXPECT_EQ(LN_InventoryApplyMove(no_swap, bag, rules, nullptr, nullptr, {}, lookup, made),
            LN_InventoryOutcome::None);
  EXPECT_EQ(LN_InventoryApplyMove(Move(bag, 0, bag, 1), bag, rules, nullptr, nullptr, {}, lookup, made),
            LN_InventoryOutcome::Swapped);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Gem", 1}, {"Apple", 5}}));
  /* A recipe of the list combines them. */
  bag = Slots({{"Apple", 1}, {"Mushroom", 1}});
  EXPECT_EQ(LN_InventoryApplyMove(Move(bag, 0, bag, 1),
                                  bag,
                                  rules,
                                  nullptr,
                                  nullptr,
                                  Recipes({"Apple + Mushroom = Potion"}),
                                  lookup,
                                  made),
            LN_InventoryOutcome::Combined);
  EXPECT_EQ(made, "Potion");
  EXPECT_EQ(Contents(bag), (Contents_t{{"Potion", 1}}));
  /* A part dropped on another item takes the nearest free place. */
  bag = Slots({{"Apple", 4}, {"Gem", 1}});
  LN_InventoryMove part = Move(bag, 0, bag, 1);
  part.amount = 2;
  EXPECT_EQ(LN_InventoryApplyMove(part, bag, rules, nullptr, nullptr, {}, lookup, made),
            LN_InventoryOutcome::Moved);
  EXPECT_EQ(Contents(bag), (Contents_t{{"Apple", 2}, {"Apple", 2}, {"Gem", 1}}));
}

TEST(LN_Inventory, ApplyMoveFindsAMovedStackAgainAndFollowsEachListsRules)
{
  const LN_InventoryLookup lookup;
  std::string made;
  std::vector<LN_Value> bag = Slots({{"Apple", 2}, {"Gem", 1}});
  std::vector<LN_Value> chest = Slots({{"Sword", 1}});
  /* The player saw the Gem at 1; another move put it at 0 meanwhile: it is found by its value. */
  LN_InventoryMove move = Move(bag, 1, chest, 1);
  std::swap(bag[0], bag[1]);
  const LN_InventoryRules bag_rules = Rules(true);
  const LN_InventoryRules chest_rules = Rules();
  EXPECT_EQ(LN_InventoryApplyMove(move, chest, chest_rules, &bag, &bag_rules, {}, lookup, made),
            LN_InventoryOutcome::Moved);
  /* The bag keeps its places (a gap where the Gem was, trimmed at its end: none here). */
  EXPECT_TRUE(LN_InventoryIsEmptyPlace(bag[0]));
  EXPECT_EQ(Contents({bag[1]}), (Contents_t{{"Apple", 2}}));
  EXPECT_EQ(Contents(chest), (Contents_t{{"Sword", 1}, {"Gem", 1}}));
  /* A stack that is gone moves nothing. */
  LN_InventoryMove gone;
  gone.from = 0;
  gone.from_item = LN_MakeInventoryEntry("Key", 1);
  gone.to = 0;
  EXPECT_EQ(LN_InventoryApplyMove(gone, chest, chest_rules, &bag, &bag_rules, {}, lookup, made),
            LN_InventoryOutcome::None);
}

TEST(LN_Inventory, ApplyMoveIntoAFullPlaceGoesIntoTheListUnlessItIsOneCell)
{
  const LN_InventoryLookup lookup;
  const LN_InventoryRules rules = Rules();
  std::string made;
  /* The quick slot's Apple dropped on the bag's Gem: the Gem may not go back (the quick slot's
   * own Accept Categories), so the Apple goes into the bag. */
  std::vector<LN_Value> quick = Slots({{"Apple", 1}});
  std::vector<LN_Value> bag = Slots({{"Gem", 1}});
  LN_InventoryMove move = Move(quick, 0, bag, 0);
  move.swap_back_allowed = false;
  EXPECT_EQ(LN_InventoryApplyMove(move, bag, rules, &quick, &rules, {}, lookup, made),
            LN_InventoryOutcome::Moved);
  EXPECT_TRUE(quick.empty());
  EXPECT_EQ(Contents(bag), (Contents_t{{"Gem", 1}, {"Apple", 1}}));
  /* Into a one-cell list nothing goes beside its item. */
  std::vector<LN_Value> weapon = Slots({{"Sword", 1}});
  LN_InventoryMove into_cell = Move(bag, 1, weapon, 0);
  into_cell.swap_back_allowed = false;
  into_cell.into_list_fallback = false;
  EXPECT_EQ(LN_InventoryApplyMove(into_cell, weapon, rules, &bag, &rules, {}, lookup, made),
            LN_InventoryOutcome::None);
  EXPECT_EQ(Contents(weapon), (Contents_t{{"Sword", 1}}));
}

TEST(LN_Inventory, AListsAcceptCategoriesRefuseWhatItMayNotHold)
{
  const LN_InventoryLookup lookup;
  std::string made;
  const LN_InventoryRules any = Rules();
  const LN_InventoryRules weapons = Rules(false, 0, "Weapon");
  std::vector<LN_Value> bag = {CategorizedSlot("Apple", 1, "food"),
                               CategorizedSlot("Sword", 1, "weapon")};
  std::vector<LN_Value> weapon;
  /* An apple never enters the weapon list, whatever moves it. */
  EXPECT_EQ(LN_InventoryApplyMove(Move(bag, 0, weapon, 0), weapon, weapons, &bag, &any, {}, lookup, made),
            LN_InventoryOutcome::Refused);
  EXPECT_EQ(bag.size(), 2u);
  EXPECT_TRUE(weapon.empty());
  EXPECT_FALSE(LN_InventoryHasRoom(weapon, bag[0], weapons, lookup));
  EXPECT_TRUE(LN_InventoryHasRoom(weapon, bag[1], weapons, lookup));
  /* The sword does. */
  EXPECT_EQ(LN_InventoryApplyMove(Move(bag, 1, weapon, 0), weapon, weapons, &bag, &any, {}, lookup, made),
            LN_InventoryOutcome::Moved);
  ASSERT_EQ(weapon.size(), 1u);
  /* The sword dropped on the bag's apple would swap the apple into the weapon list, which refuses
   * it: the sword goes into the bag instead. */
  std::vector<LN_Value> sword_bag = {CategorizedSlot("Apple", 1, "food")};
  std::vector<LN_Value> sword_slot = {CategorizedSlot("Sword", 1, "weapon")};
  EXPECT_EQ(LN_InventoryApplyMove(Move(sword_slot, 0, sword_bag, 0),
                                  sword_bag,
                                  any,
                                  &sword_slot,
                                  &weapons,
                                  {},
                                  lookup,
                                  made),
            LN_InventoryOutcome::Moved);
  EXPECT_TRUE(sword_slot.empty());
  EXPECT_EQ(sword_bag.size(), 2u);
}

TEST(LN_Inventory, HasRoomFollowsCapacityKeepPlacesAndStacks)
{
  const LN_InventoryLookup lookup;
  const LN_Value apple = LN_MakeInventoryEntry("Apple", 1);
  std::vector<LN_Value> full = Slots({{"Gem", 1}, {"Key", 1}});
  EXPECT_FALSE(LN_InventoryHasRoom(full, apple, Rules(false, 2), lookup));
  EXPECT_TRUE(LN_InventoryHasRoom(full, apple, Rules(false, 3), lookup));
  /* A stack of the same item with room, or a gap a list that keeps places has. */
  std::vector<LN_Value> stacked = Slots({{"Apple", 1}, {"Key", 1}});
  EXPECT_TRUE(LN_InventoryHasRoom(stacked, apple, Rules(false, 2), lookup));
  std::vector<LN_Value> gap = {LN_Value(), LN_MakeInventoryEntry("Key", 1)};
  EXPECT_TRUE(LN_InventoryHasRoom(gap, apple, Rules(true, 2), lookup));
  EXPECT_FALSE(LN_InventoryHasRoom(gap, apple, Rules(false, 2), lookup));
}

/** \} */

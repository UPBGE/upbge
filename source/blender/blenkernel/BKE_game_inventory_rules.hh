/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The scene's Inventory Rules (#GameData.inventory_rules): named rule sets that the game's
 * inventories (List or Table game properties) pick by name (#bProperty.inventory_rules). Every
 * grid and Inventory node that changes such a list follows its set.
 */

namespace blender {

struct BlendDataReader;
struct BlendWriter;
struct GameData;
struct GameInventoryRules;
struct Main;
struct Scene;

/** Adds a rule set with a unique name (default "Rules"); it becomes the active one. */
GameInventoryRules *BKE_game_inventory_rules_add(GameData *gm, const char *name);
/** The set of that name, or null. */
GameInventoryRules *BKE_game_inventory_rules_find(const GameData *gm, const char *name);
/** Makes the set's name unique among the scene's sets ("Rules" when empty). */
void BKE_game_inventory_rules_unique_name(GameData *gm, GameInventoryRules *rules);
/** Removes a set; the lists of the scene's objects that picked it pick none. */
bool BKE_game_inventory_rules_remove(Main *bmain, Scene *scene, GameInventoryRules *rules);
/** Renames a set (made unique); the lists of the scene's objects that picked it follow. */
void BKE_game_inventory_rules_rename(Main *bmain,
                                     Scene *scene,
                                     GameInventoryRules *rules,
                                     const char *name);
/** How many lists (List or Table properties of the scene's objects) pick \a name. */
int BKE_game_inventory_rules_users(Main *bmain, Scene *scene, const char *name);

void BKE_game_inventory_rules_copy(GameData *dst, const GameData *src);
void BKE_game_inventory_rules_free(GameData *gm);
void BKE_game_inventory_rules_blend_write(BlendWriter *writer, GameData *gm);
void BKE_game_inventory_rules_blend_read_data(BlendDataReader *reader, GameData *gm);

}  // namespace blender

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cstddef>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"

#include "DNA_collection_types.h"
#include "DNA_object_types.h"
#include "DNA_property_types.h"
#include "DNA_scene_types.h"

#include "BKE_collection.hh"
#include "BKE_game_inventory_rules.hh"
#include "BKE_main.hh"

#include "BLO_read_write.hh"

namespace blender {

namespace {

/** Calls \a function with every List or Table game property of the scene's objects. */
template<typename Function> void for_each_scene_list(Main *bmain, Scene *scene, Function function)
{
  if (bmain == nullptr || scene == nullptr || scene->master_collection == nullptr) {
    return;
  }
  for (Object &ob : bmain->objects) {
    if (!BKE_collection_has_object_recursive(scene->master_collection, &ob)) {
      continue;
    }
    for (bProperty *prop = static_cast<bProperty *>(ob.prop.first); prop; prop = prop->next) {
      if (ELEM(prop->type, GPROP_LIST, GPROP_TABLE)) {
        function(*prop);
      }
    }
  }
}

}  // namespace

void BKE_game_inventory_rules_unique_name(GameData *gm, GameInventoryRules *rules)
{
  if (rules->name[0] == '\0') {
    STRNCPY(rules->name, "Rules");
  }
  BLI_uniquename(&gm->inventory_rules,
                 rules,
                 "Rules",
                 '.',
                 offsetof(GameInventoryRules, name),
                 sizeof(rules->name));
}

GameInventoryRules *BKE_game_inventory_rules_add(GameData *gm, const char *name)
{
  GameInventoryRules *rules = MEM_new<GameInventoryRules>(__func__);
  STRNCPY_UTF8(rules->name, (name != nullptr && name[0] != '\0') ? name : "Rules");
  BLI_addtail(&gm->inventory_rules, rules);
  BKE_game_inventory_rules_unique_name(gm, rules);
  gm->active_inventory_rules = BLI_findindex(&gm->inventory_rules, rules);
  return rules;
}

GameInventoryRules *BKE_game_inventory_rules_find(const GameData *gm, const char *name)
{
  if (gm == nullptr || name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  for (const GameInventoryRules &rules : gm->inventory_rules) {
    if (STREQ(rules.name, name)) {
      return const_cast<GameInventoryRules *>(&rules);
    }
  }
  return nullptr;
}

bool BKE_game_inventory_rules_remove(Main *bmain, Scene *scene, GameInventoryRules *rules)
{
  GameData *gm = scene != nullptr ? &scene->gm : nullptr;
  if (gm == nullptr || rules == nullptr || BLI_findindex(&gm->inventory_rules, rules) == -1) {
    return false;
  }
  const std::string name = rules->name;
  for_each_scene_list(bmain, scene, [&](bProperty &prop) {
    if (name == prop.inventory_rules) {
      prop.inventory_rules[0] = '\0';
    }
  });
  BLI_freelinkN(&gm->inventory_rules, rules);
  const int count = BLI_listbase_count(&gm->inventory_rules);
  gm->active_inventory_rules = count > 0 ? std::min(gm->active_inventory_rules, count - 1) : 0;
  return true;
}

void BKE_game_inventory_rules_rename(Main *bmain,
                                     Scene *scene,
                                     GameInventoryRules *rules,
                                     const char *name)
{
  const std::string old_name = rules->name;
  STRNCPY_UTF8(rules->name, name != nullptr ? name : "");
  BKE_game_inventory_rules_unique_name(&scene->gm, rules);
  if (old_name == rules->name) {
    return;
  }
  for_each_scene_list(bmain, scene, [&](bProperty &prop) {
    if (old_name == prop.inventory_rules) {
      STRNCPY(prop.inventory_rules, rules->name);
    }
  });
}

int BKE_game_inventory_rules_users(Main *bmain, Scene *scene, const char *name)
{
  int users = 0;
  if (name == nullptr || name[0] == '\0') {
    return users;
  }
  for_each_scene_list(bmain, scene, [&](bProperty &prop) {
    if (STREQ(prop.inventory_rules, name)) {
      users++;
    }
  });
  return users;
}

void BKE_game_inventory_rules_copy(GameData *dst, const GameData *src)
{
  BLI_listbase_clear(&dst->inventory_rules);
  for (const GameInventoryRules &rules : src->inventory_rules) {
    BLI_addtail(&dst->inventory_rules, MEM_dupalloc(&rules));
  }
}

void BKE_game_inventory_rules_free(GameData *gm)
{
  BLI_freelistN(&gm->inventory_rules);
}

void BKE_game_inventory_rules_blend_write(BlendWriter *writer, GameData *gm)
{
  for (GameInventoryRules &rules : gm->inventory_rules) {
    writer->write_struct(&rules);
  }
}

void BKE_game_inventory_rules_blend_read_data(BlendDataReader *reader, GameData *gm)
{
  BLO_read_struct_list(reader, GameInventoryRules, &gm->inventory_rules);
}

}  // namespace blender

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cstddef>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"

#include "DNA_scene_types.h"

#include "BKE_game_input_map.hh"

#include "BLO_read_write.hh"

#include "WM_types.hh"

namespace blender {

namespace {

struct DefaultBinding {
  short device;
  short code;
  short modifiers = 0;
};

struct BuiltinControl {
  const char *name;
  DefaultBinding bindings[4];
  int binding_count;
};

/* The engine's UI controls in #GAME_INPUT_BUILTIN_NAVIGATE_UP... order, with the keys, mouse
 * buttons and gamepad controls menus have always used. */
const BuiltinControl builtin_controls[GAME_INPUT_BUILTIN_COUNT] = {
    {"Navigate Up",
     {{GAME_INPUT_KEYBOARD, EVT_UPARROWKEY},
      {GAME_INPUT_KEYBOARD, EVT_PAD8},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_DPAD_UP},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_LEFT_STICK_UP}},
     4},
    {"Navigate Down",
     {{GAME_INPUT_KEYBOARD, EVT_DOWNARROWKEY},
      {GAME_INPUT_KEYBOARD, EVT_PAD2},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_DPAD_DOWN},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_LEFT_STICK_DOWN}},
     4},
    {"Navigate Left",
     {{GAME_INPUT_KEYBOARD, EVT_LEFTARROWKEY},
      {GAME_INPUT_KEYBOARD, EVT_PAD4},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_DPAD_LEFT},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_LEFT_STICK_LEFT}},
     4},
    {"Navigate Right",
     {{GAME_INPUT_KEYBOARD, EVT_RIGHTARROWKEY},
      {GAME_INPUT_KEYBOARD, EVT_PAD6},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_DPAD_RIGHT},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_LEFT_STICK_RIGHT}},
     4},
    {"Accept",
     {{GAME_INPUT_KEYBOARD, EVT_RETKEY},
      {GAME_INPUT_KEYBOARD, EVT_SPACEKEY},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_A}},
     3},
    {"Back", {{GAME_INPUT_KEYBOARD, EVT_ESCKEY}, {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_B}}, 2},
    {"Next",
     {{GAME_INPUT_KEYBOARD, EVT_TABKEY},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_RIGHT_SHOULDER}},
     2},
    {"Previous",
     {{GAME_INPUT_KEYBOARD, EVT_TABKEY, GAME_INPUT_MOD_SHIFT},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_LEFT_SHOULDER}},
     2},
    {"Secondary",
     {{GAME_INPUT_MOUSE, RIGHTMOUSE}, {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_X}},
     2},
    {"Scroll Up",
     {{GAME_INPUT_MOUSE, WHEELUPMOUSE}, {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_RIGHT_STICK_UP}},
     2},
    {"Scroll Down",
     {{GAME_INPUT_MOUSE, WHEELDOWNMOUSE},
      {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_RIGHT_STICK_DOWN}},
     2},
    {"Grab", {{GAME_INPUT_KEYBOARD, EVT_GKEY}, {GAME_INPUT_GAMEPAD, GAME_INPUT_GAMEPAD_Y}}, 2},
};

bool is_builtin(const int builtin)
{
  return builtin > GAME_INPUT_BUILTIN_NONE && builtin <= GAME_INPUT_BUILTIN_COUNT;
}

void add_default_bindings(GameInputAction *action)
{
  const BuiltinControl &control = builtin_controls[action->builtin - 1];
  for (int index = 0; index < control.binding_count; ++index) {
    GameInputBinding *binding = MEM_new<GameInputBinding>(__func__);
    binding->device = control.bindings[index].device;
    binding->code = control.bindings[index].code;
    binding->modifiers = control.bindings[index].modifiers;
    BLI_addtail(&action->bindings, binding);
  }
  action->active_binding = 0;
}

GameInputAction *find_builtin(GameData *gm, const int builtin)
{
  for (GameInputAction &action : gm->input_actions) {
    if (action.builtin == builtin) {
      return &action;
    }
  }
  return nullptr;
}

/** The first of the game's own actions: the UI controls come before it. */
GameInputAction *first_custom_action(GameData *gm)
{
  for (GameInputAction &action : gm->input_actions) {
    if (!is_builtin(action.builtin)) {
      return &action;
    }
  }
  return nullptr;
}

short default_code(const GameInputAction *action, const int device)
{
  switch (device) {
    case GAME_INPUT_MOUSE:
      return LEFTMOUSE;
    case GAME_INPUT_GAMEPAD:
      return action->type == GAME_INPUT_ACTION_VECTOR ? GAME_INPUT_GAMEPAD_LEFT_STICK :
                                                        GAME_INPUT_GAMEPAD_A;
    default:
      /* No key yet: the Properties editor waits for one to be pressed. */
      return 0;
  }
}

}  // namespace

const char *BKE_game_input_builtin_name(const int builtin)
{
  return is_builtin(builtin) ? builtin_controls[builtin - 1].name : "";
}

void BKE_game_input_map_ensure_builtins(GameData *gm)
{
  /* The selected row stays on the same action when a control is added before it. */
  const bool had_actions = !BLI_listbase_is_empty(&gm->input_actions);
  GameInputAction *previous = nullptr;
  for (int builtin = 1; builtin <= GAME_INPUT_BUILTIN_COUNT; ++builtin) {
    GameInputAction *action = find_builtin(gm, builtin);
    if (action == nullptr) {
      action = MEM_new<GameInputAction>(__func__);
      action->builtin = short(builtin);
      action->type = GAME_INPUT_ACTION_BUTTON;
      STRNCPY(action->name, builtin_controls[builtin - 1].name);
      add_default_bindings(action);
      if (previous != nullptr) {
        BLI_insertlinkafter(&gm->input_actions, previous, action);
      }
      else {
        BLI_addhead(&gm->input_actions, action);
      }
      if (had_actions && BLI_findindex(&gm->input_actions, action) <= gm->active_input_action) {
        gm->active_input_action++;
      }
    }
    previous = action;
  }
  /* A game's own action that took a UI control's name keeps working under a unique one. */
  for (GameInputAction &action : gm->input_actions) {
    if (!is_builtin(action.builtin)) {
      BKE_game_input_action_unique_name(gm, &action);
    }
  }
}

void BKE_game_input_map_reset_builtins(GameData *gm)
{
  BKE_game_input_map_ensure_builtins(gm);
  for (GameInputAction &action : gm->input_actions) {
    if (!is_builtin(action.builtin)) {
      continue;
    }
    BLI_freelistN(&action.bindings);
    STRNCPY(action.name, builtin_controls[action.builtin - 1].name);
    action.context[0] = '\0';
    action.type = GAME_INPUT_ACTION_BUTTON;
    add_default_bindings(&action);
  }
  const GameData defaults;
  gm->input_repeat_delay = defaults.input_repeat_delay;
  gm->input_repeat_interval = defaults.input_repeat_interval;
  gm->input_stick_threshold = defaults.input_stick_threshold;
  gm->input_flag = defaults.input_flag;
}

void BKE_game_input_action_unique_name(GameData *gm, GameInputAction *action)
{
  if (is_builtin(action->builtin)) {
    STRNCPY(action->name, builtin_controls[action->builtin - 1].name);
    return;
  }
  if (action->name[0] == '\0') {
    STRNCPY(action->name, "Action");
  }
  BLI_uniquename(&gm->input_actions,
                 action,
                 "Action",
                 '.',
                 offsetof(GameInputAction, name),
                 sizeof(action->name));
}

GameInputAction *BKE_game_input_action_add(GameData *gm, const char *name, const int type)
{
  GameInputAction *action = MEM_new<GameInputAction>(__func__);
  STRNCPY_UTF8(action->name, (name != nullptr && name[0] != '\0') ? name : "Action");
  action->type = short(type == GAME_INPUT_ACTION_VECTOR ? GAME_INPUT_ACTION_VECTOR :
                                                          GAME_INPUT_ACTION_BUTTON);
  BLI_addtail(&gm->input_actions, action);
  BKE_game_input_action_unique_name(gm, action);
  gm->active_input_action = BLI_findindex(&gm->input_actions, action);
  return action;
}

bool BKE_game_input_action_remove(GameData *gm, GameInputAction *action)
{
  if (action == nullptr || is_builtin(action->builtin) ||
      BLI_findindex(&gm->input_actions, action) == -1)
  {
    return false;
  }
  BLI_freelistN(&action->bindings);
  BLI_freelinkN(&gm->input_actions, action);
  const int count = BLI_listbase_count(&gm->input_actions);
  gm->active_input_action = count > 0 ? std::min(gm->active_input_action, count - 1) : 0;
  return true;
}

GameInputBinding *BKE_game_input_binding_add(GameInputAction *action, const int device)
{
  GameInputBinding *binding = MEM_new<GameInputBinding>(__func__);
  binding->device = short(
      device == GAME_INPUT_MOUSE || device == GAME_INPUT_GAMEPAD ? device : GAME_INPUT_KEYBOARD);
  binding->code = default_code(action, binding->device);
  BLI_addtail(&action->bindings, binding);
  action->active_binding = BLI_listbase_count(&action->bindings) - 1;
  return binding;
}

bool BKE_game_input_binding_remove(GameInputAction *action, GameInputBinding *binding)
{
  if (binding == nullptr || BLI_findindex(&action->bindings, binding) == -1) {
    return false;
  }
  BLI_freelinkN(&action->bindings, binding);
  const int count = BLI_listbase_count(&action->bindings);
  action->active_binding = count > 0 ? std::min(action->active_binding, count - 1) : 0;
  return true;
}

void BKE_game_input_binding_set_device(GameInputAction *action,
                                       GameInputBinding *binding,
                                       const int device)
{
  if (device != GAME_INPUT_KEYBOARD && device != GAME_INPUT_MOUSE && device != GAME_INPUT_GAMEPAD)
  {
    return;
  }
  if (binding->device == device) {
    return;
  }
  binding->device = short(device);
  binding->code = default_code(action, device);
  binding->modifiers = 0;
}

void BKE_game_input_map_copy(GameData *dst, const GameData *src)
{
  BLI_listbase_clear(&dst->input_actions);
  for (const GameInputAction &action : src->input_actions) {
    GameInputAction *copy = MEM_dupalloc(&action);
    BLI_duplicatelist(&copy->bindings, &action.bindings);
    BLI_addtail(&dst->input_actions, copy);
  }
}

void BKE_game_input_map_free(GameData *gm)
{
  for (GameInputAction &action : gm->input_actions) {
    BLI_freelistN(&action.bindings);
  }
  BLI_freelistN(&gm->input_actions);
}

void BKE_game_input_map_blend_write(BlendWriter *writer, GameData *gm)
{
  for (GameInputAction &action : gm->input_actions) {
    writer->write_struct(&action);
    writer->write_struct_list(&action.bindings);
  }
}

void BKE_game_input_map_blend_read_data(BlendDataReader *reader, GameData *gm)
{
  BLO_read_struct_list(reader, GameInputAction, &gm->input_actions);
  for (GameInputAction &action : gm->input_actions) {
    BLO_read_struct_list(reader, GameInputBinding, &action.bindings);
  }
}

}  // namespace blender

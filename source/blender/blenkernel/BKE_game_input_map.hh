/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The scene's game Input Map (#GameData.input_actions): the engine's UI controls and the game's
 * own actions, each with its keys, mouse buttons and gamepad controls.
 */

namespace blender {

struct BlendDataReader;
struct BlendWriter;
struct GameData;
struct GameInputAction;
struct GameInputBinding;

/** The name of a UI control (#GAME_INPUT_BUILTIN_NAVIGATE_UP...), or "" for others. */
const char *BKE_game_input_builtin_name(int builtin);

/** Adds the engine's UI controls that are missing, with their default bindings, in front of the
 * game's own actions. */
void BKE_game_input_map_ensure_builtins(GameData *gm);
/** Gives the UI controls their default bindings and settings again. The game's own actions stay
 * as they are. */
void BKE_game_input_map_reset_builtins(GameData *gm);

/** Adds one of the game's own actions with a unique name. */
GameInputAction *BKE_game_input_action_add(GameData *gm, const char *name, int type);
/** Removes one of the game's own actions; the UI controls cannot be removed. */
bool BKE_game_input_action_remove(GameData *gm, GameInputAction *action);
/** Makes the action's name unique in the map; a UI control keeps its own name. */
void BKE_game_input_action_unique_name(GameData *gm, GameInputAction *action);

GameInputBinding *BKE_game_input_binding_add(GameInputAction *action, int device);
bool BKE_game_input_binding_remove(GameInputAction *action, GameInputBinding *binding);
/** Switches the binding to another device with that device's default control. */
void BKE_game_input_binding_set_device(GameInputAction *action,
                                       GameInputBinding *binding,
                                       int device);

void BKE_game_input_map_copy(GameData *dst, const GameData *src);
void BKE_game_input_map_free(GameData *gm);
void BKE_game_input_map_blend_write(BlendWriter *writer, GameData *gm);
void BKE_game_input_map_blend_read_data(BlendDataReader *reader, GameData *gm);

}  // namespace blender

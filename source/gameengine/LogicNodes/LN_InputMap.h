/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_InputMap.h
 *  \ingroup logicnodes
 *
 * The game's Input Map (Scene > Game Input Map of the start scene): the menus' UI controls and
 * the game's own actions, each with its keys, mouse buttons and gamepad controls. Player 0 has
 * the keyboard and mouse. By default player N also has gamepad N; the map's settings and Set
 * Player Gamepad change which gamepad a player has (GamepadOfUser).
 */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "LN_Snapshot.h"
#include "LN_Types.h"

#include "MT_Vector3.h"

namespace blender {
struct GameData;
}
struct LN_GamepadSnapshot;
class LN_InputSnapshot;
/** DEV_Joystick.h: whose labels a gamepad's buttons carry (0 = Generic). */
enum class DEV_PadFamily : uint8_t;

/** GameInputBinding.device. */
enum class LN_InputDevice : uint8_t {
  Keyboard = 0,
  Mouse,
  Gamepad,
};

/** Which bindings Rebind Input Action and Get Input Binding use. */
enum class LN_InputDeviceClass : uint8_t {
  /** Get Input Binding: the device player 0 used last. */
  Automatic = 0,
  KeyboardMouse,
  Gamepad,
};

struct LN_InputBinding {
  LN_InputDevice device = LN_InputDevice::Keyboard;
  /** Keyboard and mouse: a Blender event type. Gamepad: a GAME_INPUT_GAMEPAD_* control. */
  int16_t code = 0;
  /** Keyboard and mouse: the engine's input code of `code`. */
  int16_t input = 0;
  /** GAME_INPUT_MOD_* keys that must be held. */
  uint8_t modifiers = 0;
  /** GAME_INPUT_DIRECTION_*: the way it pushes a vector action. */
  uint8_t direction = 0;
  /** One bit per modifier combination that another binding of the same key or button has, with
   * more modifiers than this one: while all of them are held, that binding wins. */
  uint8_t shadowed_by = 0;

  bool operator==(const LN_InputBinding &other) const
  {
    return device == other.device && code == other.code && modifiers == other.modifiers &&
           direction == other.direction;
  }
};

struct LN_InputAction {
  std::string name;
  /** Only used while the topmost menu has this Input Context; empty: always. */
  std::string context;
  bool vector = false;
  std::vector<LN_InputBinding> bindings;
  /** The bindings of the .blend, for Reset. */
  std::vector<LN_InputBinding> defaults;
};

/** What an action does this tick for one player. */
struct LN_InputActionState {
  bool active = false;
  bool pressed = false;
  bool released = false;
  float value = 0.0f;
  /** Vector actions: x right, y up, at most 1 long. */
  MT_Vector3 vector = MT_Vector3(0.0f, 0.0f, 0.0f);
};

/** One binding's reading in a tick. */
struct LN_InputBindingReading {
  bool level = false;
  bool pressed = false;
  bool released = false;
  float value = 0.0f;
};

/**
 * Changed (Load, Rebind, Reset, Restore, capture) only on the main thread, before worker trees
 * run in a tick: Rebind Input Action and Input Map are main-thread-only instructions, and
 * main-thread trees finish before the worker trees start. Worker trees only read it.
 */
class LN_InputMap {
 public:
  /** The map in use: the engine's defaults until the game loads its start scene's map. */
  static LN_InputMap &Get();

  /** Takes a scene's Input Map and drops the player's changes; nullptr: the engine's defaults. */
  void Load(const blender::GameData *game);

  int32_t Find(const std::string &name) const;
  /** Whether one of the game's own actions (not a menu control) takes the key (an engine input
   * code) without modifiers: that key then does not end the game as the Exit Key, as with a
   * Keyboard sensor on it. */
  bool GameActionUsesKey(int16_t input) const;
  int32_t FindBuiltin(LN_UIAction action) const
  {
    return m_builtins[size_t(action)];
  }
  const LN_InputAction &Action(const int32_t index) const
  {
    return m_actions[size_t(index)];
  }
  size_t Size() const
  {
    return m_actions.size();
  }
  float RepeatDelay() const
  {
    return m_repeatDelay;
  }
  float RepeatInterval() const
  {
    return m_repeatInterval;
  }
  float StickThreshold() const
  {
    return m_stickThreshold;
  }
  /** Changes whenever a binding changes. */
  uint64_t Revision() const
  {
    return m_revision;
  }

  /** Puts `binding` in place of the action's first binding of the same device class (and, for a
   * vector action, the same direction), or adds it. */
  void Rebind(int32_t action, const LN_InputBinding &binding);
  /** Back to the bindings of the .blend. */
  void Reset();
  /** The bindings that differ from the .blend, as JSON. */
  std::string Save() const;
  /** Applies what Save() wrote over the .blend's bindings; false when it is not such a file. */
  bool Restore(const std::string &json);

  /** A binding's name for players: "Space", "Shift Tab", "A", "Left Stick Up". A gamepad's
   * buttons are named as that kind of pad labels them ("Cross" on a PlayStation pad). */
  static std::string BindingText(const LN_InputBinding &binding,
                                 DEV_PadFamily family = DEV_PadFamily(0));
  /** The name of the action's first binding for that device class and direction ("" if none).
   * Gamepad buttons are named for `family`. */
  std::string ActionText(int32_t action,
                         LN_InputDeviceClass device_class,
                         int direction,
                         DEV_PadFamily family = DEV_PadFamily(0)) const;
  /** Whose labels the player's gamepad carries. A player without a gamepad gets the first one
   * plugged in, so texts everyone reads name the pad in use; Generic when none is. */
  DEV_PadFamily PadFamilyOfUser(int32_t user) const;

  /** Notes the device player 0 last used, for Automatic. */
  void NoteInput(const LN_InputSnapshot &input);
  bool LastUsedGamepad() const
  {
    return m_lastUsedGamepad;
  }
  /** Changes whenever player 0 switches between keyboard and mouse, and gamepad, and when a
   * gamepad of another kind takes a number (it names its buttons otherwise). */
  uint64_t DeviceRevision() const
  {
    return m_deviceRevision;
  }
  /** The action's key or button for `user`'s device: player 0's last used one, other players'
   * gamepad. Falls back to the other device's binding when that device has none. A vector action
   * names all four directions ("WASD") or its stick. */
  std::string ActionTextForUser(int32_t action, int32_t user) const;
  /** The action's control on `device` (a vector action: all four directions or its stick). */
  std::string ActionTextForDevice(int32_t action,
                                  LN_InputDeviceClass device,
                                  DEV_PadFamily family = DEV_PadFamily(0)) const;
  /** Turns what is between braces, when it is not an action, into text (a game property's
   * value); false leaves it as written. */
  using TokenResolver = std::function<bool(const std::string &token, std::string &r_text)>;
  /** Text with each `{Action}` (an action of this map) replaced by its key or button for
   * `user`, or `{Action:Gamepad}` / `{Action:Keyboard}` by that device's; other braces go to
   * `resolver`, else stay as written. */
  std::string ExpandActionNames(const std::string &text,
                                int32_t user,
                                const TokenResolver *resolver = nullptr) const;

  /* Players' gamepads (split screen, local multiplayer). Gamepad numbers are DEV_Joystick slots.
   * By default player N has gamepad N; Gamepads Start at Player 1 gives gamepad N to player N + 1
   * (player 0 keeps the keyboard and mouse alone); Press to Join gives a gamepad to the first
   * free player when it presses a button. A player keeps their gamepad when it is unplugged. */

  /** The player's gamepad, -1 for none. */
  int32_t GamepadOfUser(int32_t user) const
  {
    return user >= 0 && user < LN_MAX_GAMEPADS ? m_userGamepad[size_t(user)] : -1;
  }
  /** The player who has the gamepad, -1 for none. */
  int32_t UserOfGamepad(int32_t gamepad) const;
  /** The player joined: their gamepad pressed a button (On Player Join ran for them). */
  bool IsUserJoined(const int32_t user) const
  {
    return user >= 0 && user < LN_MAX_GAMEPADS && m_joined[size_t(user)];
  }
  /** Set Player Gamepad: -1 takes the player's gamepad away (the player leaves); the player who
   * had `gamepad` before loses it. A player who joined stays joined with another gamepad. */
  void SetUserGamepad(int32_t user, int32_t gamepad);
  /** A gamepad's buttons as UpdatePlayers reads them. */
  struct GamepadButtons {
    bool connected = false;
    /** Changes when another device takes the gamepad's number. */
    uint64_t serial = 0;
    /** Per button: how many times it was pressed. */
    std::array<uint64_t, 32> presses = {};
    /** The buttons held now, one bit each. */
    uint32_t held = 0;
    /** Whose labels its buttons carry. */
    DEV_PadFamily family = DEV_PadFamily(0);
  };
  /** Once a frame, after the devices were read: a gamepad pressing a button joins its player
   * (Press to Join: gets the first free player), which On Player Join reports. */
  void UpdatePlayers();
  /** UpdatePlayers with the gamepads given (tests). */
  void UpdatePlayers(const std::array<GamepadButtons, LN_MAX_GAMEPADS> &gamepads);
  /** Press to Join: the press that gave a gamepad its player is hidden from the game and the
   * menus until it is let go (its release too), so joining doesn't also press A in a menu. */
  bool HidesJoinPress(const int32_t gamepad, const int32_t button) const
  {
    return gamepad >= 0 && gamepad < LN_MAX_GAMEPADS && button >= 0 && button < 32 &&
           (m_hiddenJoinButtons[size_t(gamepad)] & (1u << uint32_t(button))) != 0;
  }
  /** The next join after `io_serial` (which it advances); false when there is none. */
  bool NextJoin(uint64_t &io_serial, int32_t &r_user, int32_t &r_gamepad) const;
  uint64_t JoinSerial() const
  {
    return m_joinSerial;
  }

  /** While Rebind Input Action waits for a control, and in the frame (input device clear
   * revision) it gets one, menus ignore input: the key pressed is the new binding, not a click. */
  void BeginCapture();
  void EndCapture(uint64_t device_revision);
  bool BlocksUI(uint64_t device_revision) const
  {
    return m_captures > 0 ||
           (device_revision != UINT64_MAX && device_revision == m_captureEndRevision);
  }

 private:
  void UpdateShadows();
  /** A new game: the players' gamepads from the settings, and the presses seen so far. */
  void ResetPlayers();

  struct PlayerJoin {
    uint64_t serial = 0;
    int32_t user = -1;
    int32_t gamepad = -1;
  };

  std::vector<LN_InputAction> m_actions;
  std::unordered_map<std::string, int32_t> m_index;
  int32_t m_builtins[LN_UI_ACTION_COUNT] = {};
  float m_repeatDelay = 0.4f;
  float m_repeatInterval = 0.1f;
  float m_stickThreshold = 0.5f;
  uint64_t m_revision = 0;
  int m_captures = 0;
  uint64_t m_captureEndRevision = UINT64_MAX;
  bool m_lastUsedGamepad = false;
  uint64_t m_deviceRevision = 0;
  std::array<int32_t, LN_MAX_GAMEPADS> m_userGamepad = {0, 1, 2, 3, 4, 5, 6, 7};
  /** A player joined: their gamepad pressed a button (On Player Join ran); they leave when Set
   * Player Gamepad takes their gamepad away or gives it to another player. */
  std::array<bool, LN_MAX_GAMEPADS> m_joined = {};
  bool m_gamepadsFromPlayer1 = false;
  bool m_pressToJoin = false;
  /** Per gamepad: its device (instance serial) and the press count of each button, to see a new
   * press once a frame. */
  std::array<uint64_t, LN_MAX_GAMEPADS> m_gamepadSerial = {};
  std::array<std::array<uint64_t, 32>, LN_MAX_GAMEPADS> m_gamepadPresses = {};
  /** Per gamepad: whose labels its buttons carry (the last pad seen with that number). */
  std::array<DEV_PadFamily, LN_MAX_GAMEPADS> m_gamepadFamily = {};
  std::array<uint32_t, LN_MAX_GAMEPADS> m_hiddenJoinButtons = {};
  /** Hidden join presses let go at the last update: shown again at the next one, after the
   * frame that saw the release. */
  std::array<uint32_t, LN_MAX_GAMEPADS> m_hiddenJoinLetGo = {};
  /** The last joins, oldest first; each scene reads them by serial (On Player Join). */
  std::vector<PlayerJoin> m_joins;
  uint64_t m_joinSerial = 0;
};

/** Rebind Input Action's watch for the player's next control. */
struct LN_InputCapture {
  enum class Result : uint8_t {
    Waiting = 0,
    Captured,
    /** Esc. */
    Cancelled,
  };
  /** A Shift, Ctrl or Alt pressed so far with no other key: it becomes the binding when it is
   * let go alone. */
  int32_t modifier_input = 0;
  /** A stick counts only after it was centred during the capture. */
  bool stick_centred[2] = {false, false};

  /** Looks for a newly pressed control of the device class. A vector action takes a whole stick
   * when a stick is pushed. The binding's direction is left for the caller. */
  Result Update(const LN_InputSnapshot &input,
                int32_t user,
                LN_InputDeviceClass device_class,
                bool vector,
                LN_InputBinding &r_binding);
};

/** The GAME_INPUT_MOD_* keys held in `input`. */
uint8_t LN_InputModifiersHeld(const LN_InputSnapshot &input);

/** Reads a key, mouse button, wheel direction, gamepad button or trigger binding. Stick bindings
 * read nothing here: their direction needs the stick's history. */
LN_InputBindingReading LN_ReadInputBinding(const LN_InputBinding &binding,
                                           const LN_InputSnapshot &input,
                                           int32_t user,
                                           uint8_t held_modifiers);

/** Stick bindings: 0 = none, 1 = left stick, 2 = right stick. */
int LN_InputBindingStick(const LN_InputBinding &binding);
/** A stick's position, x right and y up, each -1..1. */
MT_Vector3 LN_GamepadStickPosition(const LN_GamepadSnapshot &gamepad, int stick);

/** Each player's input context: the one their topmost menu sets (empty: none). */
using LN_InputContexts = std::array<std::string_view, LN_MAX_GAMEPADS>;

/** The actions' states, sampled once per tick from one input. Edges of keys and buttons come
 * from the input; sticks remember their last tick to find theirs. */
class LN_InputActionTracker {
 public:
  /** Samples a new tick. `tick` is the phase's tick index: after a gap the tracker starts over,
   * so a control held meanwhile is not pressed or released again. */
  void Update(const LN_InputSnapshot &input, const LN_InputContexts &contexts, uint64_t tick);
  /** Samples the same tick again, after the UI took some of `input`'s presses. */
  void Refresh(const LN_InputSnapshot &input, const LN_InputContexts &contexts);
  /** nullptr: no such action or player. */
  const LN_InputActionState *Find(int32_t action, int32_t user) const;

 private:
  void Sample(const LN_InputSnapshot &input, const LN_InputContexts &contexts);

  std::vector<LN_InputActionState> m_states;
  /** Bindings held last tick, per binding and player. */
  std::vector<uint8_t> m_levels;
  /** The history this tick was sampled from, for Refresh. */
  std::vector<LN_InputActionState> m_statesBefore;
  std::vector<uint8_t> m_levelsBefore;
  std::vector<uint32_t> m_firstBinding;
  uint64_t m_revision = UINT64_MAX;
  uint64_t m_tick = UINT64_MAX;
  /** No binding history (first tick, changed bindings, a gap): a stick or trigger held now was
   * not just pressed. */
  bool m_fresh = true;
};

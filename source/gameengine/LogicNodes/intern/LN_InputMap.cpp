/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_InputMap.cpp
 *  \ingroup logicnodes
 */

#include "LN_InputMap.h"

#include <algorithm>
#include <optional>
#include <cmath>
#include <sstream>

#include "BKE_game_input_map.hh"
#include "BLI_listbase.h"
#include "BLI_serialize.hh"
#include "DNA_scene_types.h"
#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "wm_event_types.hh"

#include "BL_DataConversion.h"
#include "DEV_Joystick.h"
#include "LN_Snapshot.h"
#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"

/* Blender's event type tests are macros that name the event types of namespace blender. */
namespace blender {
static bool ln_event_is_key(const int type)
{
  return ISKEYBOARD(type);
}
static bool ln_event_is_mouse(const int type)
{
  return ISMOUSE_BUTTON(type) || ISMOUSE_WHEEL(type);
}
}  // namespace blender

namespace {

struct GamepadControlName {
  int16_t code;
  const char *identifier;
  const char *text;
};

/* Identifiers match the Properties editor's; the text is what players read. */
constexpr GamepadControlName kGamepadControls[] = {
    {GAME_INPUT_GAMEPAD_A, "A_CROSS", "A"},
    {GAME_INPUT_GAMEPAD_B, "B_CIRCLE", "B"},
    {GAME_INPUT_GAMEPAD_X, "X_SQUARE", "X"},
    {GAME_INPUT_GAMEPAD_Y, "Y_TRIANGLE", "Y"},
    {GAME_INPUT_GAMEPAD_LEFT_SHOULDER, "LB_L1", "LB"},
    {GAME_INPUT_GAMEPAD_RIGHT_SHOULDER, "RB_R1", "RB"},
    {GAME_INPUT_GAMEPAD_LEFT_TRIGGER, "LT_L2", "LT"},
    {GAME_INPUT_GAMEPAD_RIGHT_TRIGGER, "RT_R2", "RT"},
    {GAME_INPUT_GAMEPAD_LEFT_STICK_PRESS, "L3", "L3"},
    {GAME_INPUT_GAMEPAD_RIGHT_STICK_PRESS, "R3", "R3"},
    {GAME_INPUT_GAMEPAD_SELECT, "SELECT_SHARE", "Select"},
    {GAME_INPUT_GAMEPAD_START, "START_OPTIONS", "Start"},
    {GAME_INPUT_GAMEPAD_DPAD_UP, "DPAD_UP", "D-Pad Up"},
    {GAME_INPUT_GAMEPAD_DPAD_DOWN, "DPAD_DOWN", "D-Pad Down"},
    {GAME_INPUT_GAMEPAD_DPAD_LEFT, "DPAD_LEFT", "D-Pad Left"},
    {GAME_INPUT_GAMEPAD_DPAD_RIGHT, "DPAD_RIGHT", "D-Pad Right"},
    {GAME_INPUT_GAMEPAD_LEFT_STICK_UP, "LEFT_STICK_UP", "Left Stick Up"},
    {GAME_INPUT_GAMEPAD_LEFT_STICK_DOWN, "LEFT_STICK_DOWN", "Left Stick Down"},
    {GAME_INPUT_GAMEPAD_LEFT_STICK_LEFT, "LEFT_STICK_LEFT", "Left Stick Left"},
    {GAME_INPUT_GAMEPAD_LEFT_STICK_RIGHT, "LEFT_STICK_RIGHT", "Left Stick Right"},
    {GAME_INPUT_GAMEPAD_RIGHT_STICK_UP, "RIGHT_STICK_UP", "Right Stick Up"},
    {GAME_INPUT_GAMEPAD_RIGHT_STICK_DOWN, "RIGHT_STICK_DOWN", "Right Stick Down"},
    {GAME_INPUT_GAMEPAD_RIGHT_STICK_LEFT, "RIGHT_STICK_LEFT", "Right Stick Left"},
    {GAME_INPUT_GAMEPAD_RIGHT_STICK_RIGHT, "RIGHT_STICK_RIGHT", "Right Stick Right"},
    {GAME_INPUT_GAMEPAD_LEFT_STICK, "LEFT_STICK", "Left Stick"},
    {GAME_INPUT_GAMEPAD_RIGHT_STICK, "RIGHT_STICK", "Right Stick"},
};

constexpr const char *kDirectionIdentifiers[] = {"UP", "DOWN", "LEFT", "RIGHT"};

/* Below this a stick reads as centred, so a drifting stick gives no value. */
constexpr float kStickDeadzone = 0.1f;
/* A stick direction or stick held past the threshold lets go again only under this share of it,
 * so a stick resting near the threshold does not flicker. */
constexpr float kStickRelease = 0.7f;

const GamepadControlName *FindGamepadControl(const int code)
{
  for (const GamepadControlName &control : kGamepadControls) {
    if (control.code == code) {
      return &control;
    }
  }
  return nullptr;
}

/** A control's name as that kind of pad labels it; the D-pad and the sticks read the same on
 * all. */
const char *GamepadControlText(const GamepadControlName &control, const DEV_PadFamily family)
{
  const bool playstation = family == DEV_PadFamily::PlayStation3 ||
                           family == DEV_PadFamily::PlayStation4 ||
                           family == DEV_PadFamily::PlayStation5;
  const bool xbox = family == DEV_PadFamily::Xbox || family == DEV_PadFamily::Xbox360;
  const bool nintendo = family == DEV_PadFamily::Nintendo;
  switch (control.code) {
    case GAME_INPUT_GAMEPAD_A:
      return playstation ? "Cross" : control.text;
    case GAME_INPUT_GAMEPAD_B:
      return playstation ? "Circle" : control.text;
    case GAME_INPUT_GAMEPAD_X:
      return playstation ? "Square" : control.text;
    case GAME_INPUT_GAMEPAD_Y:
      return playstation ? "Triangle" : control.text;
    case GAME_INPUT_GAMEPAD_LEFT_SHOULDER:
      return playstation ? "L1" : nintendo ? "L" : control.text;
    case GAME_INPUT_GAMEPAD_RIGHT_SHOULDER:
      return playstation ? "R1" : nintendo ? "R" : control.text;
    case GAME_INPUT_GAMEPAD_LEFT_TRIGGER:
      return playstation ? "L2" : nintendo ? "ZL" : control.text;
    case GAME_INPUT_GAMEPAD_RIGHT_TRIGGER:
      return playstation ? "R2" : nintendo ? "ZR" : control.text;
    case GAME_INPUT_GAMEPAD_LEFT_STICK_PRESS:
      return xbox || nintendo ? "LS" : control.text;
    case GAME_INPUT_GAMEPAD_RIGHT_STICK_PRESS:
      return xbox || nintendo ? "RS" : control.text;
    case GAME_INPUT_GAMEPAD_SELECT:
      switch (family) {
        case DEV_PadFamily::Xbox:
          return "View";
        case DEV_PadFamily::Xbox360:
          return "Back";
        case DEV_PadFamily::PlayStation4:
          return "Share";
        case DEV_PadFamily::PlayStation5:
          return "Create";
        case DEV_PadFamily::Nintendo:
          return "-";
        default:
          return control.text;
      }
    case GAME_INPUT_GAMEPAD_START:
      switch (family) {
        case DEV_PadFamily::Xbox:
          return "Menu";
        case DEV_PadFamily::PlayStation4:
        case DEV_PadFamily::PlayStation5:
          return "Options";
        case DEV_PadFamily::Nintendo:
          return "+";
        default:
          return control.text;
      }
    default:
      return control.text;
  }
}

bool IsWheel(const LN_InputBinding &binding)
{
  return binding.device == LN_InputDevice::Mouse &&
         (binding.code == blender::WHEELUPMOUSE || binding.code == blender::WHEELDOWNMOUSE);
}

/** Converts a binding of the .blend; false when it has no control (a key not chosen yet). */
bool ConvertBinding(const blender::GameInputBinding &source, LN_InputBinding &r_binding)
{
  r_binding = LN_InputBinding();
  r_binding.code = source.code;
  r_binding.modifiers = uint8_t(source.modifiers & (GAME_INPUT_MOD_SHIFT | GAME_INPUT_MOD_CTRL |
                                                    GAME_INPUT_MOD_ALT));
  r_binding.direction = uint8_t(std::clamp<int>(
      source.direction, GAME_INPUT_DIRECTION_UP, GAME_INPUT_DIRECTION_RIGHT));
  switch (source.device) {
    case GAME_INPUT_KEYBOARD:
    case GAME_INPUT_MOUSE: {
      r_binding.device = source.device == GAME_INPUT_KEYBOARD ? LN_InputDevice::Keyboard :
                                                                LN_InputDevice::Mouse;
      if (source.code == blender::EVENT_NONE) {
        return false;
      }
      r_binding.input = int16_t(BL_ConvertKeyCode(source.code));
      return r_binding.input != SCA_IInputDevice::NOKEY;
    }
    case GAME_INPUT_GAMEPAD:
      r_binding.device = LN_InputDevice::Gamepad;
      r_binding.modifiers = 0;
      return FindGamepadControl(source.code) != nullptr;
  }
  return false;
}

bool BindingFromNames(const std::string &device,
                      const std::string &control,
                      const int modifiers,
                      const std::string &direction,
                      LN_InputBinding &r_binding)
{
  blender::GameInputBinding source;
  if (device == "GAMEPAD") {
    const GamepadControlName *found = nullptr;
    for (const GamepadControlName &item : kGamepadControls) {
      if (control == item.identifier) {
        found = &item;
      }
    }
    if (found == nullptr) {
      return false;
    }
    source.device = GAME_INPUT_GAMEPAD;
    source.code = found->code;
  }
  else if (device == "KEYBOARD" || device == "MOUSE") {
    int value = 0;
    if (!blender::RNA_enum_value_from_identifier(
            blender::rna_enum_event_type_items, control.c_str(), &value))
    {
      return false;
    }
    const bool mouse = blender::ln_event_is_mouse(value);
    if (mouse != (device == "MOUSE") || (!mouse && !blender::ln_event_is_key(value))) {
      return false;
    }
    source.device = mouse ? GAME_INPUT_MOUSE : GAME_INPUT_KEYBOARD;
    source.code = short(value);
    source.modifiers = short(modifiers);
  }
  else {
    return false;
  }
  for (int index = 0; index < 4; ++index) {
    if (direction == kDirectionIdentifiers[index]) {
      source.direction = short(index);
    }
  }
  return ConvertBinding(source, r_binding);
}

void SaveBinding(blender::io::serialize::ArrayValue &array, const LN_InputBinding &binding)
{
  std::shared_ptr<blender::io::serialize::DictionaryValue> item = array.append_dict();
  const char *control = "";
  if (binding.device == LN_InputDevice::Gamepad) {
    item->append_str("device", "GAMEPAD");
    if (const GamepadControlName *name = FindGamepadControl(binding.code)) {
      control = name->identifier;
    }
  }
  else {
    item->append_str("device", binding.device == LN_InputDevice::Mouse ? "MOUSE" : "KEYBOARD");
    blender::RNA_enum_identifier(blender::rna_enum_event_type_items, binding.code, &control);
  }
  item->append_str("control", control);
  item->append_int("modifiers", binding.modifiers);
  item->append_str("direction", kDirectionIdentifiers[std::min<int>(binding.direction, 3)]);
}

bool IsTrigger(const LN_InputBinding &binding)
{
  return binding.device == LN_InputDevice::Gamepad &&
         (binding.code == GAME_INPUT_GAMEPAD_LEFT_TRIGGER ||
          binding.code == GAME_INPUT_GAMEPAD_RIGHT_TRIGGER);
}

bool SameDeviceClass(const LN_InputBinding &a, const LN_InputBinding &b)
{
  return (a.device == LN_InputDevice::Gamepad) == (b.device == LN_InputDevice::Gamepad);
}

bool IsWholeStick(const LN_InputBinding &binding)
{
  return binding.device == LN_InputDevice::Gamepad &&
         (binding.code == GAME_INPUT_GAMEPAD_LEFT_STICK ||
          binding.code == GAME_INPUT_GAMEPAD_RIGHT_STICK);
}

MT_Vector3 DirectionVector(const int direction)
{
  switch (direction) {
    case GAME_INPUT_DIRECTION_DOWN:
      return MT_Vector3(0.0f, -1.0f, 0.0f);
    case GAME_INPUT_DIRECTION_LEFT:
      return MT_Vector3(-1.0f, 0.0f, 0.0f);
    case GAME_INPUT_DIRECTION_RIGHT:
      return MT_Vector3(1.0f, 0.0f, 0.0f);
    default:
      return MT_Vector3(0.0f, 1.0f, 0.0f);
  }
}

/** The Blender event type of an engine input code (0 if none). */
int16_t BlenderEventForInput(const int input)
{
  static const std::vector<int16_t> table = [] {
    std::vector<int16_t> events(SCA_IInputDevice::MAX_KEYS, 0);
    for (const blender::EnumPropertyItem *item = blender::rna_enum_event_type_items;
         item->identifier != nullptr;
         ++item)
    {
      if (item->identifier[0] == '\0' ||
          !(blender::ln_event_is_key(item->value) || blender::ln_event_is_mouse(item->value)))
      {
        continue;
      }
      const int converted = BL_ConvertKeyCode(item->value);
      if (converted > SCA_IInputDevice::NOKEY && converted < SCA_IInputDevice::MAX_KEYS &&
          events[size_t(converted)] == 0)
      {
        events[size_t(converted)] = int16_t(item->value);
      }
    }
    return events;
  }();
  return input > 0 && input < int(table.size()) ? table[size_t(input)] : 0;
}

bool IsModifierInput(const int input)
{
  return input == SCA_IInputDevice::LEFTSHIFTKEY || input == SCA_IInputDevice::RIGHTSHIFTKEY ||
         input == SCA_IInputDevice::LEFTCTRLKEY || input == SCA_IInputDevice::RIGHTCTRLKEY ||
         input == SCA_IInputDevice::LEFTALTKEY || input == SCA_IInputDevice::RIGHTALTKEY;
}

/** Past the deadzone, rescaled so the stick still reaches 1. */
float StickAmount(const float amount)
{
  return amount > kStickDeadzone ?
             std::min(1.0f, (amount - kStickDeadzone) / (1.0f - kStickDeadzone)) :
             0.0f;
}

}  // namespace

LN_InputMap &LN_InputMap::Get()
{
  static LN_InputMap map = [] {
    LN_InputMap defaults;
    defaults.Load(nullptr);
    return defaults;
  }();
  return map;
}

void LN_InputMap::Load(const blender::GameData *game)
{
  /* The scene's built-in actions are the UI controls, one for one. */
  static_assert(GAME_INPUT_BUILTIN_COUNT == LN_UI_ACTION_COUNT);
  blender::GameData defaults;
  blender::BKE_game_input_map_ensure_builtins(&defaults);

  m_actions.clear();
  m_index.clear();
  std::fill(std::begin(m_builtins), std::end(m_builtins), -1);
  const auto add_action = [&](const blender::GameInputAction &source) {
    const bool builtin = source.builtin > GAME_INPUT_BUILTIN_NONE &&
                         source.builtin <= GAME_INPUT_BUILTIN_COUNT;
    if (builtin ? m_builtins[source.builtin - 1] >= 0 : m_index.contains(source.name)) {
      return;
    }
    LN_InputAction action;
    action.name = source.name;
    action.context = source.context;
    action.vector = source.type == GAME_INPUT_ACTION_VECTOR;
    for (const blender::GameInputBinding &source_binding : source.bindings) {
      LN_InputBinding binding;
      if (ConvertBinding(source_binding, binding)) {
        action.bindings.push_back(binding);
      }
    }
    const int32_t index = int32_t(m_actions.size());
    if (builtin) {
      m_builtins[source.builtin - 1] = index;
    }
    m_index.emplace(action.name, index);
    m_actions.push_back(std::move(action));
  };
  if (game != nullptr) {
    for (const blender::GameInputAction &action : game->input_actions) {
      add_action(action);
    }
  }
  /* A map always has every UI control: the menus rely on them. */
  for (const blender::GameInputAction &action : defaults.input_actions) {
    if (m_builtins[action.builtin - 1] < 0) {
      add_action(action);
    }
  }
  const blender::GameData &settings = game != nullptr ? *game : defaults;
  m_repeatDelay = std::clamp(settings.input_repeat_delay, 0.05f, 2.0f);
  m_repeatInterval = std::clamp(settings.input_repeat_interval, 0.02f, 1.0f);
  m_stickThreshold = std::clamp(settings.input_stick_threshold, 0.1f, 0.95f);
  if ((settings.input_flag & GAME_INPUT_SWAP_ACCEPT_BACK) != 0) {
    /* As on Japanese consoles: the menus accept with B and go back with A. */
    for (const LN_UIAction control : {LN_UIAction::Accept, LN_UIAction::Cancel}) {
      for (LN_InputBinding &binding : m_actions[size_t(FindBuiltin(control))].bindings) {
        if (binding.device == LN_InputDevice::Gamepad && binding.code == GAME_INPUT_GAMEPAD_A) {
          binding.code = GAME_INPUT_GAMEPAD_B;
        }
        else if (binding.device == LN_InputDevice::Gamepad &&
                 binding.code == GAME_INPUT_GAMEPAD_B)
        {
          binding.code = GAME_INPUT_GAMEPAD_A;
        }
      }
    }
  }
  for (LN_InputAction &action : m_actions) {
    action.defaults = action.bindings;
  }
  m_gamepadsFromPlayer1 = (settings.input_flag & GAME_INPUT_GAMEPADS_FROM_PLAYER_1) != 0;
  m_pressToJoin = (settings.input_flag & GAME_INPUT_PRESS_TO_JOIN) != 0;
  blender::BKE_game_input_map_free(&defaults);
  ResetPlayers();
  m_lastUsedGamepad = false;
  /* A new game: no rebind of a previous one is waiting. */
  m_captures = 0;
  m_captureEndRevision = UINT64_MAX;
  m_revision++;
  UpdateShadows();
}

void LN_InputMap::ResetPlayers()
{
  const int32_t first = m_gamepadsFromPlayer1 ? 1 : 0;
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    m_userGamepad[size_t(user)] = !m_pressToJoin && user >= first ? user - first : -1;
  }
  m_joined.fill(false);
  m_hiddenJoinButtons.fill(0);
  m_hiddenJoinLetGo.fill(0);
  m_joins.clear();
  /* Only presses from now on count: a gamepad keeps its counts from earlier games. */
  for (int32_t gamepad = 0; gamepad < LN_MAX_GAMEPADS; ++gamepad) {
    DEV_Joystick *joystick = DEV_Joystick::GetInstance(short(gamepad));
    const bool connected = joystick != nullptr && joystick->Connected();
    m_gamepadSerial[size_t(gamepad)] = connected ? joystick->GetInstanceSerial() : 0;
    m_gamepadFamily[size_t(gamepad)] = connected ? joystick->GetPadFamily() :
                                                   DEV_PadFamily::Generic;
    for (int32_t button = 0; button < LN_MAX_GAMEPAD_BUTTONS; ++button) {
      m_gamepadPresses[size_t(gamepad)][size_t(button)] =
          connected ? joystick->GetButtonPressRevision(button) : 0;
    }
  }
  m_deviceRevision++;
}

int32_t LN_InputMap::UserOfGamepad(const int32_t gamepad) const
{
  if (gamepad < 0) {
    return -1;
  }
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    if (m_userGamepad[size_t(user)] == gamepad) {
      return user;
    }
  }
  return -1;
}

void LN_InputMap::SetUserGamepad(const int32_t user, const int32_t gamepad)
{
  if (user < 0 || user >= LN_MAX_GAMEPADS || gamepad < -1 || gamepad >= LN_MAX_GAMEPADS) {
    return;
  }
  if (gamepad >= 0) {
    const int32_t previous_user = UserOfGamepad(gamepad);
    if (previous_user >= 0 && previous_user != user) {
      m_userGamepad[size_t(previous_user)] = -1;
      m_joined[size_t(previous_user)] = false;
    }
  }
  else {
    /* No gamepad: the player left, and joins again with one. */
    m_joined[size_t(user)] = false;
  }
  if (m_userGamepad[size_t(user)] != gamepad) {
    m_userGamepad[size_t(user)] = gamepad;
    m_deviceRevision++;
  }
}

void LN_InputMap::UpdatePlayers()
{
  std::array<GamepadButtons, LN_MAX_GAMEPADS> gamepads;
  for (int32_t gamepad = 0; gamepad < LN_MAX_GAMEPADS; ++gamepad) {
    DEV_Joystick *joystick = DEV_Joystick::GetInstance(short(gamepad));
    GamepadButtons &buttons = gamepads[size_t(gamepad)];
    buttons.connected = joystick != nullptr && joystick->Connected();
    if (!buttons.connected) {
      continue;
    }
    buttons.serial = joystick->GetInstanceSerial();
    buttons.family = joystick->GetPadFamily();
    for (int32_t button = 0; button < LN_MAX_GAMEPAD_BUTTONS; ++button) {
      buttons.presses[size_t(button)] = joystick->GetButtonPressRevision(button);
      if (joystick->aButtonPressIsPositive(button)) {
        buttons.held |= 1u << uint32_t(button);
      }
    }
  }
  UpdatePlayers(gamepads);
}

void LN_InputMap::UpdatePlayers(const std::array<GamepadButtons, LN_MAX_GAMEPADS> &gamepads)
{
  const int32_t first = m_gamepadsFromPlayer1 ? 1 : 0;
  for (int32_t gamepad = 0; gamepad < LN_MAX_GAMEPADS; ++gamepad) {
    const size_t index = size_t(gamepad);
    const GamepadButtons &buttons = gamepads[index];
    if (!buttons.connected) {
      if (m_gamepadSerial[index] != 0) {
        /* The texts of a player without a gamepad may have named this one. */
        m_deviceRevision++;
      }
      m_gamepadSerial[index] = 0;
      m_hiddenJoinButtons[index] = 0;
      m_hiddenJoinLetGo[index] = 0;
      continue;
    }
    if (m_gamepadSerial[index] != buttons.serial) {
      /* A gamepad plugged in: its presses count from 0, and texts name its own buttons
       * ("{Jump}" reads "Cross" on a PlayStation pad). */
      m_gamepadSerial[index] = buttons.serial;
      m_gamepadPresses[index].fill(0);
      m_gamepadFamily[index] = buttons.family;
      m_deviceRevision++;
    }
    uint32_t pressed = 0;
    for (int32_t button = 0; button < LN_MAX_GAMEPAD_BUTTONS; ++button) {
      const uint64_t presses = buttons.presses[size_t(button)];
      if (presses != m_gamepadPresses[index][size_t(button)]) {
        pressed |= 1u << uint32_t(button);
        m_gamepadPresses[index][size_t(button)] = presses;
      }
    }
    /* A hidden join press shows again one update after it is let go, so the frame that sees its
     * release hides that too. */
    m_hiddenJoinButtons[index] &= ~m_hiddenJoinLetGo[index];
    m_hiddenJoinLetGo[index] = m_hiddenJoinButtons[index] & ~buttons.held;
    if (pressed == 0) {
      continue;
    }
    int32_t user = UserOfGamepad(gamepad);
    if (user < 0 && m_pressToJoin) {
      /* A gamepad plugged back in takes the place of a player whose gamepad is gone (no new join
       * if they had joined); else it gets the first free player. */
      int32_t returning = -1;
      int32_t free_user = -1;
      for (int32_t candidate = first; candidate < LN_MAX_GAMEPADS; ++candidate) {
        const int32_t had = m_userGamepad[size_t(candidate)];
        if (had >= 0 && had < LN_MAX_GAMEPADS && !gamepads[size_t(had)].connected) {
          returning = candidate;
          break;
        }
        if (had < 0 && free_user < 0) {
          free_user = candidate;
        }
      }
      const int32_t taker = returning >= 0 ? returning : free_user;
      if (taker < 0) {
        continue;
      }
      m_userGamepad[size_t(taker)] = gamepad;
      m_hiddenJoinButtons[index] = buttons.held | pressed;
      m_hiddenJoinLetGo[index] = m_hiddenJoinButtons[index] & ~buttons.held;
      m_deviceRevision++;
      /* A returning player who had joined doesn't join again. */
      user = taker;
    }
    if (user < 0 || m_joined[size_t(user)]) {
      continue;
    }
    m_joined[size_t(user)] = true;
    m_joins.push_back({++m_joinSerial, user, gamepad});
    if (m_joins.size() > 16) {
      m_joins.erase(m_joins.begin());
    }
  }
}

bool LN_InputMap::NextJoin(uint64_t &io_serial, int32_t &r_user, int32_t &r_gamepad) const
{
  for (const PlayerJoin &join : m_joins) {
    if (join.serial > io_serial) {
      io_serial = join.serial;
      r_user = join.user;
      r_gamepad = join.gamepad;
      return true;
    }
  }
  return false;
}

int32_t LN_InputMap::Find(const std::string &name) const
{
  const auto item = m_index.find(name);
  return item != m_index.end() ? item->second : -1;
}

bool LN_InputMap::GameActionUsesKey(const int16_t input) const
{
  for (size_t index = 0; index < m_actions.size(); ++index) {
    if (std::find(std::begin(m_builtins), std::end(m_builtins), int32_t(index)) !=
        std::end(m_builtins))
    {
      continue;
    }
    for (const LN_InputBinding &binding : m_actions[index].bindings) {
      if (binding.device == LN_InputDevice::Keyboard && binding.input == input &&
          binding.modifiers == 0)
      {
        return true;
      }
    }
  }
  return false;
}

void LN_InputMap::UpdateShadows()
{
  for (LN_InputAction &action : m_actions) {
    for (LN_InputBinding &binding : action.bindings) {
      binding.shadowed_by = 0;
      if (binding.device == LN_InputDevice::Gamepad) {
        continue;
      }
      for (const LN_InputAction &other_action : m_actions) {
        for (const LN_InputBinding &other : other_action.bindings) {
          if (other.device != LN_InputDevice::Gamepad && other.input == binding.input &&
              other.modifiers != binding.modifiers &&
              (other.modifiers & binding.modifiers) == binding.modifiers)
          {
            binding.shadowed_by |= uint8_t(1u << other.modifiers);
          }
        }
      }
    }
  }
}

void LN_InputMap::Rebind(const int32_t action_index, const LN_InputBinding &binding)
{
  if (action_index < 0 || action_index >= int32_t(m_actions.size())) {
    return;
  }
  LN_InputAction &action = m_actions[size_t(action_index)];
  LN_InputBinding *target = nullptr;
  for (LN_InputBinding &existing : action.bindings) {
    if (!SameDeviceClass(existing, binding)) {
      continue;
    }
    /* A vector action keeps one binding per direction, and its whole stick apart. */
    if (action.vector && (IsWholeStick(existing) != IsWholeStick(binding) ||
                          (!IsWholeStick(binding) && existing.direction != binding.direction)))
    {
      continue;
    }
    target = &existing;
    break;
  }
  if (target != nullptr) {
    *target = binding;
  }
  else {
    action.bindings.push_back(binding);
  }
  m_revision++;
  UpdateShadows();
}

void LN_InputMap::Reset()
{
  for (LN_InputAction &action : m_actions) {
    action.bindings = action.defaults;
  }
  m_revision++;
  UpdateShadows();
}

std::string LN_InputMap::Save() const
{
  using namespace blender::io::serialize;
  DictionaryValue root;
  root.append_int("input_map", 1);
  std::shared_ptr<DictionaryValue> actions = root.append_dict("actions");
  for (const LN_InputAction &action : m_actions) {
    if (action.bindings == action.defaults) {
      continue;
    }
    std::shared_ptr<ArrayValue> bindings = actions->append_array(action.name);
    for (const LN_InputBinding &binding : action.bindings) {
      SaveBinding(*bindings, binding);
    }
  }
  std::ostringstream stream;
  JsonFormatter formatter;
  formatter.indentation_len = 2;
  formatter.serialize(stream, root);
  return stream.str();
}

bool LN_InputMap::Restore(const std::string &json)
{
  using namespace blender::io::serialize;
  std::istringstream stream(json);
  JsonFormatter formatter;
  const std::unique_ptr<Value> root = formatter.deserialize(stream);
  const DictionaryValue *dictionary = root ? root->as_dictionary_value() : nullptr;
  if (dictionary == nullptr || !dictionary->lookup_int("input_map")) {
    return false;
  }
  for (LN_InputAction &action : m_actions) {
    action.bindings = action.defaults;
  }
  if (const DictionaryValue *actions = dictionary->lookup_dict("actions")) {
    for (const DictionaryValue::Item &item : actions->elements()) {
      const int32_t index = Find(item.first);
      const ArrayValue *bindings = item.second ? item.second->as_array_value() : nullptr;
      if (index < 0 || bindings == nullptr) {
        continue;
      }
      std::vector<LN_InputBinding> restored;
      for (const std::shared_ptr<Value> &element : bindings->elements()) {
        const DictionaryValue *fields = element ? element->as_dictionary_value() : nullptr;
        if (fields == nullptr) {
          continue;
        }
        LN_InputBinding binding;
        if (BindingFromNames(std::string(fields->lookup_str("device").value_or("")),
                             std::string(fields->lookup_str("control").value_or("")),
                             int(fields->lookup_int("modifiers").value_or(0)),
                             std::string(fields->lookup_str("direction").value_or("UP")),
                             binding))
        {
          restored.push_back(binding);
        }
      }
      m_actions[size_t(index)].bindings = std::move(restored);
    }
  }
  m_revision++;
  UpdateShadows();
  return true;
}

std::string LN_InputMap::BindingText(const LN_InputBinding &binding, const DEV_PadFamily family)
{
  if (binding.device == LN_InputDevice::Gamepad) {
    const GamepadControlName *control = FindGamepadControl(binding.code);
    return control != nullptr ? GamepadControlText(*control, family) : "";
  }
  std::string text;
  if ((binding.modifiers & GAME_INPUT_MOD_CTRL) != 0) {
    text += "Ctrl+";
  }
  if ((binding.modifiers & GAME_INPUT_MOD_ALT) != 0) {
    text += "Alt+";
  }
  if ((binding.modifiers & GAME_INPUT_MOD_SHIFT) != 0) {
    text += "Shift+";
  }
  const char *name = nullptr;
  if (blender::RNA_enum_name(blender::rna_enum_event_type_items, binding.code, &name)) {
    text += name;
  }
  return text;
}

std::string LN_InputMap::ActionText(const int32_t action_index,
                                    LN_InputDeviceClass device_class,
                                    const int direction,
                                    const DEV_PadFamily family) const
{
  if (action_index < 0 || action_index >= int32_t(m_actions.size())) {
    return "";
  }
  if (device_class == LN_InputDeviceClass::Automatic) {
    device_class = m_lastUsedGamepad ? LN_InputDeviceClass::Gamepad :
                                       LN_InputDeviceClass::KeyboardMouse;
  }
  const LN_InputAction &action = m_actions[size_t(action_index)];
  for (const LN_InputBinding &binding : action.bindings) {
    if ((binding.device == LN_InputDevice::Gamepad) !=
        (device_class == LN_InputDeviceClass::Gamepad))
    {
      continue;
    }
    if (action.vector && !IsWholeStick(binding) && binding.direction != direction) {
      continue;
    }
    return BindingText(binding, family);
  }
  return "";
}

DEV_PadFamily LN_InputMap::PadFamilyOfUser(const int32_t user) const
{
  const int32_t gamepad = GamepadOfUser(user);
  if (gamepad >= 0 && gamepad < LN_MAX_GAMEPADS) {
    return m_gamepadFamily[size_t(gamepad)];
  }
  /* No gamepad of their own (the keyboard player of Gamepads Start at Player 1): the first
   * gamepad plugged in, so a sign everyone reads names the pad in use. */
  for (size_t index = 0; index < m_gamepadSerial.size(); ++index) {
    if (m_gamepadSerial[index] != 0) {
      return m_gamepadFamily[index];
    }
  }
  return DEV_PadFamily::Generic;
}

void LN_InputMap::NoteInput(const LN_InputSnapshot &input)
{
  if (const LN_GamepadSnapshot *gamepad = input.GetGamepad(GamepadOfUser(0));
      gamepad != nullptr && gamepad->connected)
  {
    const bool button = std::any_of(gamepad->pressed.begin(),
                                    gamepad->pressed.end(),
                                    [](const uint8_t pressed) { return pressed != 0; });
    const bool stick = LN_GamepadStickPosition(*gamepad, 1).length() >= m_stickThreshold ||
                       LN_GamepadStickPosition(*gamepad, 2).length() >= m_stickThreshold;
    if (button || stick || gamepad->axes_pressed[4] != 0 || gamepad->axes_pressed[5] != 0) {
      if (!m_lastUsedGamepad) {
        m_lastUsedGamepad = true;
        m_deviceRevision++;
      }
      return;
    }
  }
  bool keyboard_or_mouse = false;
  for (int32_t code = SCA_IInputDevice::BEGINKEY; code < SCA_IInputDevice::ENDMOUSEBUTTONS;
       ++code)
  {
    if (input.GetStatus(code, SCA_InputEvent::JUSTACTIVATED)) {
      keyboard_or_mouse = true;
      break;
    }
  }
  const LN_MouseSnapshot &mouse = input.GetMouse();
  keyboard_or_mouse = keyboard_or_mouse || mouse.wheel_delta != 0 ||
                      std::abs(mouse.delta_x) + std::abs(mouse.delta_y) > 2;
  if (keyboard_or_mouse && m_lastUsedGamepad) {
    m_lastUsedGamepad = false;
    m_deviceRevision++;
  }
}

std::string LN_InputMap::ActionTextForDevice(const int32_t action,
                                             const LN_InputDeviceClass device,
                                             const DEV_PadFamily family) const
{
  if (action < 0 || action >= int32_t(m_actions.size()) || !m_actions[size_t(action)].vector) {
    return ActionText(action, device, 0, family);
  }
  /* A vector action names all its directions: one stick, or the keys in the order W A S D
   * ("WASD", else "Up/Left/Down/Right"). */
  std::vector<std::string> parts;
  bool single_characters = true;
  for (const int direction : {0, 2, 1, 3}) {
    std::string part = ActionText(action, device, direction, family);
    if (part.empty() || std::find(parts.begin(), parts.end(), part) != parts.end()) {
      continue;
    }
    single_characters = single_characters && part.size() == 1;
    parts.push_back(std::move(part));
  }
  std::string text;
  for (const std::string &part : parts) {
    text += (text.empty() || single_characters ? "" : "/") + part;
  }
  return text;
}

std::string LN_InputMap::ActionTextForUser(const int32_t action, const int32_t user) const
{
  const bool gamepad = user != 0 || m_lastUsedGamepad;
  const DEV_PadFamily family = PadFamilyOfUser(user);
  std::string text = ActionTextForDevice(
      action, gamepad ? LN_InputDeviceClass::Gamepad : LN_InputDeviceClass::KeyboardMouse, family);
  if (text.empty()) {
    text = ActionTextForDevice(
        action,
        gamepad ? LN_InputDeviceClass::KeyboardMouse : LN_InputDeviceClass::Gamepad,
        family);
  }
  return text;
}

std::string LN_InputMap::ExpandActionNames(const std::string &text,
                                           const int32_t user,
                                           const TokenResolver *resolver) const
{
  std::string result;
  result.reserve(text.size());
  size_t position = 0;
  while (position < text.size()) {
    /* A name is the text between a '}' and the nearest '{' before it ("{ or {Grab}"). */
    const size_t first_open = text.find('{', position);
    const size_t close = first_open == std::string::npos ? std::string::npos :
                                                           text.find('}', first_open);
    if (close == std::string::npos) {
      result.append(text, position, std::string::npos);
      break;
    }
    const size_t open = text.rfind('{', close);
    result.append(text, position, open - position);
    /* "{Jump:Gamepad}" or "{Jump:Keyboard}" names that device's control whatever the player
     * uses (a key list with a column for each). */
    std::string action_name = text.substr(open + 1, close - open - 1);
    std::optional<LN_InputDeviceClass> device;
    if (const size_t colon = action_name.rfind(':'); colon != std::string::npos) {
      const std::string suffix = action_name.substr(colon + 1);
      if (suffix == "Gamepad") {
        device = LN_InputDeviceClass::Gamepad;
      }
      else if (suffix == "Keyboard" || suffix == "Mouse") {
        device = LN_InputDeviceClass::KeyboardMouse;
      }
      if (device) {
        action_name.resize(colon);
      }
    }
    const int32_t action = Find(action_name);
    const std::string name = action < 0 ? std::string() :
                             device     ? ActionTextForDevice(
                                          action, *device, PadFamilyOfUser(user)) :
                                          ActionTextForUser(action, user);
    std::string resolved;
    if (!name.empty()) {
      result += name;
    }
    else if (action < 0 && resolver != nullptr && open + 1 < close &&
             (*resolver)(text.substr(open + 1, close - open - 1), resolved))
    {
      result += resolved;
    }
    else {
      result.append(text, open, close - open + 1);
    }
    position = close + 1;
  }
  return result;
}

void LN_InputMap::BeginCapture()
{
  m_captures++;
}

void LN_InputMap::EndCapture(const uint64_t device_revision)
{
  m_captures = std::max(0, m_captures - 1);
  m_captureEndRevision = device_revision;
}

LN_InputCapture::Result LN_InputCapture::Update(const LN_InputSnapshot &input,
                                                const int32_t user,
                                                const LN_InputDeviceClass device_class,
                                                const bool vector,
                                                LN_InputBinding &r_binding)
{
  r_binding = LN_InputBinding();
  if (input.GetStatus(SCA_IInputDevice::ESCKEY, SCA_InputEvent::JUSTACTIVATED)) {
    return Result::Cancelled;
  }
  if (device_class != LN_InputDeviceClass::Gamepad) {
    if (user != 0) {
      return Result::Waiting;
    }
    const uint8_t held = LN_InputModifiersHeld(input);
    const auto capture = [&](const int input_code, const uint8_t modifiers) {
      const int16_t event = BlenderEventForInput(input_code);
      if (event == 0) {
        return false;
      }
      r_binding.device = blender::ln_event_is_key(event) ? LN_InputDevice::Keyboard :
                                                           LN_InputDevice::Mouse;
      r_binding.code = event;
      r_binding.input = int16_t(input_code);
      r_binding.modifiers = modifiers;
      return true;
    };
    for (int code = SCA_IInputDevice::BEGINKEY + 1; code < SCA_IInputDevice::ENDMOUSEBUTTONS;
         ++code)
    {
      if (!input.GetStatus(code, SCA_InputEvent::JUSTACTIVATED)) {
        continue;
      }
      if (IsModifierInput(code)) {
        if (modifier_input == 0) {
          modifier_input = code;
        }
        continue;
      }
      if (capture(code, held)) {
        return Result::Captured;
      }
    }
    const int32_t wheel = input.GetMouse().wheel_delta;
    if (wheel != 0 &&
        capture(wheel > 0 ? SCA_IInputDevice::WHEELUPMOUSE : SCA_IInputDevice::WHEELDOWNMOUSE,
                held))
    {
      return Result::Captured;
    }
    /* Shift, Ctrl or Alt alone becomes the binding when it is let go. */
    if (modifier_input != 0 &&
        input.GetStatus(modifier_input, SCA_InputEvent::JUSTRELEASED) &&
        capture(modifier_input, 0))
    {
      return Result::Captured;
    }
    return Result::Waiting;
  }

  const LN_GamepadSnapshot *gamepad = input.GetGamepad(LN_InputMap::Get().GamepadOfUser(user));
  if (gamepad == nullptr || !gamepad->connected) {
    return Result::Waiting;
  }
  r_binding.device = LN_InputDevice::Gamepad;
  for (const GamepadControlName &control : kGamepadControls) {
    if (control.code <= GAME_INPUT_GAMEPAD_DPAD_RIGHT &&
        gamepad->pressed[size_t(control.code)] != 0)
    {
      r_binding.code = control.code;
      return Result::Captured;
    }
  }
  for (const int16_t trigger : {GAME_INPUT_GAMEPAD_LEFT_TRIGGER, GAME_INPUT_GAMEPAD_RIGHT_TRIGGER}) {
    if (gamepad->axes_pressed[trigger == GAME_INPUT_GAMEPAD_LEFT_TRIGGER ? 4 : 5] != 0) {
      r_binding.code = trigger;
      return Result::Captured;
    }
  }
  const float threshold = LN_InputMap::Get().StickThreshold();
  for (int stick = 1; stick <= 2; ++stick) {
    const MT_Vector3 position = LN_GamepadStickPosition(*gamepad, stick);
    if (position.length() < threshold * kStickRelease) {
      stick_centred[stick - 1] = true;
      continue;
    }
    if (!stick_centred[stick - 1] || position.length() < threshold) {
      continue;
    }
    if (vector) {
      r_binding.code = stick == 1 ? GAME_INPUT_GAMEPAD_LEFT_STICK : GAME_INPUT_GAMEPAD_RIGHT_STICK;
      return Result::Captured;
    }
    /* The direction pushed furthest. */
    int direction = std::abs(position.x()) > std::abs(position.y()) ?
                        (position.x() > 0.0f ? GAME_INPUT_DIRECTION_RIGHT :
                                               GAME_INPUT_DIRECTION_LEFT) :
                        (position.y() > 0.0f ? GAME_INPUT_DIRECTION_UP :
                                               GAME_INPUT_DIRECTION_DOWN);
    r_binding.code = int16_t((stick == 1 ? GAME_INPUT_GAMEPAD_LEFT_STICK_UP :
                                           GAME_INPUT_GAMEPAD_RIGHT_STICK_UP) +
                             direction);
    r_binding.direction = uint8_t(direction);
    return Result::Captured;
  }
  return Result::Waiting;
}

uint8_t LN_InputModifiersHeld(const LN_InputSnapshot &input)
{
  const auto held = [&](const SCA_IInputDevice::SCA_EnumInputs left,
                        const SCA_IInputDevice::SCA_EnumInputs right) {
    return input.GetStatus(left, SCA_InputEvent::ACTIVE) ||
           input.GetStatus(right, SCA_InputEvent::ACTIVE);
  };
  uint8_t modifiers = 0;
  if (held(SCA_IInputDevice::LEFTSHIFTKEY, SCA_IInputDevice::RIGHTSHIFTKEY)) {
    modifiers |= GAME_INPUT_MOD_SHIFT;
  }
  if (held(SCA_IInputDevice::LEFTCTRLKEY, SCA_IInputDevice::RIGHTCTRLKEY)) {
    modifiers |= GAME_INPUT_MOD_CTRL;
  }
  if (held(SCA_IInputDevice::LEFTALTKEY, SCA_IInputDevice::RIGHTALTKEY)) {
    modifiers |= GAME_INPUT_MOD_ALT;
  }
  return modifiers;
}

LN_InputBindingReading LN_ReadInputBinding(const LN_InputBinding &binding,
                                           const LN_InputSnapshot &input,
                                           const int32_t user,
                                           const uint8_t held_modifiers)
{
  LN_InputBindingReading reading;
  if (binding.device != LN_InputDevice::Gamepad) {
    if (user != 0) {
      return reading;
    }
    bool modifiers_match = (held_modifiers & binding.modifiers) == binding.modifiers;
    for (int combination = 1; combination < 8 && modifiers_match; ++combination) {
      /* A binding of the same key with more of the held modifiers wins (Shift+Tab over Tab). */
      if ((binding.shadowed_by & (1u << combination)) != 0 &&
          (held_modifiers & combination) == combination)
      {
        modifiers_match = false;
      }
    }
    if (!modifiers_match) {
      return reading;
    }
    if (IsWheel(binding)) {
      const int32_t wheel = input.GetMouse().wheel_delta;
      reading.level = reading.pressed = binding.code == blender::WHEELUPMOUSE ? wheel > 0 :
                                                                                 wheel < 0;
      reading.value = reading.level ? 1.0f : 0.0f;
      return reading;
    }
    reading.level = input.GetStatus(binding.input, SCA_InputEvent::ACTIVE);
    reading.pressed = input.GetStatus(binding.input, SCA_InputEvent::JUSTACTIVATED);
    reading.released = input.GetStatus(binding.input, SCA_InputEvent::JUSTRELEASED);
    reading.value = reading.level ? 1.0f : 0.0f;
    return reading;
  }

  const LN_GamepadSnapshot *gamepad = input.GetGamepad(LN_InputMap::Get().GamepadOfUser(user));
  if (gamepad == nullptr || !gamepad->connected || LN_InputBindingStick(binding) != 0) {
    return reading;
  }
  const bool trigger = binding.code == GAME_INPUT_GAMEPAD_LEFT_TRIGGER ||
                       binding.code == GAME_INPUT_GAMEPAD_RIGHT_TRIGGER;
  /* SDL puts the triggers on axes 4 and 5. */
  const int32_t index = trigger ? (binding.code == GAME_INPUT_GAMEPAD_LEFT_TRIGGER ? 4 : 5) :
                                  binding.code;
  reading.level = LN_GetGamepadControlState(*gamepad, index, trigger, SCA_InputEvent::ACTIVE);
  reading.pressed = LN_GetGamepadControlState(
      *gamepad, index, trigger, SCA_InputEvent::JUSTACTIVATED);
  reading.released = LN_GetGamepadControlState(
      *gamepad, index, trigger, SCA_InputEvent::JUSTRELEASED);
  if (trigger) {
    float strength = 0.0f;
    LN_TryGetGamepadTriggerStrength(*gamepad, index, strength);
    reading.value = strength;
  }
  else {
    reading.value = reading.level ? 1.0f : 0.0f;
  }
  return reading;
}

int LN_InputBindingStick(const LN_InputBinding &binding)
{
  if (binding.device != LN_InputDevice::Gamepad) {
    return 0;
  }
  if ((binding.code >= GAME_INPUT_GAMEPAD_LEFT_STICK_UP &&
       binding.code <= GAME_INPUT_GAMEPAD_LEFT_STICK_RIGHT) ||
      binding.code == GAME_INPUT_GAMEPAD_LEFT_STICK)
  {
    return 1;
  }
  if ((binding.code >= GAME_INPUT_GAMEPAD_RIGHT_STICK_UP &&
       binding.code <= GAME_INPUT_GAMEPAD_RIGHT_STICK_RIGHT) ||
      binding.code == GAME_INPUT_GAMEPAD_RIGHT_STICK)
  {
    return 2;
  }
  return 0;
}

MT_Vector3 LN_GamepadStickPosition(const LN_GamepadSnapshot &gamepad, const int stick)
{
  if (!gamepad.connected || (stick != 1 && stick != 2)) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  /* SDL axes 0 and 1 are the left stick, 2 and 3 the right; down is positive. */
  const size_t axis = stick == 1 ? 0 : 2;
  return MT_Vector3(std::clamp(float(gamepad.axes[axis]) / 32767.0f, -1.0f, 1.0f),
                    -std::clamp(float(gamepad.axes[axis + 1]) / 32767.0f, -1.0f, 1.0f),
                    0.0f);
}

void LN_InputActionTracker::Update(const LN_InputSnapshot &input,
                                   const LN_InputContexts &contexts,
                                   const uint64_t tick)
{
  const LN_InputMap &map = LN_InputMap::Get();
  const bool gap = m_tick == UINT64_MAX || tick != m_tick + 1;
  m_tick = tick;
  m_fresh = gap;
  if (m_revision != map.Revision()) {
    m_revision = map.Revision();
    m_firstBinding.assign(map.Size() + 1, 0);
    for (size_t action = 0; action < map.Size(); ++action) {
      m_firstBinding[action + 1] = m_firstBinding[action] +
                                   uint32_t(map.Action(int32_t(action)).bindings.size());
    }
    /* Rebinding keeps the actions: an action held through it is still held. */
    if (m_states.size() != map.Size() * LN_MAX_GAMEPADS) {
      m_states.assign(map.Size() * LN_MAX_GAMEPADS, LN_InputActionState());
    }
    m_levels.assign(size_t(m_firstBinding.back()) * LN_MAX_GAMEPADS, 0);
    m_fresh = true;
  }
  if (gap) {
    std::fill(m_states.begin(), m_states.end(), LN_InputActionState());
  }
  m_statesBefore = m_states;
  m_levelsBefore = m_levels;
  Sample(input, contexts);
}

void LN_InputActionTracker::Refresh(const LN_InputSnapshot &input,
                                    const LN_InputContexts &contexts)
{
  if (m_tick == UINT64_MAX || m_revision != LN_InputMap::Get().Revision()) {
    return;
  }
  m_states = m_statesBefore;
  m_levels = m_levelsBefore;
  Sample(input, contexts);
}

void LN_InputActionTracker::Sample(const LN_InputSnapshot &input,
                                   const LN_InputContexts &contexts)
{
  const LN_InputMap &map = LN_InputMap::Get();
  const uint8_t held_modifiers = LN_InputModifiersHeld(input);
  const float threshold = map.StickThreshold();
  for (size_t action_index = 0; action_index < map.Size(); ++action_index) {
    const LN_InputAction &action = map.Action(int32_t(action_index));
    for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
      /* Each player's own menu sets their context. */
      const bool in_context = action.context.empty() || action.context == contexts[size_t(user)];
      LN_InputActionState &state = m_states[action_index * LN_MAX_GAMEPADS + size_t(user)];
      const LN_GamepadSnapshot *gamepad = input.GetGamepad(map.GamepadOfUser(user));
      const bool connected = gamepad != nullptr && gamepad->connected;
      if (user != 0 && !connected && !state.active) {
        state = LN_InputActionState();
        continue;
      }
      const bool was_active = state.active;
      LN_InputActionState next;
      bool pressed = false;
      bool released = false;
      /* Each wheel notch is a press of its own, also while the action is still active. */
      bool notch = false;
      for (size_t binding_index = 0; binding_index < action.bindings.size(); ++binding_index) {
        const LN_InputBinding &binding = action.bindings[binding_index];
        uint8_t &held_before =
            m_levels[(size_t(m_firstBinding[action_index]) + binding_index) * LN_MAX_GAMEPADS +
                     size_t(user)];
        LN_InputBindingReading reading;
        MT_Vector3 push = DirectionVector(binding.direction);
        if (!in_context) {
          /* Nothing reads as held outside the action's context. */
        }
        else if (const int stick = LN_InputBindingStick(binding); stick != 0) {
          const MT_Vector3 position = connected ? LN_GamepadStickPosition(*gamepad, stick) :
                                                  MT_Vector3(0.0f, 0.0f, 0.0f);
          float amount = 0.0f;
          if (IsWholeStick(binding)) {
            amount = position.length();
            push = amount > 0.0f ? position / amount : MT_Vector3(0.0f, 0.0f, 0.0f);
          }
          else {
            const int direction = (binding.code - GAME_INPUT_GAMEPAD_LEFT_STICK_UP) % 4;
            amount = std::max(0.0f, DirectionVector(direction).dot(position));
          }
          reading.level = amount >= (held_before != 0 ? threshold * kStickRelease : threshold);
          reading.pressed = reading.level && held_before == 0 && !m_fresh;
          reading.value = StickAmount(amount);
        }
        else {
          reading = LN_ReadInputBinding(binding, input, user, held_modifiers);
          if (m_fresh && IsTrigger(binding)) {
            /* Its press may be the one a rebind took. */
            reading.pressed = false;
          }
          notch = notch || (reading.pressed && IsWheel(binding));
        }
        /* A release counts only for a binding that was held, not for Tab let go without the
         * Shift its Shift+Tab binding needs. */
        released = released || (reading.released && (held_before != 0 || reading.pressed));
        pressed = pressed || reading.pressed;
        held_before = reading.level ? 1 : 0;
        next.active = next.active || reading.level;
        next.value = std::max(next.value, reading.value);
        if (action.vector) {
          next.vector += push * reading.value;
        }
      }
      if (action.vector) {
        const float length = next.vector.length();
        if (length > 1.0f) {
          next.vector /= length;
        }
        next.value = std::min(1.0f, length);
      }
      /* Pressing a second binding of an action already held does not press it again. */
      next.pressed = (pressed && !was_active) || notch;
      next.released = !next.active && (was_active || released);
      state = next;
    }
  }
}

const LN_InputActionState *LN_InputActionTracker::Find(const int32_t action,
                                                       const int32_t user) const
{
  if (action < 0 || user < 0 || user >= LN_MAX_GAMEPADS ||
      size_t(action) * LN_MAX_GAMEPADS + size_t(user) >= m_states.size())
  {
    return nullptr;
  }
  return &m_states[size_t(action) * LN_MAX_GAMEPADS + size_t(user)];
}

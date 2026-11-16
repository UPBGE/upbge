/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): snailrose, lordloki
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Device/DEV_Joystick.cpp
 *  \ingroup device
 */

#include "DEV_Joystick.h"

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef WITH_SDL
#  include <SDL3/SDL.h>
#endif

#include "BKE_appdir.hh"
#include "BLI_path_utils.hh"

#include "CM_Message.h"
#include "DEV_JoystickPrivate.h"

using namespace blender;

#ifdef WITH_SDL
static_assert(JOYBUT_MAX == SDL_GAMEPAD_BUTTON_COUNT, "JOYBUT_MAX must follow SDL");
static_assert(JOYBUT_MAX <= 32, "button state is a single 32-bit mask");
#endif

DEV_Joystick::DEV_Joystick(short bge_joystick_slot)
    : m_bge_joystick_slot(bge_joystick_slot),
      m_axismax(-1),
      m_buttonmax(-1),
      m_isinit(0),
      m_istrig_axis(0),
      m_istrig_button(0),
      m_buttons(0),
      m_pressed(0)
{
  for (int i = 0; i < JOYAXIS_MAX; i++)
    m_axis_array[i] = 0;

#ifdef WITH_SDL
  m_private = new PrivateData();
#endif
}

DEV_Joystick::~DEV_Joystick()
{
}

DEV_Joystick *DEV_Joystick::m_instance[JOYINDEX_MAX];
int DEV_Joystick::m_numRawJoysticks = 0;
uint32_t DEV_Joystick::m_rawJoystickIds[JOYINDEX_MAX] = {};

/** Names of the hints set through SetHint(), reset by Close(). */
static std::vector<std::string> g_hints_set;

void DEV_Joystick::Init()
{
#ifdef WITH_SDL

  /* To have xbox gamepads vibrations working on windows in recent SDL versions. To be tested with other gamepads/OS */
  SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");

  /* SDL 3 defaults to "1": PlayStation and Switch pads connected over Bluetooth are switched to
   * enhanced reports as soon as they are opened, and PlayStation pads stay that way until power
   * cycled, which breaks DirectInput for other applications. "auto" still advertises rumble,
   * LED and sensors, but SDL switches the report mode only when the game uses one of them. */
  SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "auto");

  /* Initializing Game Controller related subsystems */
  bool success = SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC);

  if (success) {
    /* Game controller data base loading */
    const std::optional<std::string> path = blender::BKE_appdir_folder_id(
        blender::BLENDER_DATAFILES,
                                                                 "gamecontroller");

    if (path.has_value()) {
      char fullpath[FILE_MAX];
      blender::BLI_path_join(fullpath, path->c_str(), "gamecontrollerdb.txt");

      if ((SDL_AddGamepadMappingsFromFile(fullpath)) == -1) {
        CM_Warning(
            "gamecontrollerdb.txt file not loaded, we will load SDL gamecontroller internal "
            "database (more restricted)");
      }
    }
  }
  else {
    CM_Error("initializing SDL Game Controller: " << SDL_GetError());
  }
#endif
}

void DEV_Joystick::Close()
{
#ifdef WITH_SDL
  /* Closing possible connected Joysticks */
  for (int i = 0; i < JOYINDEX_MAX; i++) {
    if (m_instance[i]) {
      m_instance[i]->ReleaseInstance(i);
    }
  }

  /* Closing SDL Game controller system */
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC);

  for (const std::string &name : g_hints_set) {
    SDL_ResetHint(name.c_str());
  }
  g_hints_set.clear();
#endif
}

bool DEV_Joystick::IsJoystickHint(const char *name)
{
  return std::strncmp(name, "SDL_JOYSTICK_", 13) == 0 ||
         std::strncmp(name, "SDL_GAMECONTROLLER", 18) == 0 ||
         std::strncmp(name, "SDL_HIDAPI_", 11) == 0;
}

bool DEV_Joystick::SetHint(const char *name, const char *value)
{
#ifdef WITH_SDL
  if (!IsJoystickHint(name)) {
    return false;
  }
  if (std::find(g_hints_set.begin(), g_hints_set.end(), name) == g_hints_set.end()) {
    g_hints_set.emplace_back(name);
  }
  /* False when a higher-priority value (an environment variable) already sets the hint. */
  return SDL_SetHint(name, value);
#else
  return false;
#endif
}

DEV_Joystick *DEV_Joystick::GetInstance(short bge_joystick_slot)
{
#ifndef WITH_SDL
  return nullptr;
#else  /* WITH_SDL */

  if (bge_joystick_slot < 0 || bge_joystick_slot >= JOYINDEX_MAX) {
    CM_Error("invalid joystick index: " << bge_joystick_slot);
    return nullptr;
  }

  return m_instance[bge_joystick_slot];
#endif /* WITH_SDL */
}

#ifdef WITH_SDL
void DEV_Joystick::SetInstanceId(SDL_JoystickID sdl_joystick_id)
{
  m_private->m_sdl_joystick_id = sdl_joystick_id;
}
#endif

void DEV_Joystick::ReleaseInstance(short bge_joystick_slot)
{
#ifdef WITH_SDL
  if (m_instance[bge_joystick_slot]) {
    m_instance[bge_joystick_slot]->DestroyJoystickDevice();
    delete m_instance[bge_joystick_slot]->m_private;
    delete m_instance[bge_joystick_slot];
  }
  m_instance[bge_joystick_slot] = nullptr;
#endif /* WITH_SDL */
}

/* Thresholds are per sensor, passed in rather than stored on the shared device. Values are ints,
 * so negating -32768 cannot overflow. */
bool DEV_Joystick::aAxisPairIsPositive(int axis, int threshold) const
{
  /* Either axis of the pair beyond the threshold: a square deadzone. */
  const int x = std::abs(GetAxisValue(axis * 2));
  const int y = std::abs(GetAxisValue(axis * 2 + 1));
  return std::max(x, y) > threshold;
}

bool DEV_Joystick::aAxisPairDirectionIsPositive(int axis, int dir, int threshold) const
{
  const bool vertical = (dir == JOYAXIS_UP || dir == JOYAXIS_DOWN);
  const int value = GetAxisValue(axis * 2 + (vertical ? 1 : 0));
  return (dir == JOYAXIS_DOWN || dir == JOYAXIS_RIGHT) ? (value > threshold) :
                                                         (value < -threshold);
}

bool DEV_Joystick::aAxisIsPositive(int axis_single, int threshold) const
{
  return std::abs(GetAxisValue(axis_single)) > threshold;
}

/* Button queries read the event-maintained mask: one load and a bit test instead of
 * SDL_GetGamepadButton(), which takes the joystick lock and scans every binding per call. */
bool DEV_Joystick::aAnyButtonPressIsPositive(void)
{
  return (m_buttons | m_pressed) != 0;
}

bool DEV_Joystick::aButtonPressIsPositive(int button)
{
  if ((unsigned int)button >= JOYBUT_MAX) {
    return false;
  }
  return ((m_buttons | m_pressed) >> button) & 1u;
}

bool DEV_Joystick::aButtonReleaseIsPositive(int button)
{
  if ((unsigned int)button >= JOYBUT_MAX) {
    return false;
  }
  return !((m_buttons >> button) & 1u);
}

bool DEV_Joystick::CreateJoystickDevice(void)
{
  bool joy_error = false;

#ifndef WITH_SDL
  m_isinit = true;
  joy_error = true;
#else  /* WITH_SDL */
  if (!m_isinit) {
    if (!joy_error && !SDL_IsGamepad(m_private->m_sdl_joystick_id)) {
      /* Not a gamepad (flight stick, throttle, wheel, pedals, force-feedback joystick...): open it
       * as a raw joystick whose axes, buttons and hats are addressed by index. */
      return CreateRawJoystickDevice();
    }

    if (!joy_error) {
      m_private->m_gamecontroller = SDL_OpenGamepad(m_private->m_sdl_joystick_id);
      if (!m_private->m_gamecontroller) {
        joy_error = true;
      }
    }

    SDL_Joystick *joy;
    if (!joy_error) {
      joy = SDL_GetGamepadJoystick(m_private->m_gamecontroller);
      if (!joy) {
        joy_error = true;
      }
    }

    if (!joy_error) {
      m_private->m_sdl_joystick_id = SDL_GetJoystickID(joy);
      if (m_private->m_sdl_joystick_id == 0) { /* 0 is the invalid id */
        joy_error = true;
        CM_Error("joystick instanced failed: " << SDL_GetError());
      }
    }

    if (!joy_error) {
      CM_Debug("Game Controller (" << GetName() << ") with index " << m_bge_joystick_slot
                                   << " initialized");

      /* A Game Controller has:
       *
       * 6 axis availables (indices 0 to 5):
       *   SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY,
       *   SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
       *   SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER.
       *   Sticks span -32768 to 32767, with +Y pointing down; triggers span 0 to 32767.
       *
       * 26 buttons, indices 0 to 25 (bit i of m_buttons is button i, so they fit in 32 bits):
       *   0-3   SDL_GAMEPAD_BUTTON_SOUTH, EAST, WEST, NORTH (A, B, X, Y on Xbox),
       *   4-6   BACK, GUIDE, START,
       *   7-10  LEFT_STICK, RIGHT_STICK, LEFT_SHOULDER, RIGHT_SHOULDER,
       *   11-14 DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
       *   15    MISC1 (Xbox Series share, PS5 microphone, Switch Pro capture),
       *   16-19 RIGHT_PADDLE1, LEFT_PADDLE1, RIGHT_PADDLE2, LEFT_PADDLE2,
       *   20    TOUCHPAD (PS4 and PS5 touchpad click),
       *   21-25 MISC2 to MISC6.
       * Buttons a controller does not have are never pressed.
       */
      m_axismax = SDL_GAMEPAD_AXIS_COUNT;
      m_buttonmax = SDL_GAMEPAD_BUTTON_COUNT;

      /* Seed once from the live state (buttons held while plugging in); from here on only
       * SDL events modify the mask. */
      m_buttons = m_pressed = 0;
      for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
        if (SDL_GetGamepadButton(m_private->m_gamecontroller, (SDL_GamepadButton)i)) {
          m_buttons |= 1u << i;
        }
      }
    }

    /* Gamepad identity and sensors, resolved once; then what every device shares. */
    if (!joy_error) {
      m_private->m_joystick = joy;
      m_private->m_type = SDL_GetGamepadType(m_private->m_gamecontroller);
      m_hasGyro = SDL_GamepadHasSensor(m_private->m_gamecontroller, SDL_SENSOR_GYRO);
      m_hasAccel = SDL_GamepadHasSensor(m_private->m_gamecontroller, SDL_SENSOR_ACCEL);
      InitOutputs();
    }
  }
#endif /* WITH_SDL */

  if (joy_error) {
    m_axismax = m_buttonmax = 0;
    return false;
  }
  else {
    m_isinit = true;
    return true;
  }
}

#ifdef WITH_SDL
bool DEV_Joystick::CreateRawJoystickDevice()
{
  SDL_Joystick *joy = SDL_OpenJoystick(m_private->m_sdl_joystick_id);
  if (!joy) {
    CM_Error("joystick with index " << m_bge_joystick_slot
                                    << " could not be opened: " << SDL_GetError());
    m_axismax = m_buttonmax = 0;
    return false;
  }
  m_private->m_joystick = joy;
  m_axismax = std::clamp(SDL_GetNumJoystickAxes(joy), 0, DEV_RAW_AXES_MAX);
  m_buttonmax = std::clamp(SDL_GetNumJoystickButtons(joy), 0, DEV_RAW_BUTTONS_MAX);
  m_numHats = std::clamp(SDL_GetNumJoystickHats(joy), 0, DEV_RAW_HATS_MAX);

  /* Seed once from the live state; from here on only SDL events modify it. */
  for (int i = 0; i < m_axismax; i++) {
    m_rawAxes[i] = SDL_GetJoystickAxis(joy, i);
    if (i < JOYAXIS_MAX) {
      m_axis_array[i] = m_rawAxes[i];
    }
  }
  m_buttons = m_pressed = 0;
  for (int i = 0; i < m_buttonmax; i++) {
    if (SDL_GetJoystickButton(joy, i)) {
      m_rawButtons[i >> 6] |= uint64_t(1) << (i & 63);
      if (i < JOYBUT_MAX) {
        m_buttons |= 1u << i;
      }
    }
  }
  for (int i = 0; i < m_numHats; i++) {
    m_hats[i] = SDL_GetJoystickHat(joy, i);
  }

  m_isinit = true;
  m_numRawJoysticks++;
  m_rawJoystickIds[m_bge_joystick_slot] = m_private->m_sdl_joystick_id;
  CM_Message("Joystick (" << GetName() << ") with index " << m_bge_joystick_slot
                          << " is not a gamepad: opened as a raw joystick (axes " << m_axismax
                          << ", buttons " << m_buttonmax << ", hats " << m_numHats
                          << "). A mapping for GUID " << GetGUID()
                          << " in gamecontrollerdb.txt makes it a gamepad.");
  InitOutputs();
  return true;
}

void DEV_Joystick::InitOutputs()
{
  SDL_Joystick *joy = m_private->m_joystick;

  /* Resolved once: a property query is a lock plus a string-keyed lookup, too costly to repeat
   * per pad per logic tick (it was, via the rumble bookkeeping loop). */
  const SDL_PropertiesID properties = SDL_GetJoystickProperties(joy);
  m_private->m_hasRumble = SDL_GetBooleanProperty(
      properties, SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
  m_private->m_hasTriggerRumble = SDL_GetBooleanProperty(
      properties, SDL_PROP_JOYSTICK_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
  m_private->m_hasLED =
      SDL_GetBooleanProperty(properties, SDL_PROP_JOYSTICK_CAP_RGB_LED_BOOLEAN, false) ||
      SDL_GetBooleanProperty(properties, SDL_PROP_JOYSTICK_CAP_MONO_LED_BOOLEAN, false);

  /* SDL_Haptic is only the fallback for devices without simple rumble. Use
   * SDL_OpenHapticFromJoystick to correctly associate haptic with the joystick. */
  if (!m_private->m_hasRumble) {
    m_private->m_haptic = SDL_OpenHapticFromJoystick(joy);
  }
  /* Most flight sticks, throttles and pedals have no motors: only a gamepad without them is
   * worth a warning. */
  if (!m_private->m_haptic && !m_private->m_hasRumble && m_private->m_gamecontroller) {
    CM_Warning("Game Controller (" << GetName() << ") with index " << m_bge_joystick_slot
                                   << " has not force feedback (vibration) available");
  }

  /* The player index lights the player LED (PS5, Switch) or ring quadrant (Xbox 360) that
   * matches this slot: slot 0 is player 1. */
  SDL_SetJoystickPlayerIndex(joy, m_bge_joystick_slot);
}
#endif /* WITH_SDL */

void DEV_Joystick::DestroyJoystickDevice(void)
{
#ifdef WITH_SDL
  if (m_isinit) {

    if (m_private->m_haptic) {
      SDL_CloseHaptic(m_private->m_haptic);
      m_private->m_haptic = nullptr;
    }

    if (m_private->m_gamecontroller) {
      CM_Debug("Game Controller (" << GetName() << ") with index " << m_bge_joystick_slot
                                    << " closed");
      SDL_CloseGamepad(m_private->m_gamecontroller);
      m_private->m_gamecontroller = nullptr;
    }
    else if (m_private->m_joystick) {
      CM_Debug("Joystick (" << GetName() << ") with index " << m_bge_joystick_slot << " closed");
      SDL_CloseJoystick(m_private->m_joystick);
      m_numRawJoysticks--;
      m_rawJoystickIds[m_bge_joystick_slot] = 0;
    }
    m_private->m_joystick = nullptr;

    m_isinit = false;
  }
#endif /* WITH_SDL */
}

bool DEV_Joystick::IsGamepad() const
{
#ifdef WITH_SDL
  return m_private->m_gamecontroller != nullptr;
#else
  return false;
#endif
}

int DEV_Joystick::GetNumAxes() const
{
  return (m_axismax > 0) ? m_axismax : 0;
}

int DEV_Joystick::GetNumButtons() const
{
  return (m_buttonmax > 0) ? m_buttonmax : 0;
}

int DEV_Joystick::GetNumHats() const
{
  return m_numHats;
}

int DEV_Joystick::GetAxisValue(int axis) const
{
  if ((unsigned int)axis >= (unsigned int)GetNumAxes()) {
    return 0;
  }
  return IsGamepad() ? m_axis_array[axis] : m_rawAxes[axis];
}

bool DEV_Joystick::IsButtonDown(int button) const
{
  if ((unsigned int)button >= (unsigned int)GetNumButtons()) {
    return false;
  }
  if (IsGamepad()) {
    return ((m_buttons | m_pressed) >> button) & 1u;
  }
  return ((m_rawButtons[button >> 6] | m_rawPressed[button >> 6]) >> (button & 63)) & 1u;
}

int DEV_Joystick::GetHat(int hat) const
{
  return ((unsigned int)hat < (unsigned int)m_numHats) ? m_hats[hat] : 0;
}

int DEV_Joystick::Connected(void)
{
#ifdef WITH_SDL
  if (m_isinit && m_private->m_joystick && SDL_JoystickConnected(m_private->m_joystick)) {
    return 1;
  }
#endif
  return 0;
}

const std::string DEV_Joystick::GetName()
{
#ifdef WITH_SDL
  /* The gamepad name comes from its mapping. Either may be null, which std::string rejects. */
  const char *name = m_private->m_gamecontroller ?
                         SDL_GetGamepadName(m_private->m_gamecontroller) :
                         (m_private->m_joystick ? SDL_GetJoystickName(m_private->m_joystick) :
                                                  nullptr);
  return name ? name : "";
#else  /* WITH_SDL */
  return "";
#endif /* WITH_SDL */
}

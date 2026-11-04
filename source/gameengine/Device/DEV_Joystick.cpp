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
      m_prec(3200),
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

void DEV_Joystick::Init()
{
#ifdef WITH_SDL

  /* To have xbox gamepads vibrations working on windows in recent SDL versions. To be tested with other gamepads/OS */
  SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");

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

void DEV_Joystick::cSetPrecision(int val)
{
  m_prec = val;
}

bool DEV_Joystick::aAxisPairIsPositive(int axis)
{
  return (pAxisTest(axis) > m_prec) ? true : false;
}

bool DEV_Joystick::aAxisPairDirectionIsPositive(int axis, int dir)
{

  int res;

  if (dir == JOYAXIS_UP || dir == JOYAXIS_DOWN)
    res = pGetAxis(axis, 1);
  else /* JOYAXIS_LEFT || JOYAXIS_RIGHT */
    res = pGetAxis(axis, 0);

  if (dir == JOYAXIS_DOWN || dir == JOYAXIS_RIGHT)
    return (res > m_prec) ? true : false;
  else /* JOYAXIS_UP || JOYAXIS_LEFT */
    return (res < -m_prec) ? true : false;
}

bool DEV_Joystick::aAxisIsPositive(int axis_single)
{
  return std::abs(m_axis_array[axis_single]) > m_prec ? true : false;
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
      /* mapping instruccions if joystick is not a game controller */
      CM_Error(
          "Game Controller index "
          << m_bge_joystick_slot << ": Could not be initialized\n"
          << "Please, generate Xbox360 compatible mapping using Antimicro "
             "(https://github.com/AntiMicro/antimicro)\n"
          << "or SDL2 Gamepad Tool (http://www.generalarcade.com/gamepadtool) or Steam big mode "
             "applications\n"
          << "and after, set the SDL controller variable before you launch the executable, i.e:\n"
          << "export SDL_GAMECONTROLLERCONFIG=\"[the string you received from controllermap]\"");
      /* Need this so python args can return empty lists */
      joy_error = true;
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
      if (m_private->m_sdl_joystick_id < 0) {
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

    /* Haptic configuration */
    if (!joy_error) {
      bool has_gamepad_rumble = false;
      const SDL_PropertiesID gamepad_properties = SDL_GetGamepadProperties(
          m_private->m_gamecontroller);
      if (gamepad_properties) {
        has_gamepad_rumble = SDL_GetBooleanProperty(
            gamepad_properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
      }

      /* Use SDL_OpenHapticFromJoystick to correctly associate haptic with the
       * game controller's underlying joystick. SDL_HapticOpen(index) takes a
       * haptic device index which does not match the joystick/controller index
       * and can fail silently with newer SDL versions. */
      m_private->m_haptic = SDL_OpenHapticFromJoystick(joy);
      if (!m_private->m_haptic && !has_gamepad_rumble) {
        CM_Warning("Game Controller (" << GetName() << ") with index " << m_bge_joystick_slot
                                       << " has not force feedback (vibration) available");
      }
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

    m_isinit = false;
  }
#endif /* WITH_SDL */
}

int DEV_Joystick::Connected(void)
{
#ifdef WITH_SDL
  if (m_isinit && SDL_GamepadConnected(m_private->m_gamecontroller)) {
    return 1;
  }
#endif
  return 0;
}

int DEV_Joystick::pGetAxis(int axisnum, int udlr)
{
#ifdef WITH_SDL
  return m_axis_array[(axisnum * 2) + udlr];
#endif
  return 0;
}

int DEV_Joystick::pAxisTest(int axisnum)
{
#ifdef WITH_SDL
  /* Use ints instead of shorts here to avoid problems when we get -32768.
   * When we take the negative of that later, we should get 32768, which is greater
   * than what a short can hold. In other words, abs(MIN_SHORT) > MAX_SHRT. */
  int i1 = m_axis_array[(axisnum * 2)];
  int i2 = m_axis_array[(axisnum * 2) + 1];

  /* long winded way to do:
   * return max_ff(absf(i1), absf(i2))
   * ...avoid abs from math.h */
  if (i1 < 0)
    i1 = -i1;
  if (i2 < 0)
    i2 = -i2;
  if (i1 < i2)
    return i2;
  else
    return i1;
#else  /* WITH_SDL */
  return 0;
#endif /* WITH_SDL */
}

const std::string DEV_Joystick::GetName()
{
#ifdef WITH_SDL
  return SDL_GetGamepadName(m_private->m_gamecontroller);
#else  /* WITH_SDL */
  return "";
#endif /* WITH_SDL */
}

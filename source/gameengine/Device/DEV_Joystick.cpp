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
#  include <SDL.h>
#endif

#include "BKE_appdir.hh"
#include "BLI_path_utils.hh"

#include "CM_Message.h"
#include "DEV_JoystickPrivate.h"

DEV_Joystick::DEV_Joystick(short index)
    : m_joyindex(index),
      m_prec(3200),
      m_axismax(-1),
      m_buttonmax(-1),
      m_isinit(0),
      m_istrig_axis(0),
      m_istrig_button(0)
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
  bool success = (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != -1);

  if (success) {
    /* Game controller data base loading */
    const std::optional<std::string> path = BKE_appdir_folder_id(BLENDER_DATAFILES,
                                                                 "gamecontroller");

    if (path.has_value()) {
      char fullpath[FILE_MAX];
      BLI_path_join(fullpath, sizeof(fullpath), path->c_str(), "gamecontrollerdb.txt");

      if ((SDL_GameControllerAddMappingsFromFile(fullpath)) == -1) {
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
    m_instance[i]->ReleaseInstance(i);
  }

  /* Closing SDL Game controller system */
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);
#endif
}

DEV_Joystick *DEV_Joystick::GetInstance(short joyindex)
{
#ifndef WITH_SDL
  return nullptr;
#else  /* WITH_SDL */

  if (joyindex < 0 || joyindex >= JOYINDEX_MAX) {
    CM_Error("invalid joystick index: " << joyindex);
    return nullptr;
  }

  return m_instance[joyindex];
#endif /* WITH_SDL */
}

void DEV_Joystick::ReleaseInstance(short joyindex)
{
#ifdef WITH_SDL
  if (m_instance[joyindex]) {
    m_instance[joyindex]->DestroyJoystickDevice();
    delete m_private;
    delete m_instance[joyindex];
  }
  m_instance[joyindex] = nullptr;
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

bool DEV_Joystick::aAnyButtonPressIsPositive(void)
{
#ifdef WITH_SDL
  /* this is needed for the "all events" option
   * so we know if there are no buttons pressed */
  for (int i = 0; i < m_buttonmax; i++) {
    if (SDL_GameControllerGetButton(m_private->m_gamecontroller, (SDL_GameControllerButton)i)) {
      return true;
    }
  }
#endif
  return false;
}

bool DEV_Joystick::aButtonPressIsPositive(int button)
{
#ifdef WITH_SDL
  if (SDL_GameControllerGetButton(m_private->m_gamecontroller, (SDL_GameControllerButton)button)) {
    return true;
  }
#endif
  return false;
}

bool DEV_Joystick::aButtonReleaseIsPositive(int button)
{
#ifdef WITH_SDL
  if (!(SDL_GameControllerGetButton(m_private->m_gamecontroller,
                                    (SDL_GameControllerButton)button))) {
    return true;
  }
#endif
  return false;
}

bool DEV_Joystick::CreateJoystickDevice(void)
{
  bool joy_error = false;

#ifndef WITH_SDL
  m_isinit = true;
  joy_error = true;
#else  /* WITH_SDL */
  if (!m_isinit) {
    if (!joy_error && !SDL_IsGameController(m_joyindex)) {
      /* mapping instruccions if joystick is not a game controller */
      CM_Error(
          "Game Controller index "
          << m_joyindex << ": Could not be initialized\n"
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
      m_private->m_gamecontroller = SDL_GameControllerOpen(m_joyindex);
      if (!m_private->m_gamecontroller) {
        joy_error = true;
      }
    }

    SDL_Joystick *joy;
    if (!joy_error) {
      joy = SDL_GameControllerGetJoystick(m_private->m_gamecontroller);
      if (!joy) {
        joy_error = true;
      }
    }

    if (!joy_error) {
      m_private->m_instance_id = SDL_JoystickInstanceID(joy);
      if (m_private->m_instance_id < 0) {
        joy_error = true;
        CM_Error("joystick instanced failed: " << SDL_GetError());
      }
    }

    if (!joy_error) {
      CM_Debug("Game Controller (" << GetName() << ") with index " << m_joyindex
                                   << " initialized");

      /* A Game Controller has:
       *
       * 6 axis availables:	   AXIS_LEFTSTICK_X, AXIS_LEFTSTICK_Y,
       * (in order from 0 to 5)  AXIS_RIGHTSTICK_X, AXIS_RIGHTSTICK_Y,
       *						   AXIS_TRIGGERLEFT and AXIS_TRIGGERRIGHT.
       *
       * 15 buttons availables:  BUTTON_A, BUTTON_B, BUTTON_X, BUTTON_Y,
       * (in order from 0 to 14) BUTTON_BACK, BUTTON_GUIDE, BUTTON_START,
       *						   BUTTON_LEFTSTICK, BUTTON_RIGHTSTICK,
       *						   BUTTON_LEFTSHOULDER, BUTTON_RIGHTSHOULDER,
       *						   BUTTON_DPAD_UP, BUTTON_DPAD_DOWN,
       *						   BUTTON_DPAD_LEFT and BUTTON_DPAD_RIGHT.
       */
      m_axismax = SDL_CONTROLLER_AXIS_MAX;
      m_buttonmax = SDL_CONTROLLER_BUTTON_MAX;
    }

    /* Haptic configuration */
    if (!joy_error) {
      m_private->m_haptic = SDL_HapticOpen(m_joyindex);
      if (!m_private->m_haptic) {
        CM_Warning("Game Controller (" << GetName() << ") with index " << m_joyindex
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
      SDL_HapticClose(m_private->m_haptic);
      m_private->m_haptic = nullptr;
    }

    if (m_private->m_gamecontroller) {
      CM_Debug("Game Controller (" << GetName() << ") with index " << m_joyindex << " closed");
      SDL_GameControllerClose(m_private->m_gamecontroller);
      m_private->m_gamecontroller = nullptr;
    }

    m_isinit = false;
  }
#endif /* WITH_SDL */
}

int DEV_Joystick::Connected(void)
{
#ifdef WITH_SDL
  if (m_isinit && SDL_GameControllerGetAttached(m_private->m_gamecontroller)) {
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
  return SDL_GameControllerName(m_private->m_gamecontroller);
#else  /* WITH_SDL */
  return "";
#endif /* WITH_SDL */
}

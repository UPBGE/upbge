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
 * Contributor(s): lordloki.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Device/DEV_JoystickFeatures.cpp
 *  \ingroup device
 *
 * Optional controller features exposed by SDL 3: identity, player index. Nothing here runs per
 * logic tick; every function is called only when a script asks.
 */

#include "DEV_Joystick.h"
#include "DEV_JoystickPrivate.h"

const char *DEV_Joystick::GetTypeName() const
{
#ifdef WITH_SDL
  if (m_private->m_gamecontroller) {
    /* SDL's own stable identifiers: "xbox360", "xboxone", "ps4", "ps5", "switchpro", ... */
    const char *name = SDL_GetGamepadStringForType(m_private->m_type);
    return name ? name : "unknown";
  }
#endif
  return "unknown";
}

const char *DEV_Joystick::GetButtonLabel(int button) const
{
#ifdef WITH_SDL
  if (m_private->m_gamecontroller) {
    switch (SDL_GetGamepadButtonLabel(m_private->m_gamecontroller, (SDL_GamepadButton)button)) {
      case SDL_GAMEPAD_BUTTON_LABEL_A:
        return "A";
      case SDL_GAMEPAD_BUTTON_LABEL_B:
        return "B";
      case SDL_GAMEPAD_BUTTON_LABEL_X:
        return "X";
      case SDL_GAMEPAD_BUTTON_LABEL_Y:
        return "Y";
      case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
        return "CROSS";
      case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
        return "CIRCLE";
      case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
        return "SQUARE";
      case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
        return "TRIANGLE";
      default:
        break;
    }
  }
#endif
  return "UNKNOWN";
}

std::string DEV_Joystick::GetGUID() const
{
#ifdef WITH_SDL
  if (m_private->m_gamecontroller) {
    char guid[33]; /* 32 hex digits + terminator. */
    SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(m_private->m_gamecontroller)),
                     guid,
                     sizeof(guid));
    return guid;
  }
#endif
  return "";
}

const char *DEV_Joystick::GetConnectionName() const
{
#ifdef WITH_SDL
  if (m_private->m_gamecontroller) {
    switch (SDL_GetGamepadConnectionState(m_private->m_gamecontroller)) {
      case SDL_JOYSTICK_CONNECTION_WIRED:
        return "wired";
      case SDL_JOYSTICK_CONNECTION_WIRELESS:
        return "wireless";
      default:
        break;
    }
  }
#endif
  return "unknown";
}

int DEV_Joystick::GetPlayerIndex() const
{
#ifdef WITH_SDL
  if (m_private->m_gamecontroller) {
    return SDL_GetGamepadPlayerIndex(m_private->m_gamecontroller);
  }
#endif
  return -1;
}

bool DEV_Joystick::SetPlayerIndex(int index)
{
#ifdef WITH_SDL
  if (m_private->m_gamecontroller) {
    return SDL_SetGamepadPlayerIndex(m_private->m_gamecontroller, index);
  }
#endif
  return false;
}

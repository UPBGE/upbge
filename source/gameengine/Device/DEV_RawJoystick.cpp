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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Device/DEV_RawJoystick.cpp
 *  \ingroup device
 */

#include "DEV_RawJoystick.h"

#include <algorithm>
#include <climits>

#include "CM_Message.h"

DEV_RawJoystick *DEV_RawJoystick::m_instance[RAWJOYINDEX_MAX] = {nullptr};

#ifdef WITH_SDL
DEV_RawJoystick::DEV_RawJoystick(short slot, SDL_JoystickID sdl_joystick_id)
    : m_slot(slot), m_sdl_joystick_id(sdl_joystick_id)
{
  m_joystick = SDL_OpenJoystick(sdl_joystick_id);
}
#endif

DEV_RawJoystick::~DEV_RawJoystick()
{
#ifdef WITH_SDL
  if (m_joystick) {
    CM_Debug("Input device (" << GetName() << ") with index " << m_slot << " closed");
    SDL_CloseJoystick(m_joystick);
  }
#endif
}

DEV_RawJoystick *DEV_RawJoystick::GetInstance(short slot)
{
  if (slot < 0 || slot >= RAWJOYINDEX_MAX) {
    return nullptr;
  }
  return m_instance[slot];
}

#ifdef WITH_SDL
bool DEV_RawJoystick::HandleEvent(const SDL_Event *sdl_event, short (&addrem)[RAWJOYINDEX_MAX])
{
  switch (sdl_event->type) {
    case SDL_EVENT_JOYSTICK_ADDED: {
      const SDL_JoystickID sdl_joystick_id = sdl_event->jdevice.which;
      int free_slot = -1;
      for (int i = 0; i < RAWJOYINDEX_MAX; i++) {
        if (m_instance[i] && m_instance[i]->m_sdl_joystick_id == sdl_joystick_id) {
          /* Already opened. */
          return false;
        }
        if (!m_instance[i] && free_slot == -1) {
          free_slot = i;
        }
      }

      if (free_slot == -1) {
        CM_Warning("maximum quantity (" << RAWJOYINDEX_MAX
                                        << ") of input devices connected. It is not possible to "
                                           "set up additional ones.");
        return false;
      }

      DEV_RawJoystick *device = new DEV_RawJoystick(free_slot, sdl_joystick_id);
      if (!device->m_joystick) {
        CM_Error("input device could not be opened: " << SDL_GetError());
        delete device;
        return false;
      }

      m_instance[free_slot] = device;
      addrem[free_slot] = 1;
      CM_Debug("Input device (" << device->GetName() << ") with index " << free_slot
                                << " initialized");
      return true;
    }
    case SDL_EVENT_JOYSTICK_REMOVED: {
      for (int i = 0; i < RAWJOYINDEX_MAX; i++) {
        if (m_instance[i] && m_instance[i]->m_sdl_joystick_id == sdl_event->jdevice.which) {
          delete m_instance[i];
          m_instance[i] = nullptr;
          addrem[i] = 2;
          return true;
        }
      }
      return false;
    }
    default:
      return false;
  }
}
#endif /* WITH_SDL */

void DEV_RawJoystick::Close()
{
  for (int i = 0; i < RAWJOYINDEX_MAX; i++) {
    delete m_instance[i];
    m_instance[i] = nullptr;
  }
}

short DEV_RawJoystick::GetSlot() const
{
  return m_slot;
}

std::string DEV_RawJoystick::GetName() const
{
#ifdef WITH_SDL
  const char *name = SDL_GetJoystickName(m_joystick);
  if (name) {
    return name;
  }
#endif
  return "";
}

std::string DEV_RawJoystick::GetGUID() const
{
#ifdef WITH_SDL
  char guid[33];
  SDL_GUIDToString(SDL_GetJoystickGUID(m_joystick), guid, sizeof(guid));
  return guid;
#else
  return "";
#endif
}

int DEV_RawJoystick::GetVendorId() const
{
#ifdef WITH_SDL
  return SDL_GetJoystickVendor(m_joystick);
#else
  return 0;
#endif
}

int DEV_RawJoystick::GetProductId() const
{
#ifdef WITH_SDL
  return SDL_GetJoystickProduct(m_joystick);
#else
  return 0;
#endif
}

DEV_RawJoystick::Type DEV_RawJoystick::GetType() const
{
#ifdef WITH_SDL
  switch (SDL_GetJoystickType(m_joystick)) {
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      return Type::Gamepad;
    case SDL_JOYSTICK_TYPE_WHEEL:
      return Type::Wheel;
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return Type::ArcadeStick;
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return Type::FlightStick;
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return Type::DancePad;
    case SDL_JOYSTICK_TYPE_GUITAR:
      return Type::Guitar;
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return Type::DrumKit;
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return Type::ArcadePad;
    case SDL_JOYSTICK_TYPE_THROTTLE:
      return Type::Throttle;
    default:
      break;
  }
#endif
  return Type::Unknown;
}

bool DEV_RawJoystick::IsGamepad() const
{
#ifdef WITH_SDL
  return SDL_IsGamepad(m_sdl_joystick_id);
#else
  return false;
#endif
}

bool DEV_RawJoystick::IsConnected() const
{
#ifdef WITH_SDL
  return SDL_JoystickConnected(m_joystick);
#else
  return false;
#endif
}

int DEV_RawJoystick::GetNumAxes() const
{
#ifdef WITH_SDL
  /* SDL returns -1 on failure. */
  return std::max(SDL_GetNumJoystickAxes(m_joystick), 0);
#else
  return 0;
#endif
}

int DEV_RawJoystick::GetNumButtons() const
{
#ifdef WITH_SDL
  return std::max(SDL_GetNumJoystickButtons(m_joystick), 0);
#else
  return 0;
#endif
}

int DEV_RawJoystick::GetNumHats() const
{
#ifdef WITH_SDL
  return std::max(SDL_GetNumJoystickHats(m_joystick), 0);
#else
  return 0;
#endif
}

float DEV_RawJoystick::GetAxis(int index) const
{
#ifdef WITH_SDL
  if (index < 0 || index >= GetNumAxes()) {
    return 0.0f;
  }

  /* SDL returns a range from -32768 to 32767, divide each side by its own bound to get an exact
   * -1.0 to 1.0 mapping, the same way as SCA_PythonJoystick.axisValues does. */
  const int position = SDL_GetJoystickAxis(m_joystick, index);
  if (position < 0) {
    return position / float(-SHRT_MIN);
  }
  return position / float(SHRT_MAX);
#else
  return 0.0f;
#endif
}

bool DEV_RawJoystick::GetButton(int index) const
{
#ifdef WITH_SDL
  if (index < 0 || index >= GetNumButtons()) {
    return false;
  }
  return SDL_GetJoystickButton(m_joystick, index);
#else
  return false;
#endif
}

int DEV_RawJoystick::GetHat(int index) const
{
#ifdef WITH_SDL
  if (index < 0 || index >= GetNumHats()) {
    return 0;
  }
  return SDL_GetJoystickHat(m_joystick, index);
#else
  return 0;
#endif
}

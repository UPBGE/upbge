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
 * Contributor(s): snailrose.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Device/DEV_JoystickEvents.cpp
 *  \ingroup device
 */

#include "CM_Message.h"
#include "DEV_Joystick.h"
#include "DEV_JoystickPrivate.h"

#include <cstdlib>

#ifdef WITH_SDL
void DEV_Joystick::OnAxisEvent(SDL_Event *sdl_event)
{
  if (sdl_event->caxis.axis >= JOYAXIS_MAX)
    return;

  const int axis = sdl_event->caxis.axis;
  const int value = sdl_event->caxis.value;
  /* Logic Nodes uses a 0.1 activation threshold. Retain both threshold crossings so a quick
   * trigger/stick pulse remains observable even when SDL supplies press and release before the
   * next fixed-step sample. */
  constexpr int activation_threshold = 3277;
  const bool active = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                              axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ?
                          value >= activation_threshold :
                          std::abs(value) >= activation_threshold;
  if (active != (m_axisActive[axis] != 0)) {
    if (active) {
      ++m_axisPressRevision[axis];
    }
    else {
      ++m_axisReleaseRevision[axis];
    }
    m_axisActive[axis] = active ? 1 : 0;
  }

  m_axis_array[axis] = value;
  m_istrig_axis = 1;
}

/* See notes below in the event loop */
void DEV_Joystick::OnButtonEvent(SDL_Event *sdl_event)
{
  const int button = sdl_event->cbutton.button;
  if (button < 0 || button >= JOYBUT_MAX) {
    return;
  }
  const bool active = sdl_event->type == SDL_CONTROLLERBUTTONDOWN;
  if (active != (m_buttonActive[button] != 0)) {
    if (active) {
      ++m_buttonPressRevision[button];
    }
    else {
      ++m_buttonReleaseRevision[button];
    }
    m_buttonActive[button] = active ? 1 : 0;
  }
  m_istrig_button = 1;
}

void DEV_Joystick::OnNothing(SDL_Event *sdl_event)
{
  m_istrig_axis = m_istrig_button = 0;
}

bool DEV_Joystick::HandleEvents(short (&addrem)[JOYINDEX_MAX])
{
  SDL_Event sdl_event;
  bool remap = false;

  for (int i = 0; i < JOYINDEX_MAX; i++) {
    if (DEV_Joystick::m_instance[i])
      DEV_Joystick::m_instance[i]->OnNothing(&sdl_event);
  }

  while (SDL_PollEvent(&sdl_event)) {
    /* Note! m_instance[instance]
     * will segfault if over JOYINDEX_MAX, not too nice but what are the chances? */

    /* Current state remains available through SDL_ControllerGetButton. Per-control transition
     * revisions additionally preserve press/release pairs received within one logic tick. */

    /* Note!, we need to use SDL_JOYDEVICE ADDED to find new controllers as
     * SDL_CONTROLLERDEVICEADDED doesn't report about all devices connected at beginning.
     * Additionally we capture all devices this
     * way and we can inform properly (with ways to solve it) if the joystick it is not a game
     * controller */

    switch (sdl_event.type) {
      case SDL_JOYDEVICEADDED: {
        /* A pad takes the first free slot (its gamepad number) and is opened by its SDL device
         * index. SDL numbers devices from 0 again after one is removed, so a pad plugged back in
         * can get a device index whose slot is still used by another pad. */
        const SDL_JoystickID added_id = SDL_JoystickGetDeviceInstanceID(sdl_event.jdevice.which);
        int slot = -1;
        bool already_open = false;
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          DEV_Joystick *joystick = DEV_Joystick::m_instance[i];
          if (joystick == nullptr) {
            if (slot < 0) {
              slot = i;
            }
          }
          else if (added_id >= 0 && joystick->m_private->m_instance_id == added_id) {
            already_open = true;
          }
        }
        if (already_open) {
          break;
        }
        if (slot < 0) {
          CM_Warning(
              "maximum quantity (8) of Game Controllers connected. It is not possible to set up "
              "additional ones.");
          break;
        }
        DEV_Joystick::m_instance[slot] = new DEV_Joystick(sdl_event.jdevice.which);
        if (!DEV_Joystick::m_instance[slot]->CreateJoystickDevice()) {
          /* Not a game controller (the error says how to map it): it keeps no gamepad number,
           * and no removal event would ever free one. */
          DEV_Joystick::ReleaseInstance(slot);
          break;
        }
        addrem[slot] = 1;
        remap = true;
        break;
      }
      case SDL_CONTROLLERDEVICEREMOVED:
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          if (DEV_Joystick::m_instance[i]) {
            if (sdl_event.cdevice.which == DEV_Joystick::m_instance[i]->m_private->m_instance_id) {
              DEV_Joystick::ReleaseInstance(i);
              addrem[i] = 2;
              remap = true;
              break;
            }
          }
        }
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP:
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          if (DEV_Joystick::m_instance[i]) {
            if (sdl_event.cdevice.which == DEV_Joystick::m_instance[i]->m_private->m_instance_id) {
              DEV_Joystick::m_instance[i]->OnButtonEvent(&sdl_event);
              break;
            }
          }
        }
        break;
      case SDL_CONTROLLERAXISMOTION:
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          if (DEV_Joystick::m_instance[i]) {
            if (sdl_event.cdevice.which == DEV_Joystick::m_instance[i]->m_private->m_instance_id) {
              DEV_Joystick::m_instance[i]->OnAxisEvent(&sdl_event);
              break;
            }
          }
        }
        break;
      default:
        /* ignore old SDL_JOYSTICKS events */
        break;
    }
  }
  return remap;
}
#endif /* WITH_SDL */

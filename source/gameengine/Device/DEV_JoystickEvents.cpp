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

#ifdef WITH_SDL
void DEV_Joystick::OnAxisEvent(SDL_Event *sdl_event)
{
  if (sdl_event->caxis.axis >= JOYAXIS_MAX)
    return;

  m_axis_array[sdl_event->caxis.axis] = sdl_event->caxis.value;
  m_istrig_axis = 1;
}

/* See notes below in the event loop */
void DEV_Joystick::OnButtonEvent(SDL_Event *sdl_event)
{
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

    /* Note!, with buttons, this wont care which button is pressed,
     * only to set 'm_istrig_button', actual pressed buttons are detected by
     * SDL_ControllerGetButton */

    /* Note!, if you manage to press and release a button within 1 logic tick
     * it wont work as it should */

    /* Note!, we need to use SDL_JOYDEVICE ADDED to find new controllers as
     * SDL_CONTROLLERDEVICEADDED doesn't report about all devices connected at beginning.
     * Additionally we capture all devices this
     * way and we can inform properly (with ways to solve it) if the joystick it is not a game
     * controller */

    switch (sdl_event.type) {
      case SDL_JOYDEVICEADDED:
        if (sdl_event.jdevice.which < JOYINDEX_MAX) {
          if (!DEV_Joystick::m_instance[sdl_event.jdevice.which]) {
            DEV_Joystick::m_instance[sdl_event.jdevice.which] = new DEV_Joystick(
                sdl_event.jdevice.which);
            DEV_Joystick::m_instance[sdl_event.jdevice.which]->CreateJoystickDevice();
            addrem[sdl_event.jdevice.which] = 1;
            remap = true;
            break;
          }
          else {
            CM_Warning("conflicts with Joysticks trying to use the same index."
                       << " Please, reconnect Joysticks in different order than before");
          }
        }
        else {
          CM_Warning(
              "maximum quantity (8) of Game Controllers connected. It is not possible to set up "
              "additional ones.");
        }
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          if (DEV_Joystick::m_instance[i]) {
            if (sdl_event.cdevice.which == DEV_Joystick::m_instance[i]->m_private->m_instance_id) {
              DEV_Joystick::m_instance[i]->ReleaseInstance(i);
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

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

static int find_free_bge_joystick_slot()
{
  for (int i = 0; i < JOYINDEX_MAX; i++) {
    if (!DEV_Joystick::GetInstance(i)) {
      return i;
    }
  }
  return -1;
}

void DEV_Joystick::OnAxisEvent(SDL_Event *sdl_event)
{
  if (sdl_event->gaxis.axis >= JOYAXIS_MAX)
    return;
  m_axis_array[sdl_event->gaxis.axis] = sdl_event->gaxis.value;
  m_istrig_axis = 1;
}

void DEV_Joystick::OnButtonEvent(SDL_Event *sdl_event)
{
  const uint32_t bit = 1u << sdl_event->gbutton.button;
  if (sdl_event->gbutton.down) {
    m_buttons |= bit;
    m_pressed |= bit; /* Survives a release in the same tick: sub-tick taps are not lost. */
  }
  else {
    m_buttons &= ~bit;
  }
  m_istrig_button = 1;
}

void DEV_Joystick::OnSensorEvent(SDL_Event *sdl_event)
{
  const SDL_GamepadSensorEvent &event = sdl_event->gsensor;
  if (event.sensor == SDL_SENSOR_GYRO) {
    /* Each sample's rate is held over the interval since the previous sample (rectangle rule):
     * at 200 to 1000 Hz the difference to the trapezoid rule is below the sensor noise, and no
     * previous rate needs storing. The interval comes from the hardware timestamp when there is
     * one, and is clamped to 4 nominal periods so a stall (debugger, window drag) cannot
     * integrate seconds of stale rotation into one tick. */
    float dt = m_gyroNominalDt;
    if (m_gyroLastNs != 0 && event.sensor_timestamp > m_gyroLastNs) {
      const float measured = float(event.sensor_timestamp - m_gyroLastNs) * 1e-9f;
      dt = (measured < 4.0f * m_gyroNominalDt) ? measured : 4.0f * m_gyroNominalDt;
    }
    m_gyroLastNs = event.sensor_timestamp;
    for (int i = 0; i < 3; i++) {
      m_gyro[i] = event.data[i];
      m_gyroDelta[i] += event.data[i] * dt;
    }
  }
  else if (event.sensor == SDL_SENSOR_ACCEL) {
    m_accel[0] = event.data[0];
    m_accel[1] = event.data[1];
    m_accel[2] = event.data[2];
  }
}

void DEV_Joystick::OnNothing(SDL_Event *sdl_event)
{
  /* A tap that began and ended inside the previous tick was reported as pressed for that tick
   * only. SCA_JoystickSensor re-evaluates only on ticks flagged by IsTrigButton(), so flag this
   * tick too; otherwise the sensor would keep its positive state until an unrelated event. */
  const uint32_t tapped = m_pressed & ~m_buttons;
  m_istrig_axis = 0;
  m_istrig_button = (tapped != 0);
  m_pressed = 0;
  /* gyroDelta is the rotation since the previous tick: restart the sum. */
  m_gyroDelta[0] = m_gyroDelta[1] = m_gyroDelta[2] = 0.0f;
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

    /* Note!, button events maintain m_buttons/m_pressed; sensors and Python read those
     * instead of querying SDL. */

    /* Note!, we need to use SDL_JOYDEVICE ADDED to find new controllers as
     * SDL_CONTROLLERDEVICEADDED doesn't report about all devices connected at beginning.
     * Additionally we capture all devices this
     * way and we can inform properly (with ways to solve it) if the joystick it is not a game
     * controller */

    switch (sdl_event.type) {
      case SDL_EVENT_JOYSTICK_ADDED:
        {
          const int bge_joystick_slot = find_free_bge_joystick_slot();
          if (bge_joystick_slot >= 0) {
            DEV_Joystick::m_instance[bge_joystick_slot] = new DEV_Joystick(bge_joystick_slot);
            DEV_Joystick::m_instance[bge_joystick_slot]->SetInstanceId(sdl_event.jdevice.which);
            if (DEV_Joystick::m_instance[bge_joystick_slot]->CreateJoystickDevice()) {
              addrem[bge_joystick_slot] = 1;
              remap = true;
              break;
            }

            DEV_Joystick::m_instance[bge_joystick_slot]->ReleaseInstance(bge_joystick_slot);
          }
          else {
            CM_Warning(
                "maximum quantity (8) of Game Controllers connected. It is not possible to set up "
                "additional ones.");
          }
        }
        break;
      case SDL_EVENT_GAMEPAD_REMOVED:
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          if (DEV_Joystick::m_instance[i]) {
            if (sdl_event.gdevice.which ==
                DEV_Joystick::m_instance[i]->m_private->m_sdl_joystick_id) {
              DEV_Joystick::m_instance[i]->ReleaseInstance(i);
              addrem[i] = 2;
              remap = true;
              break;
            }
          }
        }
        break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP:
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          if (DEV_Joystick::m_instance[i]) {
            if (sdl_event.gbutton.which ==
                DEV_Joystick::m_instance[i]->m_private->m_sdl_joystick_id) {
              DEV_Joystick::m_instance[i]->OnButtonEvent(&sdl_event);
              break;
            }
          }
        }
        break;
      case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          if (DEV_Joystick::m_instance[i]) {
            if (sdl_event.gaxis.which ==
                DEV_Joystick::m_instance[i]->m_private->m_sdl_joystick_id) {
              DEV_Joystick::m_instance[i]->OnAxisEvent(&sdl_event);
              break;
            }
          }
        }
        break;
      case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        for (int i = 0; i < JOYINDEX_MAX; i++) {
          if (DEV_Joystick::m_instance[i]) {
            if (sdl_event.gsensor.which ==
                DEV_Joystick::m_instance[i]->m_private->m_sdl_joystick_id) {
              DEV_Joystick::m_instance[i]->OnSensorEvent(&sdl_event);
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

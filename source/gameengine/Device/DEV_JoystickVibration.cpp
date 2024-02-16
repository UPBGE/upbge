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
 ** Contributor(s): youle, lordloki
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Device/DEV_JoystickVibration.cpp
 *  \ingroup device
 */

#include <memory>  // We have to include that on Windows to make memset available

#include "CM_Message.h"
#include "DEV_Joystick.h"
#include "DEV_JoystickPrivate.h"
#include "BLI_time.h"  // Module to get real time in Game Engine

bool DEV_Joystick::RumblePlay(float strengthLeft, float strengthRight, unsigned int duration)
{
#ifdef WITH_SDL
  unsigned int effects;
  bool run_by_effect = false;
  bool effects_issue = false;

  if (m_private->m_haptic == nullptr) {
    return false;
  }

  // Managing vibration logic
  if (m_private->m_hapticEffectStatus == JOYHAPTIC_STOPPED) {
    memset(&m_private->m_hapticeffect, 0, sizeof(SDL_HapticEffect));  // 0 is safe default
  }
  else if (m_private->m_hapticEffectStatus == JOYHAPTIC_PLAYING_EFFECT) {
    m_private->m_hapticEffectStatus = JOYHAPTIC_UPDATING_EFFECT;
  }
  else if (m_private->m_hapticEffectStatus == JOYHAPTIC_PLAYING_RUMBLE_EFFECT) {
    m_private->m_hapticEffectStatus = JOYHAPTIC_UPDATING_RUMBLE_EFFECT;
  }

  // Checking supported effects
  effects = SDL_HapticQuery(m_private->m_haptic);

  // LeftRight is the most supported effect by XInput game controllers
  if ((effects & SDL_HAPTIC_LEFTRIGHT) &&
      m_private->m_hapticEffectStatus != JOYHAPTIC_UPDATING_RUMBLE_EFFECT) {
    if (m_private->m_hapticEffectStatus != JOYHAPTIC_UPDATING_EFFECT) {
      m_private->m_hapticeffect.type = SDL_HAPTIC_LEFTRIGHT;
    }

    m_private->m_hapticeffect.leftright.length = duration;
    m_private->m_hapticeffect.leftright.large_magnitude = (unsigned int)(strengthLeft * 0x7FFF);
    m_private->m_hapticeffect.leftright.small_magnitude = (unsigned int)(strengthRight * 0x7FFF);
    run_by_effect = true;
  }
  // Some Game Controllers only supports large/small magnitude motors using a custom effect
  else if ((effects & SDL_HAPTIC_CUSTOM) &&
           m_private->m_hapticEffectStatus != JOYHAPTIC_UPDATING_RUMBLE_EFFECT) {

    Uint16 data[2];  // data = channels * samples
    data[0] = (Uint16)(strengthLeft * 0x7FFF);
    data[1] = (Uint16)(strengthRight * 0x7FFF);

    if (m_private->m_hapticEffectStatus != JOYHAPTIC_UPDATING_EFFECT) {
      m_private->m_hapticeffect.type = SDL_HAPTIC_CUSTOM;
    }
    m_private->m_hapticeffect.custom.length = duration;
    m_private->m_hapticeffect.custom.channels = 2;
    m_private->m_hapticeffect.custom.period = 1;
    m_private->m_hapticeffect.custom.samples = 1;
    m_private->m_hapticeffect.custom.data = data;

    run_by_effect = true;
  }

  if (run_by_effect) {
    bool new_effect = true;

    if (m_private->m_hapticEffectStatus == JOYHAPTIC_UPDATING_EFFECT) {
      if (SDL_HapticUpdateEffect(
              m_private->m_haptic, m_private->m_hapticEffectId, &m_private->m_hapticeffect) == 0) {
        m_private->m_hapticEffectStatus = JOYHAPTIC_PLAYING_EFFECT;
        new_effect = false;
      }
      else {
        SDL_HapticDestroyEffect(m_private->m_haptic, m_private->m_hapticEffectId);
        m_private->m_hapticEffectId = -1;
      }
    }

    if (new_effect) {
      // Upload the effect
      m_private->m_hapticEffectId = SDL_HapticNewEffect(m_private->m_haptic,
                                                        &m_private->m_hapticeffect);
    }

    // Run the effect
    if (m_private->m_hapticEffectId >= 0 &&
        SDL_HapticRunEffect(m_private->m_haptic, m_private->m_hapticEffectId, 1) != -1) {
      m_private->m_hapticEffectStatus = JOYHAPTIC_PLAYING_EFFECT;
    }
    else {
      effects_issue = true;
    }
  }

  // Initialize simplest rumble effect for both motors if more complex effects are not supported
  // Most controllers can use SINE effect, but XInput only has LEFTRIGHT.
  if (effects_issue || m_private->m_hapticEffectStatus == JOYHAPTIC_UPDATING_RUMBLE_EFFECT) {
    bool new_effect = true;

    if (m_private->m_hapticEffectStatus != JOYHAPTIC_UPDATING_RUMBLE_EFFECT) {
      m_private->m_hapticeffect.type = SDL_HAPTIC_SINE;
    }

    m_private->m_hapticeffect.periodic.period = 1000;
    m_private->m_hapticeffect.periodic.magnitude = (unsigned int)(strengthLeft * 0x7FFF);
    m_private->m_hapticeffect.periodic.length = duration;
    m_private->m_hapticeffect.periodic.attack_length = 0;
    m_private->m_hapticeffect.periodic.fade_length = 0;

    if (m_private->m_hapticEffectStatus == JOYHAPTIC_UPDATING_RUMBLE_EFFECT) {
      if (SDL_HapticUpdateEffect(
              m_private->m_haptic, m_private->m_hapticEffectId, &m_private->m_hapticeffect) == 0) {
        m_private->m_hapticEffectStatus = JOYHAPTIC_PLAYING_RUMBLE_EFFECT;
        new_effect = false;
      }
      else {
        SDL_HapticDestroyEffect(m_private->m_haptic, m_private->m_hapticEffectId);
        m_private->m_hapticEffectId = -1;
        CM_Error("Vibration can not be updated. Trying other approach.");
      }
    }

    if (new_effect) {
      // Upload the effect
      m_private->m_hapticEffectId = SDL_HapticNewEffect(m_private->m_haptic,
                                                        &m_private->m_hapticeffect);
    }

    // Run the effect
    if (m_private->m_hapticEffectId >= 0 &&
        SDL_HapticRunEffect(m_private->m_haptic, m_private->m_hapticEffectId, 1) != -1) {
      m_private->m_hapticEffectStatus = JOYHAPTIC_PLAYING_RUMBLE_EFFECT;
    }
    else {
      SDL_HapticDestroyEffect(m_private->m_haptic, m_private->m_hapticEffectId);
      m_private->m_hapticEffectId = -1;
      m_private->m_hapticEffectStatus = JOYHAPTIC_STOPPED;
      CM_Error("Vibration not reproduced. Rumble can not initialized/played");
      m_private->m_hapticEndTime = 0.0;
      return false;
    }
  }
  m_private->m_hapticEndTime = BLI_time_now_seconds() * 1000.0 + (double)duration;
  return true;
#endif  // WITH_SDL
  return false;
}

bool DEV_Joystick::RumbleStop()
{
#ifdef WITH_SDL
  if (m_private->m_haptic == nullptr) {
    return false;
  }

  if (m_private->m_hapticEffectStatus != JOYHAPTIC_STOPPED) {
    m_private->m_hapticEffectStatus = JOYHAPTIC_STOPPED;
  }
  SDL_HapticDestroyEffect(m_private->m_haptic, m_private->m_hapticEffectId);
  m_private->m_hapticEndTime = 0.0;
  return true;
#endif
  return false;
}

bool DEV_Joystick::GetRumbleStatus()
{
#ifdef WITH_SDL
  return (m_private->m_hapticEffectStatus != JOYHAPTIC_STOPPED);
#endif
  return false;
}

bool DEV_Joystick::GetRumbleSupport()
{
#ifdef WITH_SDL
  return (m_private->m_haptic);
#endif
  return false;
}

// We can not trust in SDL_HapticGetEffectStatus function as it is not supported
// in the most used game controllers. Then we work around it using own time management.
void DEV_Joystick::ProcessRumbleStatus()
{
#ifdef WITH_SDL
  if (m_private->m_haptic == nullptr) {
    return;
  }

  if ((BLI_time_now_seconds() * 1000.0) >= m_private->m_hapticEndTime) {
    RumbleStop();
  }
#endif
  return;
}

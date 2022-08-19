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
 * Contributor(s): snailrose, lordloki.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file DEV_JoystickPrivate.h
 *  \ingroup device
 */

#pragma once

#include "DEV_JoystickDefines.h"

#ifdef WITH_SDL
class DEV_Joystick::PrivateData {
 public:
  /*
   * The Game controller
   */
  SDL_GameController *m_gamecontroller;
  SDL_JoystickID m_instance_id;
  SDL_Haptic *m_haptic;
  SDL_HapticEffect m_hapticeffect;
  int m_hapticEffectId;
  int m_hapticEffectStatus;
  double m_hapticEndTime;

  PrivateData()
      : m_gamecontroller(nullptr),
        m_haptic(nullptr),
        m_hapticEffectStatus(JOYHAPTIC_STOPPED),
        m_hapticEndTime(0.0)
  {
  }
};
#endif  // WITH_SDL

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

/** \file DEV_Joystick.h
 *  \ingroup device
 */

#pragma once

#include <string>

#include "DEV_JoystickDefines.h"

#ifdef WITH_SDL
/* SDL force defines __SSE__ and __SSE2__ flags, which generates warnings
 * because we pass those defines via command line as well. For until there's
 * proper ifndef added to SDL headers we ignore the redefinition warning.
 */
#  ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4005)
#  endif
#  include "SDL.h"
#  ifdef _MSC_VER
#    pragma warning(pop)
#  endif
#endif

/**
 * Basic Joystick class
 * I will make this class a singleton because there should be only one joystick
 * even if there are more than one scene using it and count how many scene are using it.
 * The underlying joystick should only be removed when the last scene is removed
 */

class DEV_Joystick

{
  static DEV_Joystick *m_instance[JOYINDEX_MAX];

  class PrivateData;
#ifdef WITH_SDL
  PrivateData *m_private;
#endif
  int m_joyindex;

  /**
   *support for JOYAXIS_MAX axes (in pairs)
   */
  int m_axis_array[JOYAXIS_MAX];

  /**
   * Precision or range of the axes
   */
  int m_prec;

  /**
   * max # of buttons avail
   */

  int m_axismax;
  int m_buttonmax;

  /** is the joystick initialized ?*/
  bool m_isinit;

  /** is triggered for each event type */
  bool m_istrig_axis;
  bool m_istrig_button;

#ifdef WITH_SDL
  /**
   * event callbacks
   */
  void OnAxisEvent(SDL_Event *sdl_event);
  void OnButtonEvent(SDL_Event *sdl_event);
  void OnNothing(SDL_Event *sdl_event);

#endif /* WITH_SDL */
  /**
   * Open the joystick
   */
  bool CreateJoystickDevice(void);

  /**
   * Close the joystick
   */
  void DestroyJoystickDevice(void);

  /**
   * returns m_axis_array
   */

  int pAxisTest(int axisnum);
  /**
   * returns m_axis_array
   */
  int pGetAxis(int axisnum, int udlr);

  DEV_Joystick(short index);

  ~DEV_Joystick();

 public:
  static DEV_Joystick *GetInstance(short joyindex);
  static bool HandleEvents(short (&addrem)[JOYINDEX_MAX]);
  void ReleaseInstance(short joyindex);
  static void Init();
  static void Close();

  /*
   */
  bool aAxisPairIsPositive(int axis);
  bool aAxisPairDirectionIsPositive(int axis,
                                    int dir); /* function assumes joysticks are in axis pairs */
  bool aAxisIsPositive(int axis_single);      /* check a single axis only */

  bool aAnyButtonPressIsPositive(void);
  bool aButtonPressIsPositive(int button);
  bool aButtonReleaseIsPositive(int button);

  /**
   * precision is default '3200' which is overridden by input
   */

  void cSetPrecision(int val);

  int GetAxisPosition(int index)
  {
    return m_axis_array[index];
  }

  bool IsTrigAxis(void)
  {
    return m_istrig_axis;
  }

  bool IsTrigButton(void)
  {
    return m_istrig_button;
  }

  /**
   * Force Feedback - Vibration
   * We could add many optional arguments to these functions to take into account different sort of
   * vibrations. But we propose to keep the UI simple with only joyindex, force (in both motors)
   * and duration. As the vibration strength and duration can be updated on-fly it is possible to
   * generate several types of vibration (sinus, periodic, custom, etc) using BGE python scripts
   * for more advanced uses.
   */
  bool RumblePlay(float strengthLeft, float strengthRight, unsigned int duration);
  bool RumbleStop();
  bool GetRumbleStatus();
  bool GetRumbleSupport();
  void ProcessRumbleStatus();

  /**
   * Test if the joystick is connected
   */
  int Connected(void);

  /**
   * Name of the joytsick
   */
  const std::string GetName();
};

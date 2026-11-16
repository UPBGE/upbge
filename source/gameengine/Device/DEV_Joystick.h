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

#include <cstdint>
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
#  include <SDL3/SDL.h>

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
  int m_bge_joystick_slot;

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

  /**
   * Button state maintained from SDL events, bit i = SDL_GamepadButton i.
   * m_pressed latches down edges since the last logic tick so a press and release that both
   * land inside one tick are still reported as pressed for that tick.
   */
  uint32_t m_buttons;
  uint32_t m_pressed;

  /**
   * Motion sensors, updated from SDL_EVENT_GAMEPAD_SENSOR_UPDATE once EnableSensors() is called.
   * m_gyroDelta accumulates every gyro sample since the previous logic tick, so the rotation a
   * script reads does not depend on how many samples (200 to 1000 Hz) fall inside one tick.
   */
  bool m_hasGyro = false;
  bool m_hasAccel = false;
  float m_gyro[3] = {};      /* rad/s */
  float m_gyroDelta[3] = {}; /* rad since the previous logic tick */
  float m_accel[3] = {};     /* m/s^2, gravity included */
  uint64_t m_gyroLastNs = 0;
  float m_gyroNominalDt = 1.0f / 200.0f;

#ifdef WITH_SDL
  /**
   * event callbacks
   */
  void SetInstanceId(SDL_JoystickID sdl_joystick_id);
  void OnAxisEvent(SDL_Event *sdl_event);
  void OnButtonEvent(SDL_Event *sdl_event);
  void OnSensorEvent(SDL_Event *sdl_event);
  void OnNothing(SDL_Event *sdl_event);

  /** Output capabilities and the player index, resolved when a device opens. */
  void InitOutputs();
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

  DEV_Joystick(short bge_joystick_slot);

  ~DEV_Joystick();

 public:
  static DEV_Joystick *GetInstance(short bge_joystick_slot);
  static bool HandleEvents(short (&addrem)[JOYINDEX_MAX]);

  /**
   * SDL joystick hints: names starting with SDL_JOYSTICK_, SDL_GAMECONTROLLER or SDL_HIDAPI_.
   * Hints set through SetHint() are reset by Close(), so one game's choices do not leak into the
   * next run in the same Blender session. Hints SDL reads when a device opens only affect
   * controllers connected afterwards.
   */
  static bool IsJoystickHint(const char *name);
  static bool SetHint(const char *name, const char *value);
  void ReleaseInstance(short bge_joystick_slot);
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
  bool HasTriggerRumble() const;
  bool RumbleTriggers(float left, float right, unsigned int duration);
  /** Raw, controller-specific effect packet (e.g. DualSense adaptive triggers); see SDL docs. */
  bool SendEffect(const void *data, int size);

  /**
   * Test if the joystick is connected
   */
  int Connected(void);

  /**
   * Name of the joytsick
   */
  const std::string GetName();

  /**
   * Controller identity. The type is cached at open; the rest are
   * single SDL queries, made only when a script asks.
   */
  const char *GetTypeName() const;              /* "xbox360", "ps5", "switchpro", ... */
  const char *GetButtonLabel(int button) const; /* "A", "CROSS", ... or "UNKNOWN" */
  std::string GetGUID() const;                  /* For gamecontrollerdb.txt mappings. */
  const char *GetConnectionName() const;        /* "wired", "wireless" or "unknown" */
  int GetPlayerIndex() const;
  bool SetPlayerIndex(int index);

  /**
   * Motion sensors, off until EnableSensors() is called: while off
   * SDL reports no samples and they cost nothing. Axes follow SDL: x = pitch, y = yaw,
   * z = roll, counter-clockwise positive.
   */
  bool HasGyro() const
  {
    return m_hasGyro;
  }
  bool HasAccel() const
  {
    return m_hasAccel;
  }
  bool EnableSensors(bool gyro, bool accel);
  const float *GetGyro() const
  {
    return m_gyro;
  }
  const float *GetGyroDelta() const
  {
    return m_gyroDelta;
  }
  const float *GetAccel() const
  {
    return m_accel;
  }

  /**
   * Touchpads. Fingers are written as {down, x, y, pressure}, with x
   * and y in [0, 1] from the top-left corner; returns the number of fingers written.
   */
  int GetNumTouchpads() const;
  int GetTouchpadFingers(int touchpad, float (*r_fingers)[4], int max_fingers) const;

  /**
   * Battery: SDL power state ("on_battery", "no_battery", "charging", "charged", "unknown" or
   * "error"); r_percent receives the charge level, or -1 when unknown.
   */
  const char *GetBatteryState(int *r_percent) const;

  /**
   * LED. Each call is at most one HID write, and SDL already skips
   * an LED write that repeats the current color: call them when the value changes, not per frame.
   * Single-color LEDs use the brightest of the three channels.
   */
  bool HasLED() const;
  bool SetLED(unsigned char red, unsigned char green, unsigned char blue);
};

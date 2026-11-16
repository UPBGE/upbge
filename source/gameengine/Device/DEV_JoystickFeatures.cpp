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
 * Optional controller features exposed by SDL 3. Nothing here runs per logic tick: every
 * function is called only when a script asks. Sensor samples are handled in
 * DEV_JoystickEvents.cpp.
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
  if (m_private->m_joystick) {
    /* The category the driver reports for a raw joystick. SDL has no strings for these, so the
     * names follow the SDL_JoystickType enumerators. */
    switch (SDL_GetJoystickType(m_private->m_joystick)) {
      case SDL_JOYSTICK_TYPE_GAMEPAD: /* A gamepad without a mapping. */
        return "gamepad";
      case SDL_JOYSTICK_TYPE_WHEEL:
        return "wheel";
      case SDL_JOYSTICK_TYPE_ARCADE_STICK:
        return "arcade_stick";
      case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
        return "flight_stick";
      case SDL_JOYSTICK_TYPE_DANCE_PAD:
        return "dance_pad";
      case SDL_JOYSTICK_TYPE_GUITAR:
        return "guitar";
      case SDL_JOYSTICK_TYPE_DRUM_KIT:
        return "drum_kit";
      case SDL_JOYSTICK_TYPE_ARCADE_PAD:
        return "arcade_pad";
      case SDL_JOYSTICK_TYPE_THROTTLE:
        return "throttle";
      default:
        return "joystick";
    }
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
  if (m_private->m_joystick) {
    char guid[33]; /* 32 hex digits + terminator. */
    SDL_GUIDToString(SDL_GetJoystickGUID(m_private->m_joystick), guid, sizeof(guid));
    return guid;
  }
#endif
  return "";
}

int DEV_Joystick::GetVendorId() const
{
#ifdef WITH_SDL
  if (m_private->m_joystick) {
    return SDL_GetJoystickVendor(m_private->m_joystick);
  }
#endif
  return 0;
}

int DEV_Joystick::GetProductId() const
{
#ifdef WITH_SDL
  if (m_private->m_joystick) {
    return SDL_GetJoystickProduct(m_private->m_joystick);
  }
#endif
  return 0;
}

const char *DEV_Joystick::GetConnectionName() const
{
#ifdef WITH_SDL
  if (m_private->m_joystick) {
    switch (SDL_GetJoystickConnectionState(m_private->m_joystick)) {
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
  if (m_private->m_joystick) {
    return SDL_GetJoystickPlayerIndex(m_private->m_joystick);
  }
#endif
  return -1;
}

bool DEV_Joystick::SetPlayerIndex(int index)
{
#ifdef WITH_SDL
  if (m_private->m_joystick) {
    return SDL_SetJoystickPlayerIndex(m_private->m_joystick, index);
  }
#endif
  return false;
}

bool DEV_Joystick::EnableSensors(bool gyro, bool accel)
{
#ifdef WITH_SDL
  SDL_Gamepad *gamepad = m_private->m_gamecontroller;
  if (!gamepad) {
    return false;
  }
  bool ok = true;
  if (m_hasGyro) {
    ok &= SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, gyro);
    const float rate = SDL_GetGamepadSensorDataRate(gamepad, SDL_SENSOR_GYRO);
    m_gyroNominalDt = (rate > 0.0f) ? 1.0f / rate : 1.0f / 200.0f;
  }
  else {
    ok &= !gyro;
  }
  if (m_hasAccel) {
    ok &= SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, accel);
  }
  else {
    ok &= !accel;
  }
  /* Recorded as requested even if SDL refused: a Motion sensor tests these every tick and must
   * not retry an SDL call per tick. */
  m_gyroOn = gyro && m_hasGyro;
  m_accelOn = accel && m_hasAccel;
  /* The next gyro sample has no predecessor to measure its interval from. A disabled sensor
   * keeps no stale reading. */
  m_gyroLastNs = 0;
  for (int i = 0; i < 3; i++) {
    if (!gyro) {
      m_gyro[i] = m_gyroDelta[i] = 0.0f;
    }
    if (!accel) {
      m_accel[i] = 0.0f;
    }
  }
  return ok;
#else
  return false;
#endif
}

int DEV_Joystick::GetNumTouchpads() const
{
#ifdef WITH_SDL
  if (m_private->m_gamecontroller) {
    return SDL_GetNumGamepadTouchpads(m_private->m_gamecontroller);
  }
#endif
  return 0;
}

int DEV_Joystick::GetTouchpadFingers(int touchpad, float (*r_fingers)[4], int max_fingers) const
{
#ifdef WITH_SDL
  SDL_Gamepad *gamepad = m_private->m_gamecontroller;
  if (!gamepad) {
    return 0;
  }
  int count = SDL_GetNumGamepadTouchpadFingers(gamepad, touchpad);
  count = (count < max_fingers) ? count : max_fingers;
  for (int f = 0; f < count; f++) {
    bool down = false;
    float x = 0.0f, y = 0.0f, pressure = 0.0f;
    if (!SDL_GetGamepadTouchpadFinger(gamepad, touchpad, f, &down, &x, &y, &pressure)) {
      down = false;
      x = y = pressure = 0.0f;
    }
    r_fingers[f][0] = down ? 1.0f : 0.0f;
    r_fingers[f][1] = x;
    r_fingers[f][2] = y;
    r_fingers[f][3] = pressure;
  }
  return (count > 0) ? count : 0;
#else
  return 0;
#endif
}

const char *DEV_Joystick::GetBatteryState(int *r_percent) const
{
  *r_percent = -1;
#ifdef WITH_SDL
  if (m_private->m_joystick) {
    switch (SDL_GetJoystickPowerInfo(m_private->m_joystick, r_percent)) {
      case SDL_POWERSTATE_ON_BATTERY:
        return "on_battery";
      case SDL_POWERSTATE_NO_BATTERY:
        return "no_battery";
      case SDL_POWERSTATE_CHARGING:
        return "charging";
      case SDL_POWERSTATE_CHARGED:
        return "charged";
      case SDL_POWERSTATE_ERROR:
        return "error";
      default:
        break;
    }
  }
#endif
  return "unknown";
}

bool DEV_Joystick::HasLED() const
{
#ifdef WITH_SDL
  return m_private->m_hasLED;
#else
  return false;
#endif
}

bool DEV_Joystick::SetLED(unsigned char red, unsigned char green, unsigned char blue)
{
#ifdef WITH_SDL
  return m_private->m_hasLED && SDL_SetJoystickLED(m_private->m_joystick, red, green, blue);
#else
  return false;
#endif
}

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

/** \file DEV_RawJoystick.h
 *  \ingroup device
 *
 * Raw access to every SDL joystick-class device (gamepads, flight sticks, throttles, wheels,
 * pedals, button boxes...), without the Xbox layout mapping applied by the SDL Gamepad API used
 * in #DEV_Joystick. Exposed to Python as `bge.logic.inputDevices`.
 */

#pragma once

#include <string>

#include "DEV_JoystickDefines.h" /* For RAWJOYINDEX_MAX. */

#ifdef WITH_SDL
/* See DEV_Joystick.h for the reason of this warning suppression. */
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
 * A raw SDL joystick device.
 *
 * Instances are created and destroyed exclusively by #HandleEvent when SDL reports a device as
 * added or removed, and are stored in a static slot table of #RAWJOYINDEX_MAX entries.
 * Input values are polled from SDL on demand; SDL updates the joystick state every time
 * #DEV_Joystick::HandleEvents pumps the event queue.
 *
 * Devices that SDL recognizes as gamepads are also exposed through #DEV_Joystick. SDL reference
 * counts opened joysticks, so opening the same device in both places is safe.
 */
class DEV_RawJoystick {
 public:
  /** Device category reported by the driver, mirrors `SDL_JoystickType`. */
  enum class Type {
    Unknown,
    Gamepad,
    Wheel,
    ArcadeStick,
    FlightStick,
    DancePad,
    Guitar,
    DrumKit,
    ArcadePad,
    Throttle,
  };

  /**
   * Return the device in \a slot, or nullptr if the slot is empty or out of range.
   */
  static DEV_RawJoystick *GetInstance(short slot);

#ifdef WITH_SDL
  /**
   * Process an SDL event, opening a device on `SDL_EVENT_JOYSTICK_ADDED` and closing it on
   * `SDL_EVENT_JOYSTICK_REMOVED`. Other events are ignored.
   *
   * \param addrem: Set to 1 for a slot whose device was added, 2 for a slot whose device was
   * removed. Other entries are left untouched.
   * \return True if a slot was added or removed.
   */
  static bool HandleEvent(const SDL_Event *sdl_event, short (&addrem)[RAWJOYINDEX_MAX]);
#endif

  /**
   * Close and free all open devices. Must be called before the SDL joystick subsystem is shut
   * down.
   */
  static void Close();

  /** Slot of this device in the slot table. */
  short GetSlot() const;

  /** Name of the device as reported by the driver, empty if unknown. */
  std::string GetName() const;

  /**
   * SDL GUID of the device as 32 hexadecimal characters. It stays the same across sessions
   * and slots for the same device model, which makes it suited to identify a device.
   */
  std::string GetGUID() const;

  /** USB vendor ID, 0 if unavailable. */
  int GetVendorId() const;

  /** USB product ID, 0 if unavailable. */
  int GetProductId() const;

  /** Device category reported by the driver. */
  Type GetType() const;

  /** True if SDL recognizes the device as a gamepad, which is then also handled by #DEV_Joystick. */
  bool IsGamepad() const;

  /** True while the device is connected. */
  bool IsConnected() const;

  /** Number of axes of the device. */
  int GetNumAxes() const;

  /** Number of buttons of the device. */
  int GetNumButtons() const;

  /** Number of hats (POV switches) of the device. */
  int GetNumHats() const;

  /**
   * Position of an axis normalized to [-1.0, 1.0], 0.0 for an invalid \a index.
   */
  float GetAxis(int index) const;

  /**
   * Pressed state of a button, false for an invalid \a index.
   */
  bool GetButton(int index) const;

  /**
   * Position of a hat as a bitmask of `SDL_HAT_UP` (1), `SDL_HAT_RIGHT` (2), `SDL_HAT_DOWN` (4)
   * and `SDL_HAT_LEFT` (8), 0 when centered or for an invalid \a index.
   */
  int GetHat(int index) const;

 private:
  static DEV_RawJoystick *m_instance[RAWJOYINDEX_MAX];

  short m_slot;

#ifdef WITH_SDL
  SDL_JoystickID m_sdl_joystick_id;
  SDL_Joystick *m_joystick;

  /** Open the SDL device \a sdl_joystick_id, check #m_joystick for success. */
  DEV_RawJoystick(short slot, SDL_JoystickID sdl_joystick_id);
#endif

  /** Close the SDL device. */
  ~DEV_RawJoystick();
};

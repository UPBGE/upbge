/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_DisplayManagerWin32 class.
 */

#pragma once

#ifndef WIN32
#  error WIN32 only!
#endif  // WIN32

#include "GHOST_DisplayManager.h"

/**
 * Manages system displays  (WIN32 implementation).
 */
class GHOST_DisplayManagerWin32 : public GHOST_DisplayManager {
 public:
  /**
   * Constructor.
   */
  GHOST_DisplayManagerWin32(void);

  /**
   * Returns the number of display devices on this system.
   * \param numDisplays: The number of displays on this system.
   * \return Indication of success.
   */
  GHOST_TSuccess getNumDisplays(uint8_t &numDisplays) const;

  /**
   * Returns the number of display settings for this display device.
   * \param display: The index of the display to query with 0 <= display < getNumDisplays().
   * \param numSetting: The number of settings of the display device with this index.
   * \return Indication of success.
   */
  GHOST_TSuccess getNumDisplaySettings(uint8_t display, int32_t &numSettings) const;

  /**
   * Returns the current setting for this display device.
   * \param display: The index of the display to query with 0 <= display < getNumDisplays().
   * \param index: The setting index to be returned.
   * \param setting: The setting of the display device with this index.
   * \return Indication of success.
   */
  GHOST_TSuccess getDisplaySetting(uint8_t display,
                                   int32_t index,
                                   GHOST_DisplaySetting &setting) const;

  /**
   * Returns the current setting for this display device.
   * \param display: The index of the display to query with 0 <= display < getNumDisplays().
   * \param setting: The current setting of the display device with this index.
   * \return Indication of success.
   */
  GHOST_TSuccess getCurrentDisplaySetting(uint8_t display, GHOST_DisplaySetting &setting) const;

  /**
   * Changes the current setting for this display device.
   * \param display: The index of the display to query with 0 <= display < getNumDisplays().
   * \param setting: The current setting of the display device with this index.
   * \return Indication of success.
   */
  GHOST_TSuccess setCurrentDisplaySetting(uint8_t display, const GHOST_DisplaySetting &setting);

 protected:
};

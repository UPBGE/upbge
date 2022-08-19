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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file DEV_InputDevice.h
 *  \ingroup device
 */

#pragma once

#include <map>

#include "SCA_IInputDevice.h"

class DEV_InputDevice : public SCA_IInputDevice {
 protected:
  /// These maps converts GHOST input number to SCA input enum.
  std::map<int, SCA_EnumInputs> m_reverseKeyTranslateTable;
  std::map<int, SCA_EnumInputs> m_reverseButtonTranslateTable;
  std::map<int, SCA_EnumInputs> m_reverseWindowTranslateTable;

 public:
  DEV_InputDevice();
  virtual ~DEV_InputDevice();

  void ConvertKeyEvent(int incode, int val, unsigned int unicode);
  void ConvertButtonEvent(int incode, int val);
  void ConvertWindowEvent(int incode);
  void ConvertMoveEvent(int x, int y);
  void ConvertWheelEvent(int z);
  void ConvertEvent(SCA_IInputDevice::SCA_EnumInputs type, int val, unsigned int unicode);
};

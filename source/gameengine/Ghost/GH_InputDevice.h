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

/** \file GH_InputDevice.h
 *  \ingroup ghost
 */

#ifndef __GH_KEYBOARDDEVICE_H__
#define __GH_KEYBOARDDEVICE_H__

#include "SCA_IInputDevice.h"

#include <map>


/**
 * System independent implementation of SCA_IInputDevice.
 * System dependent keyboard devices need only to inherit this class
 * and fill the m_reverseKeyTranslateTable key translation map.
 * \see SCA_IInputDevice
 */

class GH_InputDevice : public SCA_IInputDevice
{
protected:
	/**
	 * These maps converts system dependent keyboard codes into Ketsji codes.
	 * System dependent keyboard codes are stored as ints.
	 */
	std::map<int, SCA_EnumInputs> m_reverseKeyTranslateTable;
	std::map<int, SCA_EnumInputs> m_reverseButtonTranslateTable;
	std::map<int, SCA_EnumInputs> m_reverseWindowTranslateTable;

public:
	GH_InputDevice();
	virtual ~GH_InputDevice();

	void ConvertKeyEvent(int incode, int val, unsigned int unicode);
	void ConvertButtonEvent(int incode, int val);
	void ConvertWindowEvent(int incode);
	void ConvertMoveEvent(int x, int y);
	void ConvertWheelEvent(int z);
	void ConvertEvent(SCA_IInputDevice::SCA_EnumInputs type, int val, unsigned int unicode);
};

#endif  /* __GH_KEYBOARDDEVICE_H__ */

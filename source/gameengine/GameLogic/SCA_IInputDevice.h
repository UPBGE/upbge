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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_IInputDevice.h
 *  \ingroup gamelogic
 */

#ifndef __SCA_IINPUTDEVICE_H__
#define __SCA_IINPUTDEVICE_H__

#ifdef WITH_CXX_GUARDEDALLOC
#include "MEM_guardedalloc.h"
#endif

#include "SCA_InputEvent.h"

#define MOUSEX         MOUSEMOVE
#define MOUSEY         ACTIONMOUSE

class SCA_IInputDevice 
{
public:
	SCA_IInputDevice();
	virtual ~SCA_IInputDevice();

	enum SCA_EnumInputs {
		KX_NOKEY = 0,

		KX_BEGINWIN,

		KX_WINRESIZE,
		KX_WINCLOSE,
		KX_WINQUIT,

		KX_ENDWIN,

		KX_BEGINKEY,

		KX_RETKEY,
		KX_SPACEKEY,
		KX_PADASTERKEY,
		KX_COMMAKEY,
		KX_MINUSKEY,
		KX_PERIODKEY,
		KX_PLUSKEY,
		KX_ZEROKEY,

		KX_ONEKEY,
		KX_TWOKEY,
		KX_THREEKEY,
		KX_FOURKEY,
		KX_FIVEKEY,
		KX_SIXKEY,
		KX_SEVENKEY,
		KX_EIGHTKEY,
		KX_NINEKEY,

		KX_AKEY,
		KX_BKEY,
		KX_CKEY,
		KX_DKEY,
		KX_EKEY,
		KX_FKEY,
		KX_GKEY,
		KX_HKEY,
		KX_IKEY,
		KX_JKEY,
		KX_KKEY,
		KX_LKEY,
		KX_MKEY,
		KX_NKEY,
		KX_OKEY,
		KX_PKEY,
		KX_QKEY,
		KX_RKEY,
		KX_SKEY,
		KX_TKEY,
		KX_UKEY,
		KX_VKEY,
		KX_WKEY,
		KX_XKEY,
		KX_YKEY,
		KX_ZKEY,

		KX_CAPSLOCKKEY,

		KX_LEFTCTRLKEY,
		KX_LEFTALTKEY,
		KX_RIGHTALTKEY,
		KX_RIGHTCTRLKEY,
		KX_RIGHTSHIFTKEY,
		KX_LEFTSHIFTKEY,

		KX_ESCKEY,
		KX_TABKEY,

		KX_LINEFEEDKEY,
		KX_BACKSPACEKEY,
		KX_DELKEY,
		KX_SEMICOLONKEY,

		KX_QUOTEKEY,
		KX_ACCENTGRAVEKEY,

		KX_SLASHKEY,
		KX_BACKSLASHKEY,
		KX_EQUALKEY,
		KX_LEFTBRACKETKEY,
		KX_RIGHTBRACKETKEY,

		KX_LEFTARROWKEY,
		KX_DOWNARROWKEY,
		KX_RIGHTARROWKEY,
		KX_UPARROWKEY,

		KX_PAD2,
		KX_PAD4,
		KX_PAD6,
		KX_PAD8,

		KX_PAD1,
		KX_PAD3,
		KX_PAD5,
		KX_PAD7,
		KX_PAD9,
		
		KX_PADPERIOD,
		KX_PADSLASHKEY,

		KX_PAD0,
		KX_PADMINUS,
		KX_PADENTER,
		KX_PADPLUSKEY,

		KX_F1KEY,
		KX_F2KEY,
		KX_F3KEY,
		KX_F4KEY,
		KX_F5KEY,
		KX_F6KEY,
		KX_F7KEY,
		KX_F8KEY,
		KX_F9KEY,
		KX_F10KEY,
		KX_F11KEY,
		KX_F12KEY,
		KX_F13KEY,
		KX_F14KEY,
		KX_F15KEY,
		KX_F16KEY,
		KX_F17KEY,
		KX_F18KEY,
		KX_F19KEY,

		KX_OSKEY,

		KX_PAUSEKEY,
		KX_INSERTKEY,
		KX_HOMEKEY,
		KX_PAGEUPKEY,
		KX_PAGEDOWNKEY,
		KX_ENDKEY,

		// MOUSE
		KX_BEGINMOUSE,

		KX_BEGINMOUSEBUTTONS,

		KX_LEFTMOUSE,
		KX_MIDDLEMOUSE,
		KX_RIGHTMOUSE,

		KX_ENDMOUSEBUTTONS,

		KX_WHEELUPMOUSE,
		KX_WHEELDOWNMOUSE,

		KX_MOUSEX,
		KX_MOUSEY,

		KX_ENDMOUSE,

		KX_MAX_KEYS
	}; // enum


protected:
	/// Table of all possible input.
	SCA_InputEvent m_eventsTable[SCA_IInputDevice::KX_MAX_KEYS];
	/// Typed text in unicode during a frame.
	std::wstring m_text;

public:
	virtual SCA_InputEvent& GetEvent(SCA_IInputDevice::SCA_EnumInputs inputcode);

	/**
	 * Count active events(active and just_activated)
	 */
	virtual int		GetNumActiveEvents();

	/**
	 * Get the number of remapping events (just_activated, just_released)
	 */
	virtual int		GetNumJustEvents();
	
	virtual void		HookEscape();

	/** Clear event:
	 *     - Clear status and copy last status to first status.
	 *     - Clear queue
	 *     - Clear values and copy last value to first value.
	 */
	virtual void ClearEvents();

	/** Manage move event like mouse by releasing if possible.
	 * These kind of events are precise of one frame.
	 */
	virtual void ReleaseMoveEvent();

	/// Return typed unicode text during a frame.
	const std::wstring& GetText() const;


#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:SCA_InputEvent")
#endif
};

#endif	 /* __SCA_IINPUTDEVICE_H__ */


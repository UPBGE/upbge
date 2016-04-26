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

/** \file gameengine/GameLogic/SCA_IInputDevice.cpp
 *  \ingroup gamelogic
 */


#include "BLI_utildefines.h"
#include "SCA_IInputDevice.h"

SCA_IInputDevice::SCA_IInputDevice()
{
	for (int i = 0; i < SCA_IInputDevice::KX_MAX_KEYS; ++i) {
		m_eventsTable[i] = SCA_InputEvent();
	}
}

SCA_IInputDevice::~SCA_IInputDevice()
{
}

void SCA_IInputDevice::HookEscape()
{
	BLI_assert(false && "This device does not support hooking escape.");
}

void SCA_IInputDevice::ClearEvents()
{
	for (int i = 0; i < SCA_IInputDevice::KX_MAX_KEYS; ++i) {
		m_eventsTable[i].Clear();
	}
}

void SCA_IInputDevice::ReleaseMoveEvent()
{
	/* We raise the release mose move event if:
	 *   - there are only one value from the last call to Clear()
	 *   - the last state was KX_ACTIVE
	 * If the both are true then the KX_ACTIVE come from the last call to ClearEvent and must
	 * be removed of the status list to avoid setting the mouse active for two frames.
	 */
	SCA_InputEvent &xevent = m_eventsTable[KX_MOUSEX];
	if ((xevent.m_values.size() == 1) && (xevent.m_status[xevent.m_status.size() - 1] == SCA_InputEvent::KX_ACTIVE)) {
		xevent.m_status.pop_back();
		xevent.m_status.push_back(SCA_InputEvent::KX_NO_INPUTSTATUS);
		xevent.m_queue.push_back(SCA_InputEvent::KX_JUSTRELEASED);
	}

	SCA_InputEvent &yevent = m_eventsTable[KX_MOUSEY];
	if ((yevent.m_values.size() == 1) && (yevent.m_status[yevent.m_status.size() - 1] == SCA_InputEvent::KX_ACTIVE)) {
		yevent.m_status.pop_back();
		yevent.m_status.push_back(SCA_InputEvent::KX_NO_INPUTSTATUS);
		yevent.m_queue.push_back(SCA_InputEvent::KX_JUSTRELEASED);
	}
}

SCA_InputEvent& SCA_IInputDevice::GetEvent(SCA_IInputDevice::SCA_EnumInputs inputcode)
{
	return m_eventsTable[inputcode];
}

int SCA_IInputDevice::GetNumActiveEvents()
{
	int num = 0;

	for (int i = 0; i < SCA_IInputDevice::KX_MAX_KEYS; ++i) {
		const SCA_InputEvent& event = m_eventsTable[i];
		if (event.Find(SCA_InputEvent::KX_ACTIVE)) {
			++num;
		}
	}

	return num;
}

int SCA_IInputDevice::GetNumJustEvents()
{
	int num = 0;

	for (int i = 0; i < SCA_IInputDevice::KX_MAX_KEYS; ++i) {
		const SCA_InputEvent& event = m_eventsTable[i];
		if (event.Find(SCA_InputEvent::KX_JUSTACTIVATED) || event.Find(SCA_InputEvent::KX_JUSTRELEASED)) {
			++num;
		}
	}

	return num;
}

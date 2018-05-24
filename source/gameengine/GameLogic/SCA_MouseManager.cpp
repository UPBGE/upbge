/*
 * Manager for mouse events
 *
 *
 *
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

/** \file gameengine/GameLogic/SCA_MouseManager.cpp
 *  \ingroup gamelogic
 */


#ifdef _MSC_VER
/* This warning tells us about truncation of __long__ stl-generated names.
 * It can occasionally cause DevStudio to have internal compiler warnings. */
#  pragma warning( disable:4786 )
#endif

#include "EXP_PropBool.h"
#include "SCA_MouseManager.h"
#include "SCA_MouseSensor.h"
#include "EXP_PropInt.h"


SCA_MouseManager::SCA_MouseManager(SCA_LogicManager *logicmgr,
                                   SCA_IInputDevice *mousedev)
	:SCA_EventManager(logicmgr, MOUSE_EVENTMGR),
	m_mousedevice(mousedev)
{
}



SCA_MouseManager::~SCA_MouseManager()
{
}



SCA_IInputDevice *SCA_MouseManager::GetInputDevice()
{
	return m_mousedevice;
}



void SCA_MouseManager::NextFrame()
{
	if (m_mousedevice) {
		for (SCA_ISensor *sensor : m_sensors) {
			SCA_MouseSensor *mousesensor = static_cast<SCA_MouseSensor *>(sensor);
			// (0,0) is the Upper Left corner in our local window
			// coordinates
			if (!mousesensor->IsSuspended()) {
				const SCA_InputEvent& event1 =
					m_mousedevice->GetInput(SCA_IInputDevice::MOUSEX);
				const SCA_InputEvent& event2 =
					m_mousedevice->GetInput(SCA_IInputDevice::MOUSEY);

				int mx = event1.m_values[event1.m_values.size() - 1];
				int my = event2.m_values[event2.m_values.size() - 1];

				mousesensor->setX(mx);
				mousesensor->setY(my);

				mousesensor->Activate(m_logicmgr);
			}
		}
	}
}

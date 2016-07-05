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

/** \file gameengine/GameLogic/Joystick/SCA_JoystickEvents.cpp
 *  \ingroup gamelogic
 */

#include "SCA_Joystick.h"
#include "SCA_JoystickPrivate.h"

#ifdef _MSC_VER
#  include <cstdio> /* printf */
#endif

#ifdef WITH_SDL
/* To convert sdl device mapping to instance mapping */
static int GetInstanceFromMapping(int device_num, int instancemapping[])
{
	for (int i = 0; i < JOYINDEX_MAX; i++) {
		if (device_num == instancemapping[i]) {
			return i;
		}
	}
	return -2;
}

void SCA_Joystick::OnAxisEvent(SDL_Event* sdl_event)
{
	if (!this || sdl_event->caxis.axis >= JOYAXIS_MAX)
		return;
	
	m_axis_array[sdl_event->caxis.axis] = sdl_event->caxis.value;
	m_istrig_axis = 1;
}

/* See notes below in the event loop */
void SCA_Joystick::OnButtonEvent(SDL_Event* sdl_event)
{
	if (!this)
		return;
	m_istrig_button = 1;
}


void SCA_Joystick::OnNothing(SDL_Event* sdl_event)
{
	if (!this)
		return;
	m_istrig_axis = m_istrig_button = 0;
}

void SCA_Joystick::HandleEvents(void)
{
	SDL_Event		sdl_event;

	if (SDL_PollEvent == (void*)0) {
		return;
	}

	for (int i = 0; i < m_joynum; i++) {
		if (SCA_Joystick::m_instance[i])
			SCA_Joystick::m_instance[i]->OnNothing(&sdl_event);
	}
	
	while (SDL_PollEvent(&sdl_event)) {
		/* Note! m_instance[instance]
		 * will segfault if over JOYINDEX_MAX, not too nice but what are the chances? */
		
		/* Note!, with buttons, this wont care which button is pressed,
		 * only to set 'm_istrig_button', actual pressed buttons are detected by SDL_JoystickGetButton */
		
		/* Note!, if you manage to press and release a button within 1 logic tick
		 * it wont work as it should */

		/* Note!, we need to use GetInstanceFromMapping function for index conversion as
		   sdl_event.cdevice.which returns correct index when is called from SDL_CONTROLLERDEVICEADDED event but
		   it returns an accumulative index when is called from SDL_CONTROLLERDEVICEREMOVED, SDL_CONTROLLERAXISMOTION,
		   SDL_CONTROLLERBUTTONUP or SDL_CONTROLLERBUTTONDOWN events */
		int inst_mapping;
		
		switch (sdl_event.type) {
			case SDL_CONTROLLERDEVICEADDED:
				if (m_joynum != SDL_NumJoysticks()) {
					for (int j = 0; j < JOYINDEX_MAX; j++) {
						if (!SCA_Joystick::m_instance[j]) {
							SCA_Joystick::m_instance[j] = new SCA_Joystick(j);
							SCA_Joystick::m_instance[j]->CreateJoystickDevice();
							m_joynum++;
							m_instancemapping[j] = ++m_refCount;
							SCA_Joystick::SetJoystickUpdateStatus(true);
							break;
						}
					}
				}
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				inst_mapping = GetInstanceFromMapping(sdl_event.cdevice.which, m_instancemapping);
				if (SCA_Joystick::m_instance[inst_mapping]) {
					SCA_Joystick::m_instance[inst_mapping]->ReleaseInstance(inst_mapping);
					m_joynum--;
					SCA_Joystick::SetJoystickUpdateStatus(true);
				}
				break;
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
				inst_mapping = GetInstanceFromMapping(sdl_event.cdevice.which, m_instancemapping);
				SCA_Joystick::m_instance[inst_mapping]->OnButtonEvent(&sdl_event);
				break;
			case SDL_CONTROLLERAXISMOTION:
				inst_mapping = GetInstanceFromMapping(sdl_event.cdevice.which, m_instancemapping);
				SCA_Joystick::m_instance[inst_mapping]->OnAxisEvent(&sdl_event);
				break;
			default:
				/* ignore old SDL_JOYSTICKS events */
				break;
		}
	}
}
#endif /* WITH_SDL */

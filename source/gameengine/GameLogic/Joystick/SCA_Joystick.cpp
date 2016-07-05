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
 * Contributor(s): snailrose, lordloki
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/Joystick/SCA_Joystick.cpp
 *  \ingroup gamelogic
 */

#ifdef WITH_SDL
#  include <SDL.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#include "SCA_Joystick.h"
#include "SCA_JoystickPrivate.h"
#include "SCA_JoystickMappingdb.h"

#ifdef WITH_SDL
#  define SDL_CHECK(x) ((x) != (void *)0)
#endif

SCA_Joystick::SCA_Joystick(short index)
	:
	m_joyindex(index),
	m_prec(3200),
	m_axismax(-1),
	m_buttonmax(-1),
	m_isinit(0),
	m_istrig_axis(0),
	m_istrig_button(0)
{
	for (int i=0; i < JOYAXIS_MAX; i++)
		m_axis_array[i] = 0;
	
#ifdef WITH_SDL
	m_private = new PrivateData();
#endif
}


SCA_Joystick::~SCA_Joystick()

{
#ifdef WITH_SDL
	delete m_private;
#endif
}

SCA_Joystick *SCA_Joystick::m_instance[JOYINDEX_MAX];
int SCA_Joystick::m_joynum;
int SCA_Joystick::m_refCount;
int SCA_Joystick::m_instancemapping[JOYINDEX_MAX];
bool SCA_Joystick::m_joystickupdatestatus;


void SCA_Joystick::Init()
{
#ifdef WITH_SDL

	if (!(SDL_CHECK(SDL_InitSubSystem)) ||
	    !(SDL_CHECK(SDL_GameControllerAddMapping)) ||
	    !(SDL_CHECK(SDL_NumJoysticks))) {
		return;
	}

	/* Initializing Game Controller related subsystems */
	bool success = (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != -1 );

	if (success) {
		/*Initializing variables */
		m_joynum = 0;
		m_refCount = -1;
		m_joystickupdatestatus = false;

		for (int ins = 0; ins < JOYINDEX_MAX; ins++)
			m_instancemapping[ins] = -1;

		/* Loading Game Controller mapping data base from a string */
		unsigned short i = 0;
		const char *mapping_string = NULL;
		mapping_string = controller_mappings[i];

		while (mapping_string) {
			SDL_GameControllerAddMapping(mapping_string);
			i++;
			mapping_string = controller_mappings[i];
	    }

		/* Creating Game Controllers that are already connected */
		m_joynum = SDL_NumJoysticks();
		printf("m_refCount = %i\n", m_refCount);
		for (int j = 0; j < m_joynum; j++) {
			m_instance[j] = new SCA_Joystick(j);
			m_instance[j]->CreateJoystickDevice();
			m_instancemapping[j] = ++m_refCount;
		}
	}
	else {
		printf("Error initializing SDL Game Controller: %s\n", SDL_GetError());
	}
#endif
}

void SCA_Joystick::Close()
{
#ifdef WITH_SDL
	SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);
#endif
}

SCA_Joystick *SCA_Joystick::GetInstance(short joyindex)
{
#ifndef WITH_SDL
	return NULL;
#else  /* WITH_SDL */

	if (joyindex < 0 || joyindex >= JOYINDEX_MAX) {
		printf("Error-invalid joystick index: %i\n", joyindex);
		return NULL;
	}

	return m_instance[joyindex];
#endif /* WITH_SDL */
}

void SCA_Joystick::ReleaseInstance(short joyindex)
{
#ifdef WITH_SDL
	if (m_instance[joyindex]) {
		m_instance[joyindex]->DestroyJoystickDevice();
		delete m_instance[joyindex];
		m_instance[joyindex] = NULL;
	}
#endif /* WITH_SDL */
}

void SCA_Joystick::cSetPrecision(int val)
{
	m_prec = val;
}


bool SCA_Joystick::aAxisPairIsPositive(int axis)
{
	return (pAxisTest(axis) > m_prec) ? true:false;
}

bool SCA_Joystick::aAxisPairDirectionIsPositive(int axis, int dir)
{

	int res;

	if (dir==JOYAXIS_UP || dir==JOYAXIS_DOWN)
		res = pGetAxis(axis, 1);
	else /* JOYAXIS_LEFT || JOYAXIS_RIGHT */
		res = pGetAxis(axis, 0);
	
	if (dir==JOYAXIS_DOWN || dir==JOYAXIS_RIGHT)
		return (res > m_prec) ? true : false;
	else /* JOYAXIS_UP || JOYAXIS_LEFT */
		return (res < -m_prec) ? true : false;
}

bool SCA_Joystick::aAxisIsPositive(int axis_single)
{
	return abs(m_axis_array[axis_single]) > m_prec ? true:false;
}

bool SCA_Joystick::aAnyButtonPressIsPositive(void)
{
#ifdef WITH_SDL
	if (!(SDL_CHECK(SDL_GameControllerGetButton))) {
		return false;
	}

	/* this is needed for the "all events" option
	 * so we know if there are no buttons pressed */
	for (int i = 0; i < m_buttonmax; i++) {
		if (SDL_GameControllerGetButton(m_private->m_gamecontroller, (SDL_GameControllerButton)i)) {
			return true;
		}
	}
#endif
	return false;
}

bool SCA_Joystick::aButtonPressIsPositive(int button)
{
#ifdef WITH_SDL
	if ((SDL_CHECK(SDL_GameControllerGetButton) &&
	     SDL_GameControllerGetButton(m_private->m_gamecontroller, (SDL_GameControllerButton)button)))
	{
		return true;
	}
#endif
	return false;
}


bool SCA_Joystick::aButtonReleaseIsPositive(int button)
{
#ifdef WITH_SDL
	if (!(SDL_CHECK(SDL_GameControllerGetButton) &&
       SDL_GameControllerGetButton(m_private->m_gamecontroller, (SDL_GameControllerButton)button)))
	{
		return true;
	}
#endif
	return false;
}

bool SCA_Joystick::CreateJoystickDevice(void)
{
	bool joy_error = false;

#ifndef WITH_SDL
	m_isinit = true;
	joy_error = true;
#else /* WITH_SDL */
	if (!m_isinit) {
		if (m_joynum >= JOYINDEX_MAX) {
			printf("Maximum quantity (8) of Game Controllers connected. It is not possible to set up additional ones.\n");
			joy_error = true;
		}

		if (!joy_error &&
		    !(SDL_CHECK(SDL_IsGameController) &&
		    SDL_CHECK(SDL_GameControllerOpen) &&
		    SDL_CHECK(SDL_GameControllerEventState)) &&
		    !SDL_IsGameController(m_joyindex))
		{
			/* mapping instruccions if joystick is not a game controller */
			printf("Game Controller index %i: Could not be initialized\n", m_joyindex);
			printf("Please, generate Xbox360 compatible mapping using antimicro or Steam big mode application\n");
			printf("and after set, the SDL controller variable before you launch the executable, i.e:\n");
			printf("export SDL_GAMECONTROLLERCONFIG=\"[the string you received from controllermap]\"\n");
			/* Need this so python args can return empty lists */
			joy_error = true;
		}

		if (!joy_error) {
			m_private->m_gamecontroller = SDL_GameControllerOpen(m_joyindex);
			if (!m_private->m_gamecontroller) {
				joy_error = true;
			}
		}

		if (!joy_error) {
			SDL_GameControllerEventState(SDL_ENABLE);
			printf("\nGame Controller (%s) with index %i: Initialized", GetName(), m_joyindex);

			/* A Game Controller has:
			 *
			 * 6 axis availables:	   AXIS_LEFTSTICK_X, AXIS_LEFTSTICK_Y,
			 * (in order from 0 to 5)  AXIS_RIGHTSTICK_X, AXIS_RIGHTSTICK_Y,
			 *						   AXIS_TRIGGERLEFT and AXIS_TRIGGERRIGHT.
			 *
			 * 15 buttons availables:  BUTTON_A, BUTTON_B, BUTTON_X, BUTTON_Y,
			 * (in order from 0 to 14) BUTTON_BACK, BUTTON_GUIDE, BUTTON_START,
			 *						   BUTTON_LEFTSTICK, BUTTON_RIGHTSTICK,
			 *						   BUTTON_LEFTSHOULDER, BUTTON_RIGHTSHOULDER,
			 *						   BUTTON_DPAD_UP, BUTTON_DPAD_DOWN,
			 *						   BUTTON_DPAD_LEFT and BUTTON_DPAD_RIGHT.
			 */
			m_axismax = SDL_CONTROLLER_AXIS_MAX;
			m_buttonmax = SDL_CONTROLLER_BUTTON_MAX;
		}

		/* Haptic configuration */
		if (!joy_error && !SDL_CHECK(SDL_HapticOpen)) {
			m_private->m_haptic = SDL_HapticOpen(m_joyindex);
			if (!m_private->m_haptic) {
				printf("Game Controller (%s) with index %i: Has not force feedback (vibration) available\n", GetName(), m_joyindex);
			}
		}
	}
#endif /* WITH_SDL */

	if (joy_error) {
		m_axismax = m_buttonmax = 0;
		return false;
	}
	else {
		m_isinit = true;
		return true;
	}
}


void SCA_Joystick::DestroyJoystickDevice(void)
{
#ifdef WITH_SDL
	if (m_isinit) {
		if (SDL_CHECK(SDL_GameControllerClose)) {
			if (m_private->m_haptic && SDL_CHECK(SDL_HapticClose)) {
				SDL_HapticClose(m_private->m_haptic);
				m_private->m_haptic = NULL;
			}

			printf("Game Controller (%s) with index %i: Closed\n", GetName(), m_joyindex);
			SDL_GameControllerClose(m_private->m_gamecontroller);
			m_private->m_gamecontroller = NULL;
		}
		m_isinit = false;
	}
#endif /* WITH_SDL */
}

int SCA_Joystick::Connected(void)
{
#ifdef WITH_SDL
	if (m_isinit &&
		(SDL_CHECK(SDL_GameControllerGetAttached) &&
		SDL_GameControllerGetAttached(m_private->m_gamecontroller)))
	{
		return 1;
	}
#endif
	return 0;
}

int SCA_Joystick::pGetAxis(int axisnum, int udlr)
{
#ifdef WITH_SDL
	return m_axis_array[(axisnum*2)+udlr];
#endif
	return 0;
}

int SCA_Joystick::pAxisTest(int axisnum)
{
#ifdef WITH_SDL
	/* Use ints instead of shorts here to avoid problems when we get -32768.
	 * When we take the negative of that later, we should get 32768, which is greater
	 * than what a short can hold. In other words, abs(MIN_SHORT) > MAX_SHRT. */
	int i1 = m_axis_array[(axisnum * 2)];
	int i2 = m_axis_array[(axisnum * 2) + 1];
	
	/* long winded way to do:
	 * return max_ff(absf(i1), absf(i2))
	 * ...avoid abs from math.h */
	if (i1 < 0) i1 = -i1;
	if (i2 < 0) i2 = -i2;
	if (i1 <i2) return i2;
	else        return i1;
#else /* WITH_SDL */
	return 0;
#endif /* WITH_SDL */
}

const char *SCA_Joystick::GetName()
{
#ifdef WITH_SDL
	return (SDL_CHECK(SDL_GameControllerName)) ? SDL_GameControllerName(m_private->m_gamecontroller) : "";
#else /* WITH_SDL */
	return "";
#endif /* WITH_SDL */
}

bool SCA_Joystick::GetJoystickUpdateStatus()
{
#ifdef WITH_SDL
	return m_joystickupdatestatus;
#else
	return false;
#endif
}

void SCA_Joystick::SetJoystickUpdateStatus(bool status)
{
#ifdef WITH_SDL
	m_joystickupdatestatus = status;
#else
	return;
#endif
}

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
 ** Contributor(s): youle, lordloki
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Device/DEV_JoystickVibration.cpp
 *  \ingroup device
 */

#include "DEV_Joystick.h"
#include "DEV_JoystickPrivate.h"
#include "DEV_JoystickDefines.h"

#ifdef _MSC_VER
#  include <cstdio> /* printf */
#endif
#include <memory> // We have to include that on Windows to make memset available

bool DEV_Joystick::RumblePlay(unsigned short mode, float strength[2], unsigned int duration)
{
#ifdef WITH_SDL
	unsigned int effects;
	bool run_by_effect = false;
	bool effects_issue = false;

	if (m_private->m_haptic == NULL) {
		return false;
	}

	if (m_private->m_hapticeffect_status == JOYHAPTIC_STOPPED) {
		memset(&m_private->m_hapticeffect, 0, sizeof(SDL_HapticEffect)); // 0 is safe default
	}

	// Checking supported effects
	effects = SDL_HapticQuery(m_private->m_haptic);

	if ((effects & SDL_HAPTIC_LEFTRIGHT) && m_private->m_hapticeffect_status != JOYHAPTIC_UPDATING_RUMBLE) {
		if (m_private->m_hapticeffect_status != JOYHAPTIC_UPDATING_EFFECT) {
			m_private->m_hapticeffect.type = SDL_HAPTIC_LEFTRIGHT;
		}

		m_private->m_hapticeffect.leftright.length = duration;

		switch (mode) {
			case JOYMOTORS_BOTH_EQUAL:
			{
				m_private->m_hapticeffect.leftright.large_magnitude = (unsigned int) (strength[0] * 0x7FFF);
				m_private->m_hapticeffect.leftright.small_magnitude = (unsigned int) (strength[0] * 0x7FFF);
				break;
			}
			case JOYMOTORS_BOTH_NOTEQUAL:
			{
				m_private->m_hapticeffect.leftright.large_magnitude = (unsigned int) (strength[0] * 0x7FFF);
				m_private->m_hapticeffect.leftright.small_magnitude = (unsigned int) (strength[1] * 0x7FFF);
				break;
			}
			case JOYMOTORS_ONLY_LARGE:
			{
				m_private->m_hapticeffect.leftright.large_magnitude = (unsigned int) (strength[0] * 0x7FFF);
				m_private->m_hapticeffect.leftright.small_magnitude = 0;
				break;
			}
			case JOYMOTORS_ONLY_SMALL:
			{
				m_private->m_hapticeffect.leftright.large_magnitude = 0;
				m_private->m_hapticeffect.leftright.small_magnitude = (unsigned int) (strength[1] * 0x7FFF);
				break;
			}
			default:
				printf ("Not available vibration mode found\n");
				break;
		}
		run_by_effect = true;

	}
	// Some Game Controllers only supports large/small magnitude motors using a custom effect
	else if ((effects & SDL_HAPTIC_CUSTOM) && m_private->m_hapticeffect_status != JOYHAPTIC_UPDATING_RUMBLE) {

		unsigned int data[2]; // data = channels * samples
		data[0] = (unsigned int) (strength[0] * 0x7FFF);
		data[1] = (unsigned int) (strength[1] * 0x7FFF);

		if (m_private->m_hapticeffect_status != JOYHAPTIC_UPDATING_EFFECT) {
			m_private->m_hapticeffect.type = SDL_HAPTIC_CUSTOM;
		}
		m_private->m_hapticeffect.custom.length = duration;
		m_private->m_hapticeffect.custom.channels = 2;
		m_private->m_hapticeffect.custom.period = 1;
		m_private->m_hapticeffect.custom.samples = 1;
		m_private->m_hapticeffect.custom.data = data;

		run_by_effect = true;
	}

	if (run_by_effect) {
		bool new_effect = true;

		if (m_private->m_hapticeffect_status == JOYHAPTIC_UPDATING_EFFECT) {
			if (SDL_HapticUpdateEffect(m_private->m_haptic, m_private->m_hapticeffect_id, &m_private->m_hapticeffect) == 0) {
				m_private->m_hapticeffect_status == JOYHAPTIC_PLAYING_EFFECT;
				new_effect = false;
			}
			else {
				SDL_HapticDestroyEffect(m_private->m_haptic, m_private->m_hapticeffect_id);
				m_private->m_hapticeffect_id = -1;
			}
		}

		if (new_effect) {
			// Upload the effect
			m_private->m_hapticeffect_id = SDL_HapticNewEffect(m_private->m_haptic, &m_private->m_hapticeffect);
		}

		// Run the effect
		if (m_private->m_hapticeffect_id >= 0 &&
			SDL_HapticRunEffect(m_private->m_haptic, m_private->m_hapticeffect_id, 1) != -1) {
				m_private->m_hapticeffect_status = JOYHAPTIC_PLAYING_EFFECT;
		}
		else {
			effects_issue = true;
		}
	}
	
	if (effects_issue || m_private->m_hapticeffect_status == JOYHAPTIC_UPDATING_RUMBLE) {
		// Initialize simple rumble for both motors if effects are not supported
		if (m_private->m_hapticeffect_status != JOYHAPTIC_UPDATING_RUMBLE) {
			if (SDL_HapticRumbleInit(m_private->m_haptic) != 0) {
				m_private->m_hapticeffect_status = JOYHAPTIC_STOPPED;
				return false;
			}
		}

		// Play effect at strength for m_duration milliseconds
		if (SDL_HapticRumblePlay(m_private->m_haptic, strength[0], duration[0]) != 0) {
			m_private->m_hapticeffect_status = JOYHAPTIC_STOPPED;
			return false;
		}

		m_private->m_hapticeffect_status = JOYHAPTIC_PLAYING_RUMBLE;
	}

	return true;
#endif // WITH_SDL
	return false;
}

void DEV_Joystick::RumbleUpdate(unsigned short mode, float strength[2], unsigned int duration)
{
	if (m_private->m_hapticeffect_status == JOYHAPTIC_PLAYING_EFFECT) {
		m_private->m_hapticeffect_status == JOYHAPTIC_UPDATING_EFFECT;
	} 
	else if (m_private->m_hapticeffect_status == JOYHAPTIC_PLAYING_RUMBLE) {
		m_private->m_hapticeffect_status == JOYHAPTIC_UPDATING_RUMBLE;
	}

	if (DEV_Joystick::RumblePlay(mode, strength, duration)) {
		return true;
	}
	
	return false;
}

bool DEV_Joystick::RumbleStop()
{
	if (m_private->m_hapticeffect_status == JOYHAPTIC_PLAYING_EFFECT ||
		m_private->m_hapticeffect_status == JOYHAPTIC_UPDATING_EFFECT)
	{
		SDL_HapticStopEffect(m_private->m_haptic, m_private->m_hapticeffect_id);
		SDL_HapticDestroyEffect(m_private->m_haptic, m_private->m_hapticeffect_id);
		m_private->m_hapticeffect_status = JOYHAPTIC_STOPPED;
		return true;
	}
	else if (m_private->m_hapticeffect_id == JOYHAPTIC_PLAYING_RUMBLE ||
			 m_private->m_hapticeffect_id == JOYHAPTIC_UPDATING_RUMBLE)
	{
		SDL_HapticRumbleStop(m_private->m_haptic);
		m_private->m_hapticeffect_status = JOYHAPTIC_STOPPED;
		return true;
	}
	return false;
}

int DEV_Joystick::GetRumbleStatus()
{
	return m_private->m_hapticeffect_status;
}

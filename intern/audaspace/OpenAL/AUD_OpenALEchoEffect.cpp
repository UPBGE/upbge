/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This file is part of AudaSpace.
 *
 * Audaspace is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * AudaSpace is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Audaspace; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file audaspace/OpenAL/AUD_OpenALEchoEffect.cpp
 *  \ingroup audopenal
 */

#include "AUD_OpenALEchoEffect.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

#include <algorithm>

AUD_OpenALEchoEffect::AUD_OpenALEchoEffect()
{
	m_damping = AL_ECHO_DEFAULT_DAMPING;
	m_delay = AL_ECHO_DEFAULT_DELAY;
	m_feedback = AL_ECHO_DEFAULT_FEEDBACK;
	m_lr_delay = AL_ECHO_DEFAULT_LRDELAY;
	m_spread = AL_ECHO_DEFAULT_SPREAD;
}

void AUD_OpenALEchoEffect::applyParams(ALuint effect)
{
	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_ECHO);

	alEffectf(effect, AL_ECHO_DAMPING, m_damping);
	alEffectf(effect, AL_ECHO_DELAY, m_delay);
	alEffectf(effect, AL_ECHO_FEEDBACK, m_feedback);
	alEffectf(effect, AL_ECHO_LRDELAY, m_lr_delay);
	alEffectf(effect, AL_ECHO_SPREAD, m_spread);
}

float AUD_OpenALEchoEffect::getDamping() const
{
	return m_damping;
}

void AUD_OpenALEchoEffect::setDamping(float damping)
{
	m_damping = std::max(AL_ECHO_MIN_DAMPING, std::min(damping, AL_ECHO_MAX_DAMPING));
}

float AUD_OpenALEchoEffect::getDelay() const
{
	return m_delay;
}

void AUD_OpenALEchoEffect::setDelay(float delay)
{
	m_delay = std::max(AL_ECHO_MIN_DELAY, std::min(delay, AL_ECHO_MAX_DELAY));
}

float AUD_OpenALEchoEffect::getFeedback() const
{
	return m_feedback;
}

void AUD_OpenALEchoEffect::setFeedback(float feedback)
{
	m_feedback = std::max(AL_ECHO_MIN_FEEDBACK, std::min(feedback, AL_ECHO_MAX_FEEDBACK));
}

float AUD_OpenALEchoEffect::getLRDelay() const
{
	return m_lr_delay;
}

void AUD_OpenALEchoEffect::setLRDelay(float lr_delay)
{
	m_lr_delay = std::max(AL_ECHO_MIN_LRDELAY, std::min(lr_delay, AL_ECHO_MAX_LRDELAY));
}

float AUD_OpenALEchoEffect::getSpread() const
{
	return m_spread;
}

void AUD_OpenALEchoEffect::setSpread(float spread)
{
	m_spread = std::max(AL_ECHO_MIN_SPREAD, std::min(spread, AL_ECHO_MAX_SPREAD));
}


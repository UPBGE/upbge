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

/** \file audaspace/OpenAL/AUD_OpenALChorusEffect.cpp
 *  \ingroup audopenal
 */

#include "AUD_OpenALChorusEffect.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

AUD_OpenALChorusEffect::AUD_OpenALChorusEffect()
{
	m_waveform = AL_CHORUS_DEFAULT_WAVEFORM;
	m_phase = AL_CHORUS_DEFAULT_PHASE;
	m_rate = AL_CHORUS_DEFAULT_RATE;
	m_depth = AL_CHORUS_DEFAULT_DEPTH;
	m_delay = AL_CHORUS_DEFAULT_DELAY;
	m_feedback = AL_CHORUS_DEFAULT_FEEDBACK;
}

void AUD_OpenALChorusEffect::applyParams(ALuint effect)
{
	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_CHORUS);

	alEffectf(effect, AL_CHORUS_WAVEFORM, m_waveform);
	alEffectf(effect, AL_CHORUS_PHASE, m_phase);
	alEffectf(effect, AL_CHORUS_RATE, m_rate);
	alEffectf(effect, AL_CHORUS_DEPTH, m_depth);
	alEffectf(effect, AL_CHORUS_DELAY, m_delay);
	alEffectf(effect, AL_CHORUS_FEEDBACK, m_feedback);
}

unsigned int AUD_OpenALChorusEffect::getWavefrom() const
{
	return m_wavefrom;
}

void AUD_OpenALChorusEffect::setWavefrom(unsigned int wavefrom)
{
	m_wavefrom = std::max(AL_CHORUS_MIN_WAVEFORM, std::min(wavefrom, AL_CHORUS_MAX_WAVEFORM));
}

int AUD_OpenALChorusEffect::getPhase() const
{
	return m_phase;
}

void AUD_OpenALChorusEffect::setPhase(int phase)
{
	m_phase = std::max(AL_CHORUS_MIN_PHASE, std::min(phase, AL_CHORUS_MAX_PHASE));
}

float AUD_OpenALChorusEffect::getRate() const
{
	return m_rate;
}

void AUD_OpenALChorusEffect::setRate(float rate)
{
	m_rate = std::max(AL_CHORUS_MIN_RATE, std::min(rate, AL_CHORUS_MAX_RATE));
}

float AUD_OpenALChorusEffect::getDepth() const
{
	return m_depth;
}

void AUD_OpenALChorusEffect::setDepth(float depth)
{
	m_depth = std::max(AL_CHORUS_MIN_DEPTH, std::min(depth, AL_CHORUS_MAX_DEPTH));
}

float AUD_OpenALChorusEffect::getDelay() const
{
	return m_delay;
}

void AUD_OpenALChorusEffect::setDelay(float delay)
{
	m_delay = std::max(AL_CHORUS_MIN_DELAY, std::min(delay, AL_CHORUS_MAX_DELAY));
}

float AUD_OpenALChorusEffect::getFeedback() const
{
	return m_feedback;
}

void AUD_OpenALChorusEffect::setFeedback(float feedback)
{
	m_feedback = std::max(AL_CHORUS_MIN_FEEDBACK, std::min(feedback, AL_CHORUS_MAX_FEEDBACK));
}




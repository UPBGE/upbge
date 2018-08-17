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

/** \file audaspace/OpenAL/AUD_OpenALFlangerEffect.cpp
 *  \ingroup audopenal
 */

#include "AUD_OpenALFlangerEffect.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

#include <algorithm>

AUD_OpenALFlangerEffect::AUD_OpenALFlangerEffect()
{
	m_waveform = AL_FLANGER_DEFAULT_WAVEFORM;
	m_phase = AL_FLANGER_DEFAULT_PHASE;
	m_rate = AL_FLANGER_DEFAULT_RATE;
	m_depth = AL_FLANGER_DEFAULT_DEPTH;
	m_feedback = AL_FLANGER_DEFAULT_FEEDBACK;
	m_delay = AL_FLANGER_DEFAULT_DELAY;
}

void AUD_OpenALFlangerEffect::applyParams(ALuint effect)
{
	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_FLANGER);

	alEffectf(effect, AL_FLANGER_WAVEFORM, m_waveform);
	alEffectf(effect, AL_FLANGER_PHASE, m_phase);
	alEffectf(effect, AL_FLANGER_RATE, m_rate);
	alEffectf(effect, AL_FLANGER_DEPTH, m_depth);
	alEffectf(effect, AL_FLANGER_FEEDBACK, m_feedback);
	alEffectf(effect, AL_FLANGER_DELAY, m_delay);
}

int AUD_OpenALFlangerEffect::getWaveform() const
{
	return m_waveform;
}

void AUD_OpenALFlangerEffect::setWaveform(int waveform)
{
	m_waveform = std::max(AL_FLANGER_MIN_WAVEFORM, std::min(waveform, AL_FLANGER_MAX_WAVEFORM));
}

int AUD_OpenALFlangerEffect::getPhase() const
{
	return m_phase;
}

void AUD_OpenALFlangerEffect::setPhase(int phase)
{
	m_phase = std::max(AL_FLANGER_MIN_PHASE, std::min(phase, AL_FLANGER_MAX_PHASE));
}

float AUD_OpenALFlangerEffect::getRate() const
{
	return m_rate;
}

void AUD_OpenALFlangerEffect::setRate(float rate)
{
	m_rate = std::max(AL_FLANGER_MIN_RATE, std::min(rate, AL_FLANGER_MAX_RATE));
}

float AUD_OpenALFlangerEffect::getDepth() const
{
	return m_depth;
}

void AUD_OpenALFlangerEffect::setDepth(float depth)
{
	m_depth = std::max(AL_FLANGER_MIN_DEPTH, std::min(depth, AL_FLANGER_MAX_DEPTH));
}


float AUD_OpenALFlangerEffect::getFeedback() const
{
	return m_feedback;
}

void AUD_OpenALFlangerEffect::setFeedback(float feedback)
{
	m_feedback = std::max(AL_FLANGER_MIN_FEEDBACK, std::min(feedback, AL_FLANGER_MAX_FEEDBACK));
}

float AUD_OpenALFlangerEffect::getDelay() const
{
	return m_delay;
}

void AUD_OpenALFlangerEffect::setDelay(float delay)
{
	m_delay = std::max(AL_FLANGER_MIN_DELAY, std::min(delay, AL_FLANGER_MAX_DELAY));
}


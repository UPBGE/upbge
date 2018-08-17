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

/** \file audaspace/OpenAL/AUD_OpenALAutoWahEffect.cpp
 *  \ingroup audopenal
 */

#include "AUD_OpenALAutoWahEffect.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

AUD_OpenALAutoWahEffect::AUD_OpenALAutoWahEffect()
{
	m_attack_time = AL_AUTOWAH_DEFAULT_ATTACK_TIME;
	m_release_time = AL_AUTOWAH_DEFAULT_RELEASE_TIME;
	m_resonance = AL_AUTOWAH_DEFAULT_RESONANCE;
	m_peak_gain = AL_AUTOWAH_DEFAULT_PEAK_GAIN;
}

void AUD_OpenALAutoWahEffect::applyParams(ALuint effect)
{
	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_AUTOWAH);

	alEffectf(effect, AL_AUTOWAH_ATTACK_TIME, m_attack_time);
	alEffectf(effect, AL_AUTOWAH_DEFAULT_RELEASE_TIME, m_release_time);
	alEffectf(effect, AL_AUTOWAH_RESONANCE, m_resonance);
	alEffectf(effect, AL_AUTOWAH_PEAK_GAIN, m_peak_gain);
}

float AUD_OpenALAutoWahEffect::getAttackTime() const
{
	return m_attack_time;
}

void AUD_OpenALAutoWahEffect::setAttackTime(float attack_time)
{
	m_attack_time = std::max(AL_AUTOWAH_MIN_ATTACK_TIME, std::min(attack_time, AL_AUTOWAH_MAX_ATTACK_TIME));
}

float AUD_OpenALAutoWahEffect::getReleaseTime() const
{
	return m_release_time;
}

void AUD_OpenALAutoWahEffect::setReleaseTime(float release_time)
{
	m_release_time = std::max(AL_AUTOWAH_MIN_RELEASE_TIME, std::min(release_time, AL_AUTOWAH_MAX_RELEASE_TIME));
}


float AUD_OpenALAutoWahEffect::getResonance() const
{
	return m_resonance;
}

void AUD_OpenALAutoWahEffect::setResonance(float resonance)
{
	m_resonance = std::max(AL_AUTOWAH_MIN_RESONANCE, std::min(resonance, AL_AUTOWAH_MAX_RESONANCE));
}

float AUD_OpenALAutoWahEffect::getPeakGain() const
{
	return m_peak_gain;
}

void AUD_OpenALAutoWahEffect::setPeakGain(float peak_gain)
{
	m_peak_gain = std::max(AL_AUTOWAH_MIN_PEAK_GAIN, std::min(peak_gain, AL_AUTOWAH_MAX_PEAK_GAIN));
}


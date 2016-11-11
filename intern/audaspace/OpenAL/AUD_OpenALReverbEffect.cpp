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

/** \file audaspace/OpenAL/AUD_OpenALReverbEffect.cpp
 *  \ingroup audopenal
 */

#include "AUD_OpenALReverbEffect.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

void AUD_OpenALReverbEffect::applyParams(ALuint effect)
{
	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);

	alEffectf(effect, AL_REVERB_DENSITY, m_density);
	alEffectf(effect, AL_REVERB_DIFFUSION, m_diffusion);
	alEffectf(effect, AL_REVERB_GAIN, m_gain);
	alEffectf(effect, AL_REVERB_GAINHF, m_gain_hf);
	alEffectf(effect, AL_REVERB_DECAY_TIME, m_decay_time);
	alEffectf(effect, AL_REVERB_DECAY_HFRATIO, m_decay_hf_ratio);
	alEffectf(effect, AL_REVERB_REFLECTIONS_GAIN, m_reflections_gain);
	alEffectf(effect, AL_REVERB_REFLECTIONS_DELAY, m_reflections_delay);
	alEffectf(effect, AL_REVERB_LATE_REVERB_GAIN, m_late_reverb_gain);
	alEffectf(effect, AL_REVERB_LATE_REVERB_DELAY, m_late_reverb_delay);
	alEffectf(effect, AL_REVERB_AIR_ABSORPTION_GAINHF, m_air_absorption_gain_hf);
	alEffectf(effect, AL_REVERB_ROOM_ROLLOFF_FACTOR, m_room_rolloff_factor);
	alEffecti(effect, AL_REVERB_DECAY_HFLIMIT, m_decay_limit_hf);

}

float AUD_OpenALReverbEffect::getGain() const
{
	return m_gain;
}

void AUD_OpenALReverbEffect::setGain(float gain)
{
	m_gain = gain;
}

float AUD_OpenALReverbEffect::getGainHF() const
{
	return m_gain_hf;
}

void AUD_OpenALReverbEffect::setGainHF(float gain_hf)
{
	m_gain_hf = gain_hf;
}

float AUD_OpenALReverbEffect::getDecayTime() const
{
	return m_decay_time;
}

void AUD_OpenALReverbEffect::setDecayTime(float decayTime)
{
	m_decay_time = decayTime;
}

float AUD_OpenALReverbEffect::getDecayHFRatio() const
{
	return m_decay_hf_ratio;
}

void AUD_OpenALReverbEffect::setDecayHFRatio(float decay_hf_ratio)
{
	m_decay_hf_ratio = decay_hf_ratio;
}

float AUD_OpenALReverbEffect::getReflectionsGain() const
{
	return m_reflections_gain;
}

void AUD_OpenALReverbEffect::setReflectionsGain(float reflections_gain)
{
	m_reflections_gain = reflections_gain;
}

float AUD_OpenALReverbEffect::getReflectionsDelay() const
{
	return m_reflections_delay;
}

void AUD_OpenALReverbEffect::setReflectionsDelay(float reflections_delay)
{
	m_reflections_delay = reflections_delay;
}

float AUD_OpenALReverbEffect::getLateReverbGain() const
{
	return m_late_reverb_gain;
}

void AUD_OpenALReverbEffect::setLateReverbGain(float late_reverb_gain)
{
	m_late_reverb_gain = late_reverb_gain;
}

float AUD_OpenALReverbEffect::getLateReverbDelay() const
{
	return m_late_reverb_delay;
}

void AUD_OpenALReverbEffect::setLateReverbDelay(float late_reverb_delay)
{
	m_late_reverb_delay = late_reverb_delay;
}

float AUD_OpenALReverbEffect::getAirAbsorptionGainHF() const
{
	return m_air_absorption_gain_hf;
}

void AUD_OpenALReverbEffect::setAirAbsorptionGainHF(float air_absorption_gain_hf)
{
	m_air_absorption_gain_hf = air_absorption_gain_hf;
}

float AUD_OpenALReverbEffect::getRoomRolloffFactor() const
{
	return m_room_rolloff_factor;
}

void AUD_OpenALReverbEffect::setRoomRolloffFactor(float room_rolloff_factor)
{
	m_room_rolloff_factor = room_rolloff_factor;
}

int AUD_OpenALReverbEffect::getDecayLimitHF() const
{
	return m_decay_limit_hf;
}

void AUD_OpenALReverbEffect::setDecayLimitHF(int decay_limit_hf)
{
	m_decay_limit_hf = decay_limit_hf;
}

float AUD_OpenALReverbEffect::getDensity() const
{
	return m_density;
}

void AUD_OpenALReverbEffect::setDensity(float density)
{
	m_density = density;
}

float AUD_OpenALReverbEffect::getDiffusion() const
{
	return m_diffusion;
}

void AUD_OpenALReverbEffect::setDiffusion(float diffusion)
{
	m_diffusion = diffusion;
}

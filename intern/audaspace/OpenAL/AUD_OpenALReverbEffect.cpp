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

#include <algorithm>

AUD_OpenALReverbEffect::AUD_OpenALReverbEffect()
{
	m_density = AL_REVERB_DEFAULT_DENSITY;
	m_diffusion = AL_REVERB_DEFAULT_DIFFUSION;
	m_gain = AL_REVERB_DEFAULT_GAIN;
	m_gain_hf = AL_REVERB_DEFAULT_GAINHF;
	m_decay_time = AL_REVERB_DEFAULT_DECAY_TIME;
	m_decay_hf_ratio = AL_REVERB_DEFAULT_DECAY_HFRATIO;
	m_reflections_gain = AL_REVERB_DEFAULT_REFLECTIONS_GAIN;
	m_reflections_delay = AL_REVERB_DEFAULT_REFLECTIONS_DELAY;
	m_late_reverb_delay = AL_REVERB_DEFAULT_LATE_REVERB_DELAY;
	m_late_reverb_gain = AL_REVERB_DEFAULT_LATE_REVERB_GAIN;
	m_air_absorption_gain_hf = AL_REVERB_DEFAULT_AIR_ABSORPTION_GAINHF;
	m_room_rolloff_factor = AL_REVERB_DEFAULT_ROOM_ROLLOFF_FACTOR;
	m_decay_limit_hf = AL_REVERB_DEFAULT_DECAY_HFLIMIT;
}

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
	m_gain = std::max(AL_REVERB_MIN_GAIN, std::min(gain, AL_REVERB_MAX_GAIN));
}

float AUD_OpenALReverbEffect::getGainHF() const
{
	return m_gain_hf;
}

void AUD_OpenALReverbEffect::setGainHF(float gain_hf)
{
	m_gain_hf = std::max(AL_REVERB_MIN_GAINHF, std::min(gain_hf, AL_REVERB_MAX_GAINHF));
}

float AUD_OpenALReverbEffect::getDecayTime() const
{
	return m_decay_time;
}

void AUD_OpenALReverbEffect::setDecayTime(float decay_time)
{
	m_decay_time = std::max(AL_REVERB_MIN_DECAY_TIME, std::min(decay_time, AL_REVERB_MAX_DECAY_TIME));
}

float AUD_OpenALReverbEffect::getDecayHFRatio() const
{
	return m_decay_hf_ratio;
}

void AUD_OpenALReverbEffect::setDecayHFRatio(float decay_hf_ratio)
{
	m_decay_hf_ratio = std::max(AL_REVERB_MIN_DECAY_HFRATIO, std::min(decay_hf_ratio, AL_REVERB_MAX_DECAY_HFRATIO));
}

float AUD_OpenALReverbEffect::getReflectionsGain() const
{
	return m_reflections_gain;
}

void AUD_OpenALReverbEffect::setReflectionsGain(float reflections_gain)
{
	m_reflections_gain = std::max(AL_REVERB_MIN_REFLECTIONS_GAIN, std::min(reflections_gain, AL_REVERB_MAX_REFLECTIONS_GAIN));
}

float AUD_OpenALReverbEffect::getReflectionsDelay() const
{
	return m_reflections_delay;
}

void AUD_OpenALReverbEffect::setReflectionsDelay(float reflections_delay)
{
	m_reflections_delay = std::max(AL_REVERB_MIN_REFLECTIONS_DELAY, std::min(reflections_delay, AL_REVERB_MAX_REFLECTIONS_DELAY));
}

float AUD_OpenALReverbEffect::getLateReverbGain() const
{
	return m_late_reverb_gain;
}

void AUD_OpenALReverbEffect::setLateReverbGain(float late_reverb_gain)
{
	m_late_reverb_gain = std::max(AL_REVERB_MIN_LATE_REVERB_GAIN, std::min(late_reverb_gain, AL_REVERB_MAX_LATE_REVERB_GAIN));
}

float AUD_OpenALReverbEffect::getLateReverbDelay() const
{
	return m_late_reverb_delay;
}

void AUD_OpenALReverbEffect::setLateReverbDelay(float late_reverb_delay)
{
	m_late_reverb_delay = std::max(AL_REVERB_MIN_LATE_REVERB_DELAY, std::min(late_reverb_delay, AL_REVERB_MAX_LATE_REVERB_DELAY));
}

float AUD_OpenALReverbEffect::getAirAbsorptionGainHF() const
{
	return m_air_absorption_gain_hf;
}

void AUD_OpenALReverbEffect::setAirAbsorptionGainHF(float air_absorption_gain_hf)
{
	m_air_absorption_gain_hf = std::max(AL_REVERB_MIN_AIR_ABSORPTION_GAINHF, std::min(air_absorption_gain_hf, AL_REVERB_MAX_AIR_ABSORPTION_GAINHF));
}

float AUD_OpenALReverbEffect::getRoomRolloffFactor() const
{
	return m_room_rolloff_factor;
}

void AUD_OpenALReverbEffect::setRoomRolloffFactor(float room_rolloff_factor)
{
	m_room_rolloff_factor = std::max(AL_REVERB_MIN_ROOM_ROLLOFF_FACTOR, std::min(room_rolloff_factor, AL_REVERB_MAX_ROOM_ROLLOFF_FACTOR));
}

int AUD_OpenALReverbEffect::getDecayLimitHF() const
{
	return m_decay_limit_hf;
}

void AUD_OpenALReverbEffect::setDecayLimitHF(int decay_limit_hf)
{
	m_decay_limit_hf = std::max(AL_REVERB_MIN_DECAY_HFLIMIT, std::min(decay_limit_hf, AL_REVERB_MAX_DECAY_HFLIMIT));
}

float AUD_OpenALReverbEffect::getDensity() const
{
	return m_density;
}

void AUD_OpenALReverbEffect::setDensity(float density)
{
	m_density = std::max(AL_REVERB_MIN_DENSITY, std::min(density, AL_REVERB_MAX_DENSITY));
}

float AUD_OpenALReverbEffect::getDiffusion() const
{
	return m_diffusion;
}

void AUD_OpenALReverbEffect::setDiffusion(float diffusion)
{
	m_diffusion = std::max(AL_REVERB_MIN_DIFFUSION, std::min(diffusion, AL_REVERB_MAX_DIFFUSION));
}


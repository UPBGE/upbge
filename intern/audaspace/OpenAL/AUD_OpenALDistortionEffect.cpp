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

/** \file audaspace/OpenAL/AUD_OpenALDistortionEffect.cpp
 *  \ingroup audopenal
 */

#include "AUD_OpenALDistortionEffect.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

#include <algorithm>

AUD_OpenALDistortionEffect::AUD_OpenALDistortionEffect()
{
	m_edge = AL_DISTORTION_DEFAULT_EDGE;
	m_gain = AL_DISTORTION_DEFAULT_GAIN;
	m_lowpass_cutoff = AL_DISTORTION_DEFAULT_LOWPASS_CUTOFF;
	m_eq_center = AL_DISTORTION_DEFAULT_EQCENTER;
	m_eq_bandwidth = AL_DISTORTION_DEFAULT_EQBANDWIDTH;
}

void AUD_OpenALDistortionEffect::applyParams(ALuint effect)
{
	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_DISTORTION);

	alEffectf(effect, AL_DISTORTION_EDGE, m_edge);
	alEffectf(effect, AL_DISTORTION_GAIN, m_gain);
	alEffectf(effect, AL_DISTORTION_LOWPASS_CUTOFF, m_lowpass_cutoff);
	alEffectf(effect, AL_DISTORTION_EQCENTER, m_eq_center);
	alEffectf(effect, AL_DISTORTION_EQBANDWIDTH, m_eq_bandwidth);
}

float AUD_OpenALDistortionEffect::getEdge() const
{
	return m_edge;
}

void AUD_OpenALDistortionEffect::setEdge(float edge)
{
	m_edge = std::max(AL_DISTORTION_MIN_EDGE, std::min(edge, AL_DISTORTION_MAX_EDGE));
}

float AUD_OpenALDistortionEffect::getGain() const
{
	return m_gain;
}

void AUD_OpenALDistortionEffect::setGain(float gain)
{
	m_gain = std::max(AL_DISTORTION_MIN_GAIN, std::min(gain, AL_DISTORTION_MAX_GAIN));
}

float AUD_OpenALDistortionEffect::getLowpassCutoff() const
{
	return m_lowpass_cutoff;
}

void AUD_OpenALDistortionEffect::setLowpassCutoff(float lowpass_cutoff)
{
	m_lowpass_cutoff = std::max(AL_DISTORTION_MIN_LOWPASS_CUTOFF, std::min(lowpass_cutoff, AL_DISTORTION_MAX_LOWPASS_CUTOFF));
}

float AUD_OpenALDistortionEffect::getEqCenter() const
{
	return m_eq_center;
}

void AUD_OpenALDistortionEffect::setEqCenter(float eq_center)
{
	m_eq_center = std::max(AL_DISTORTION_MIN_EQCENTER, std::min(eq_center, AL_DISTORTION_MAX_EQCENTER));
}

float AUD_OpenALDistortionEffect::getEqBandwidth() const
{
	return m_eq_bandwidth;
}

void AUD_OpenALDistortionEffect::setEqBandwidth(float eq_bandwidth)
{
	m_eq_bandwidth = std::max(AL_DISTORTION_MIN_EQBANDWIDTH, std::min(eq_bandwidth, AL_DISTORTION_MAX_EQBANDWIDTH));
}

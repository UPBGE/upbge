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

/** \file audaspace/OpenAL/AUD_OpenALCompressorEffect.cpp
 *  \ingroup audopenal
 */

#include "AUD_OpenALCompressorEffect.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

AUD_OpenALCompressorEffect::AUD_OpenALCompressorEffect()
{
	m_compressor_onoff = AL_COMPRESSOR_DEFAULT_ONOFF;
}

void AUD_OpenALCompressorEffect::applyParams(ALuint effect)
{
	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_COMPRESSOR);

	alEffectf(effect, AL_COMPRESSOR_ONOFF, m_compressor_onoff);
}

unsigned int AUD_OpenALCompressorEffect::getCompressor() const
{
	return m_compressor;
}

void AUD_OpenALCompressorEffect::setCompressor(unsigned int compressor_onoff)
{
	m_wavefrom = std::max(AL_COMPRESSOR_MIN_ONOFF, std::min(compressor_onoff, AL_COMPRESSOR_MAX_ONOFF));
}

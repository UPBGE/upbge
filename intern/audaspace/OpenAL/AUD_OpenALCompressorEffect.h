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

/** \file audaspace/OpenAL/AUD_OpenALCompressorEffect.h
 *  \ingroup audopenal
 */

#ifndef __AUD_OPENALCOMPRESSOREFFECT_H__
#define __AUD_OPENALCOMPRESSOREFFECT_H__

#include "AUD_IOpenALEffectParams.h"

class AUD_OpenALCompressorEffect : public AUD_IOpenALEffectParams
{
public:
	AUD_OpenALCompressorEffect();

	void applyParams(ALuint effect);

	int getCompressor() const;
	void setCompressor(int compressor_onoff);

private:
	int m_compressor_onoff;

};

#endif // AUD_OPENALCOMPRESSOREFFECT_H


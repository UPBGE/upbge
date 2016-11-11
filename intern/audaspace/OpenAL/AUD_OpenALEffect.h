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

/** \file audaspace/OpenAL/AUD_OpenALEffect.h
 *  \ingroup audopenal
 */

#ifndef __AUD_OPENALEFFECT_H__
#define __AUD_OPENALEFFECT_H__

#include "AUD_IOpenALEffectParams.h"

class AUD_OpenALEffect
{
public:
	void update();

	ALuint getSlot();

	AUD_OpenALEffect(AUD_IOpenALEffectParams *params);
	~AUD_OpenALEffect();
private:
	ALuint m_slot, m_effect_id;
	AUD_IOpenALEffectParams* m_effect_params;
};

#endif // AUD_OPENALEFFECT_H

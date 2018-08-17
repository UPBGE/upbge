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

/** \file audaspace/OpenAL/AUD_IOpenALEffectParams.h
 *  \ingroup audopenal
 */

#ifndef __AUD_OPENALEFFECTPARAMS_H__
#define __AUD_OPENALEFFECTPARAMS_H__

#include <AL/al.h>

#ifdef WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#endif

class AUD_IOpenALEffectParams
{
public:
	virtual void applyParams(ALuint effect) = 0;
};

#endif // AUD_OPENALEFFECTPARAMS_H

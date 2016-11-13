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

/** \file audaspace/OpenAL/AUD_OpenALEchoEffect.h
 *  \ingroup audopenal
 */

#ifndef __AUD_OPENALECHOEFFECT_H__
#define __AUD_OPENALECHOEFFECT_H__

#include "AUD_IOpenALEffectParams.h"

class AUD_OpenALEchoEffect : public AUD_IOpenALEffectParams
{
public:
	AUD_OpenALEchoEffect();

	void applyParams(ALuint effect);

	float getDamping() const;
	void setDamping(float damping);
	float getDelay() const;
	void setDelay(float delay);
	float getFeedback() const;
	void setFeedback(float feedback);
	float getLRDelay() const;
	void setLRDelay(float lr_delay);
	float getSpread() const;
	void setSpread(float spread);

private:
	float m_damping, m_delay;
	float m_feedback, m_lr_delay;
	float m_spread;
};

#endif // AUD_OPENALECHOEFFECT_H

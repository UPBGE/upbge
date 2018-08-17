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

/** \file audaspace/OpenAL/AUD_OpenALDistortionEffect.h
 *  \ingroup audopenal
 */

#ifndef __AUD_OPENALDISTORTIONEFFECT_H__
#define __AUD_OPENALDISTORTIONEFFECT_H__

#include "AUD_IOpenALEffectParams.h"

class AUD_OpenALDistortionEffect : public AUD_IOpenALEffectParams
{
public:
	AUD_OpenALDistortionEffect();

	void applyParams(ALuint effect);

	float getEdge() const;
	void setEdge(float edge);
	float getGain() const;
	void setGain(float gain);
	float getLowpassCutoff() const;
	void setLowpassCutoff(float lowpass_cutoff);
	float getEqCenter() const;
	void setEqCenter(float eq_center);
	float getEqBandwidth() const;
	void setEqBandwidth(float eq_bandwidth);


private:
	float m_edge;
	float m_gain;
	float m_lowpass_cutoff;
	float m_eq_center;
	float m_eq_bandwidth;

};

#endif // AUD_OPENALDISTORTIONEFFECT_H


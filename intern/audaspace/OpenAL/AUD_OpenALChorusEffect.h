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

/** \file audaspace/OpenAL/AUD_OpenALChorusEffect.h
 *  \ingroup audopenal
 */

#ifndef __AUD_OPENALCHORUSEFFECT_H__
#define __AUD_OPENALCHORUSEFFECT_H__

#include "AUD_IOpenALEffectParams.h"

class AUD_OpenALChorusEffect : public AUD_IOpenALEffectParams
{
public:
	AUD_OpenALChorusEffect();

	void applyParams(ALuint effect);

	int getWaveform() const;
	void setWaveform(int waveform);
	int getPhase() const;
	void setPhase(int phase);
	float getRate() const;
	void setRate(float rate);
	float getDepth() const;
	void setDepth(float depth);
	float getFeedback() const;
	void setFeedback(float feedback);
	float getDelay() const;
	void setDelay(float delay);


private:
	int m_waveform;
	int m_phase;
	float m_rate;
	float m_depth;
	float m_delay;
	float m_feedback;
	
};

#endif // AUD_OPENALCHORUSEFFECT_H


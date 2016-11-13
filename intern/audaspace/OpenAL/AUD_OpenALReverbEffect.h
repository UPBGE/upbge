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

/** \file audaspace/OpenAL/AUD_OpenALReverbEffect.h
 *  \ingroup audopenal
 */

#ifndef __AUD_OPENALREVERBEFFECT_H__
#define __AUD_OPENALREVERBEFFECT_H__

#include "AUD_IOpenALEffectParams.h"

class AUD_OpenALReverbEffect : public AUD_IOpenALEffectParams
{
public:
	AUD_OpenALReverbEffect();

	void applyParams(ALuint effect);

	float getDensity() const;
	void setDensity(float density);
	float getDiffusion() const;
	void setDiffusion(float diffusion);
	float getGain() const;
	void setGain(float gain);
	float getGainHF() const;
	void setGainHF(float gain_hf);
	float getDecayTime() const;
	void setDecayTime(float decayTime);
	float getDecayHFRatio() const;
	void setDecayHFRatio(float decay_hf_ratio);
	float getReflectionsGain() const;
	void setReflectionsGain(float reflections_gain);
	float getReflectionsDelay() const;
	void setReflectionsDelay(float reflections_delay);
	float getLateReverbGain() const;
	void setLateReverbGain(float late_reverb_gain);
	float getLateReverbDelay() const;
	void setLateReverbDelay(float late_reverb_delay);
	float getAirAbsorptionGainHF() const;
	void setAirAbsorptionGainHF(float air_absorption_gain_hf);
	float getRoomRolloffFactor() const;
	void setRoomRolloffFactor(float room_rolloff_factor);
	int getDecayLimitHF() const;
	void setDecayLimitHF(int decay_limit_hf);

private:
	float m_density;
	float m_diffusion;
	float m_gain, m_gain_hf;
	float m_decay_time, m_decay_hf_ratio;
	float m_reflections_gain, m_reflections_delay;
	float m_late_reverb_gain, m_late_reverb_delay;
	float m_air_absorption_gain_hf;
	float m_room_rolloff_factor;
	int m_decay_limit_hf;
};

#endif // AUD_OPENALREVERBEFFECT_H

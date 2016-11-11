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

#include "AUD_OpenALEffect.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

AUD_OpenALEffect::AUD_OpenALEffect(AUD_IOpenALEffectParams* params)
{
	alGenEffects(1, &m_effect_id);
	alGenAuxiliaryEffectSlots(1, &m_slot);

	m_effect_params = params;
	if (m_effect_params) {
		m_effect_params->applyParams(m_effect_id);
	}

	alAuxiliaryEffectSloti(m_slot, AL_EFFECTSLOT_EFFECT, m_effect_id);
}

AUD_OpenALEffect::~AUD_OpenALEffect()
{
	if (m_slot) {
		alDeleteAuxiliaryEffectSlots(1, &m_slot);
	}
	if (m_effect_id) {
		alDeleteEffects(1, &m_effect_id);
	}
}

void AUD_OpenALEffect::update()
{
	if (m_effect_params) {
		m_effect_params->applyParams(m_effect_id);
	}
}

ALuint AUD_OpenALEffect::getSlot()
{
	return m_slot;
}

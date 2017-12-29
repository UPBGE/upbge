/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_WorldIpoController.cpp
 *  \ingroup ketsji
 */


#include "KX_WorldIpoController.h"
#include "KX_ScalarInterpolator.h"
#include "KX_WorldInfo.h"
#include "KX_Globals.h"
#include "KX_Scene.h"

#if defined(_WIN64)
typedef unsigned __int64 uint_ptr;
#else
typedef unsigned long uint_ptr;
#endif

bool KX_WorldIpoController::Update()
{
	if (!SG_Controller::Update()) {
		return false;
	}

	KX_WorldInfo *world = KX_GetActiveScene()->GetWorldInfo();

	if (m_modify_mist_start) {
		world->setMistStart(m_mist_start);
	}

	if (m_modify_mist_dist) {
		world->setMistDistance(m_mist_dist);
	}

	if (m_modify_mist_intensity) {
		world->setMistIntensity(m_mist_intensity);
	}

	if (m_modify_horizon_color) {
		world->setHorizonColor(mt::vec4(m_hori_rgb[0], m_hori_rgb[1], m_hori_rgb[2], 1.0f));
		world->setMistColor(m_hori_rgb);
	}

	if (m_modify_zenith_color) {
		world->setZenithColor(mt::vec4(m_zeni_rgb[0], m_zeni_rgb[1], m_zeni_rgb[2], 1.0f));
	}

	if (m_modify_ambient_color) {
		world->setAmbientColor(m_ambi_rgb);
	}

	return true;
}

SG_Controller*	KX_WorldIpoController::GetReplica(class SG_Node* destnode)
{
	KX_WorldIpoController* iporeplica = new KX_WorldIpoController(*this);
	// clear object that ipo acts on
	iporeplica->ClearNode();

	// dirty hack, ask Gino for a better solution in the ipo implementation
	// hacken en zagen, in what we call datahiding, not written for replication :(

	SG_IInterpolatorList oldlist = m_interpolators;
	iporeplica->m_interpolators.clear();

	SG_IInterpolatorList::iterator i;
	for (i = oldlist.begin(); !(i == oldlist.end()); ++i) {
		KX_ScalarInterpolator* copyipo = new KX_ScalarInterpolator(*((KX_ScalarInterpolator*)*i));
		iporeplica->AddInterpolator(copyipo);

		float* scaal = ((KX_ScalarInterpolator*)*i)->GetTarget();
		uint_ptr orgbase = (uint_ptr)this;
		uint_ptr orgloc = (uint_ptr)scaal;
		uint_ptr offset = orgloc-orgbase;
		uint_ptr newaddrbase = (uint_ptr)iporeplica + offset;
		float* blaptr = (float*) newaddrbase;
		copyipo->SetNewTarget((float*)blaptr);
	}
	
	return iporeplica;
}


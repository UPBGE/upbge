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

/** \file gameengine/Ketsji/KX_LightIpoSGController.cpp
 *  \ingroup ketsji
 */


#include "KX_LightIpoSGController.h"
#include "KX_ScalarInterpolator.h"
#include "KX_LightObject.h"
#include "RAS_ILightObject.h"

#if defined(_WIN64)
typedef unsigned __int64 uint_ptr;
#else
typedef unsigned long uint_ptr;
#endif

bool KX_LightIpoSGController::Update()
{
	if (!SG_Controller::Update()) {
		return false;
	}

	KX_LightObject *kxlight = (KX_LightObject *)m_node->GetSGClientObject();
	RAS_ILightObject *lightobj = kxlight->GetLightData();

	if (m_modify_energy) {
		lightobj->m_energy = m_energy;
	}

	if (m_modify_color) {
		lightobj->m_color[0] = m_col_rgb[0];
		lightobj->m_color[1] = m_col_rgb[1];
		lightobj->m_color[2] = m_col_rgb[2];
	}

	if (m_modify_dist) {
		lightobj->m_distance = m_dist;
	}

	return true;
}

SG_Controller*	KX_LightIpoSGController::GetReplica(class SG_Node* destnode)
{
	KX_LightIpoSGController* iporeplica = new KX_LightIpoSGController(*this);
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

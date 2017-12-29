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

/** \file gameengine/SceneGraph/SG_Controller.cpp
 *  \ingroup bgesg
 */

#include "SG_Controller.h"
#include "SG_Interpolator.h"

#include <stdint.h> // For intptr_t.

SG_Controller::SG_Controller()
	:m_modified(true),
	m_node(nullptr),
	m_ipotime(0.0)
{
}

SG_Controller::~SG_Controller()
{
	for (SG_Interpolator *interp : m_interpolators) {
		delete interp;
	}
}

void SG_Controller::ProcessReplica()
{
	// Clear object that ipo acts on.
	ClearNode();

	for (unsigned short i = 0, size = m_interpolators.size(); i < size; ++i) {
		SG_Interpolator *oldinterp = m_interpolators[i];
		SG_Interpolator *newinterp = new SG_Interpolator(*oldinterp);

		m_interpolators[i] = newinterp;

		const intptr_t orgbase = (intptr_t)oldinterp;
		const intptr_t orgloc = (intptr_t)oldinterp->GetTarget();
		const intptr_t offset = orgloc - orgbase;
		const intptr_t newaddrbase = ((intptr_t)this) + offset;

		newinterp->SetTarget((float *)newaddrbase);
	}
}

bool SG_Controller::Update()
{
	if (!m_modified) {
		return false;
	}

	m_modified = false;

	for (SG_Interpolator *interp : m_interpolators) {
		interp->Execute(m_ipotime);
	}

	return true;
}

void SG_Controller::SetNode(SG_Node *node)
{
	m_node = node;
}

void SG_Controller::ClearNode()
{
	m_node = nullptr;
}

void SG_Controller::SetSimulatedTime(double time)
{
	m_ipotime = time;
	m_modified = true;
}

void SG_Controller::SetOption(SG_Controller::SG_ControllerOption option, bool value)
{
}

void SG_Controller::AddInterpolator(SG_Interpolator *interp)
{
	m_interpolators.push_back(interp);
}

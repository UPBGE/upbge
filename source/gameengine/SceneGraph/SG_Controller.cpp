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
	m_time(0.0)
{
}

bool SG_Controller::Update(SG_Node *node)
{
	if (!m_modified) {
		return false;
	}

	m_modified = false;

	for (SG_Interpolator& interp : m_interpolators) {
		interp.Execute(m_time);
	}

	return true;
}

void SG_Controller::SetSimulatedTime(double time)
{
	m_time = time;
	m_modified = true;
}

void SG_Controller::SetOption(SG_Controller::SG_ControllerOption option, bool value)
{
}

void SG_Controller::AddInterpolator(const SG_Interpolator& interp)
{
	m_interpolators.push_back(interp);
}

bool SG_Controller::Empty() const
{
	return m_interpolators.empty();
}

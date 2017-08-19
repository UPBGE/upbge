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

/** \file gameengine/Ketsji/KX_OrientationInterpolator.cpp
 *  \ingroup ketsji
 */


#include "KX_OrientationInterpolator.h"
#include "KX_IScalarInterpolator.h"

void KX_OrientationInterpolator::Execute(float currentTime) const
{
	mt::vec3 eul(m_ipos[0]->GetValue(currentTime),
				   m_ipos[1]->GetValue(currentTime),
				   m_ipos[2]->GetValue(currentTime));
	float ci = cosf(eul[0]);
	float cj = cosf(eul[1]);
	float ch = cosf(eul[2]);
	float si = sinf(eul[0]);
	float sj = sinf(eul[1]);
	float sh = sinf(eul[2]);
	float cc = ci*ch; 
	float cs = ci*sh; 
	float sc = si*ch; 
	float ss = si*sh;

	m_target = mt::mat3(cj*ch, sj*sc-cs, sj*cc+ss,
	                  cj*sh, sj*ss+cc, sj*cs-sc,
	                  -sj,    cj*si,    cj*ci);
}

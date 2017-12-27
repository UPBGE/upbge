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

/** \file gameengine/Converter/BL_ScalarInterpolator.cpp
 *  \ingroup bgeconv
 */


#include "BL_ScalarInterpolator.h"

#include <cstring>

extern "C" {
#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "BKE_fcurve.h"
}

float BL_ScalarInterpolator::GetValue(float currentTime) const
{
	// XXX 2.4x IPO_GetFloatValue(m_blender_adt, m_channel, currentTime);
	return evaluate_fcurve(m_fcu, currentTime);
}

BL_InterpolatorList::BL_InterpolatorList(bAction *action)
	:m_action(action)
{
	if (action==nullptr)
		return;
	
	for (FCurve *fcu = (FCurve *)action->curves.first; fcu; fcu = fcu->next) {
		if (fcu->rna_path) {
			BL_ScalarInterpolator *new_ipo = new BL_ScalarInterpolator(fcu); 
			//assert(new_ipo);
			m_interpolators.push_back(new_ipo);
		}
	}
}

BL_InterpolatorList::~BL_InterpolatorList()
{
	for (BL_ScalarInterpolator *interp : m_interpolators) {
		delete interp;
	}
}

bAction *BL_InterpolatorList::GetAction() const
{
	return m_action;
}

BL_ScalarInterpolator *BL_InterpolatorList::GetScalarInterpolator(const char *rna_path, int array_index)
{
	for (BL_ScalarInterpolator *interp : m_interpolators) {
		FCurve *fcu= interp->GetFCurve();
		if (array_index==fcu->array_index && strcmp(rna_path, fcu->rna_path)==0)
			return interp;
	}
	return nullptr;
}


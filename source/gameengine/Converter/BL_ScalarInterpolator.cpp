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

extern "C" {
#  include "DNA_anim_types.h"
#  include "BKE_fcurve.h"
}

BL_ScalarInterpolator::BL_ScalarInterpolator(FCurve *fcu)
	:m_fcu(fcu)
{
}

float BL_ScalarInterpolator::GetValue(float currentTime) const
{
	return evaluate_fcurve(m_fcu, currentTime);
}

FCurve *BL_ScalarInterpolator::GetFCurve() const
{
	return m_fcu;
}

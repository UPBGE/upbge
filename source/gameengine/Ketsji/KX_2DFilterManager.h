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
 * Contributor(s): Ulysse Martin, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_2DFilterManager.h
*  \ingroup ketsji
*/

#ifndef __KX_2DFILTER_MANAGER_H__
#define __KX_2DFILTER_MANAGER_H__

#include "RAS_2DFilterManager.h"
#include "EXP_PyObjectPlus.h"

class KX_2DFilterManager : public RAS_2DFilterManager, public EXP_PyObjectPlus
{
	Py_Header(KX_2DFilterManager)
public:
	KX_2DFilterManager();
	virtual ~KX_2DFilterManager();

	virtual RAS_2DFilter *NewFilter(RAS_2DFilterData& filterData);

#ifdef WITH_PYTHON

	EXP_PYMETHOD_DOC(KX_2DFilterManager, getFilter);
	EXP_PYMETHOD_DOC(KX_2DFilterManager, addFilter);
	EXP_PYMETHOD_DOC(KX_2DFilterManager, removeFilter);
	EXP_PYMETHOD_DOC(KX_2DFilterManager, createOffScreen);

#endif  // WITH_PYTHON
};

#endif // __KX_2DFILTER_MANAGER_H__

//This class is a subclass of RAS_2DFilter, it contains only the pytohn proxy and it is created only in the KX_2DFilterManager class which is also a subclass of RAS_2DFilterManager.
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

/** \file KX_2DFilter.h
*  \ingroup ketsji
*/

#ifndef __KX_2DFILTER_H__
#define __KX_2DFILTER_H__

#include "RAS_2DFilter.h"
#include "BL_Shader.h"
#include "MT_Matrix4x4.h"

class KX_2DFilter : public RAS_2DFilter, public BL_Shader
{
	Py_Header
public:
	KX_2DFilter(RAS_2DFilterData& data, RAS_2DFilterManager *manager);
	virtual ~KX_2DFilter();

	virtual bool LinkProgram();

#ifdef WITH_PYTHON

	KX_PYMETHOD_DOC(KX_2DFilter, setTexture);

#endif
};

#endif  // __KX_2DFILTER_H__

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

/** \file gameengine/Ketsji/KX_2DFilter.cpp
*  \ingroup ketsji
*/

#include "KX_2DFilter.h"
#include "BL_Texture.h" // for BL_Texture::MaxUnits

KX_2DFilter::KX_2DFilter(RAS_2DFilterData& data, RAS_2DFilterManager *manager)
	:RAS_2DFilter(data, manager)
{
}

KX_2DFilter::~KX_2DFilter()
{
}

bool KX_2DFilter::LinkProgram()
{
	return RAS_2DFilter::LinkProgram();
}


PyTypeObject KX_2DFilter::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_2DFilter",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&BL_Shader::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_2DFilter::Methods[] = {
	KX_PYMETHODTABLE(KX_2DFilter, setTexture),
	{NULL, NULL} // Sentinel
};

PyAttributeDef KX_2DFilter::Attributes[] = {
	{NULL} // Sentinel
};


KX_PYMETHODDEF_DOC(KX_2DFilter, setTexture, "setTexture(name, bindcode, index(optional))")
{
	int bindcode = 0;
	int index = -1;
	const char *uniform;

	if (!PyArg_ParseTuple(args, "si|i:setTexture", &uniform, &bindcode, &index)) {
		return NULL;
	}
	
	int loc = GetUniformLocation(uniform);
	if (loc != -1) {
		if (index != -1) {
			if (index < 1 || index > 7) {
				PyErr_SetString(PyExc_TypeError, "setTexture(name, bindcode, index(optional)): KX_2DFilter. index out of range [1, 7]");
				return NULL;
			}
			SetUniformiv(loc, RAS_Uniform::UNI_INT, &index, (sizeof(int)), 1);
			m_textures[index] = bindcode;
		}
		else {
			/* We don't use m_textures[0] because if m_textures[0] was used and had the bindcode of an existing texture,
			and if a texture "tex" was not initialized with setTexture, uniform location of "tex" would be by default 0 (GLSL default),
			and the value of this uniform value would be the bindcode of the texture already bound at location 0 */
			for (int i = 1; i < 8; i++) {
				if (m_textures[i] == 0) {
					SetUniformiv(loc, RAS_Uniform::UNI_INT, &i, (sizeof(int)), 1);
					m_textures[i] = bindcode;
					break;
				}
			}
		}
	}
	else {
		PyErr_SetString(PyExc_TypeError, "setTexture(name, bindcode, index(optional)): KX_2DFilter. Invalid Texture Name");
		return NULL;
	}
	Py_RETURN_NONE;
}

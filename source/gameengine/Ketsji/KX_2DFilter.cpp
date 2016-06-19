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

/** \file gameengine/Ketsji/KX_2DFilter.cpp
*  \ingroup ketsji
*/

#include "KX_2DFilter.h"
#include "RAS_Texture.h" // for RAS_Texture::MaxUnits

KX_2DFilter::KX_2DFilter(RAS_2DFilterData& data)
	:RAS_2DFilter(data)
{
}

KX_2DFilter::~KX_2DFilter()
{
}

bool KX_2DFilter::LinkProgram()
{
	return RAS_2DFilter::LinkProgram();
}

#ifdef WITH_PYTHON

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


KX_PYMETHODDEF_DOC(KX_2DFilter, setTexture, "setTexture(index, bindCode, samplerName)")
{
	int index = 0;
	int bindCode = 0;
	char *samplerName = NULL;

	if (!PyArg_ParseTuple(args, "ii|s:setTexture", &index, &bindCode, &samplerName)) {
		return NULL;
	}
	if (index < 0 || index >= RAS_Texture::MaxUnits) {
		PyErr_SetString(PyExc_ValueError, "setTexture(index, bindCode, samplerName): KX_2DFilter, index out of range [0, 7]");
		return NULL;
	}

	if (samplerName) {
		int loc = GetUniformLocation(samplerName);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT, &index, (sizeof(int)), 1);
#else
			SetUniform(loc, index);
#endif
		}
	}

	m_textures[index] = bindCode;
	Py_RETURN_NONE;
}

#endif  // WITH_PYTHON

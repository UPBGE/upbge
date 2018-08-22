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

/** \file KX_2DFilter.h
*  \ingroup ketsji
*/

#ifndef __KX_2DFILTER_H__
#define __KX_2DFILTER_H__

#include "RAS_2DFilter.h"
#include "KX_Shader.h"

#ifdef _MSC_VER
/* KX_2DFilter uses a diamond inheritance from a virtual pure base class. Only one branch of the diamond
 * define these virtual pure functions and come in the final class with dominance. This behaviour is wanted
 * but MSVC warn about it, we just disable the warning.
 */
#  pragma warning(disable:4250)
#endif

class KX_2DFilter : public RAS_2DFilter, public KX_Shader
{
	Py_Header
public:
	KX_2DFilter(RAS_2DFilterData& data);
	virtual ~KX_2DFilter();

	virtual bool LinkProgram();

#ifdef WITH_PYTHON
	bool CheckTexture(int index, int bindCode, const std::string& prefix) const;
	bool SetTextureUniform(int index, const char *samplerName);

	static PyObject *pyattr_get_mipmap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_mipmap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_offScreen(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

	EXP_PYMETHOD_DOC(KX_2DFilter, setTexture);
	EXP_PYMETHOD_DOC(KX_2DFilter, setCubeMap);
	EXP_PYMETHOD_DOC(KX_2DFilter, addOffScreen);
	EXP_PYMETHOD_DOC_NOARGS(KX_2DFilter, removeOffScreen);

#endif
};

#endif  // __KX_2DFILTER_H__

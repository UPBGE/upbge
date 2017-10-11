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

/** \file KX_2DFilterManager.cpp
 *  \ingroup ketsji
 */

#include "KX_2DFilterManager.h"
#include "KX_2DFilter.h"
#include "KX_2DFilterOffScreen.h"

#include "CM_Message.h"

KX_2DFilterManager::KX_2DFilterManager()
{
}

KX_2DFilterManager::~KX_2DFilterManager()
{
}

RAS_2DFilter *KX_2DFilterManager::NewFilter(RAS_2DFilterData& filterData)
{
	return new KX_2DFilter(filterData);
}

#ifdef WITH_PYTHON
PyMethodDef KX_2DFilterManager::Methods[] = {
	// creation
	EXP_PYMETHODTABLE(KX_2DFilterManager, getFilter),
	EXP_PYMETHODTABLE(KX_2DFilterManager, addFilter),
	EXP_PYMETHODTABLE(KX_2DFilterManager, removeFilter),
	{nullptr, nullptr} //Sentinel
};

EXP_Attribute KX_2DFilterManager::Attributes[] = {
	EXP_ATTRIBUTE_NULL //Sentinel
};

PyTypeObject KX_2DFilterManager::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_2DFilterManager",
	sizeof(EXP_PyObjectPlus_Proxy),
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
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};


EXP_PYMETHODDEF_DOC(KX_2DFilterManager, getFilter, " getFilter(index)")
{
	int index = 0;

	if (!PyArg_ParseTuple(args, "i:getFilter", &index)) {
		return nullptr;
	}

	KX_2DFilter *filter = (KX_2DFilter *)GetFilterPass(index);

	if (filter) {
		return filter->GetProxy();
	}

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_2DFilterManager, addFilter, " addFilter(index, type, fragmentProgram)")
{
	int index = 0;
	int type = 0;
	const char *frag = "";

	if (!PyArg_ParseTuple(args, "ii|s:addFilter", &index, &type, &frag)) {
		return nullptr;
	}

	if (GetFilterPass(index)) {
		PyErr_Format(PyExc_ValueError, "filterManager.addFilter(index, type, fragmentProgram): KX_2DFilterManager, found existing filter in index (%i)", index);
		return nullptr;
	}

	if (type < FILTER_BLUR || type > FILTER_CUSTOMFILTER) {
		PyErr_SetString(PyExc_ValueError, "filterManager.addFilter(index, type, fragmentProgram): KX_2DFilterManager, type invalid");
		return nullptr;
	}

	if (strlen(frag) > 0 && type != FILTER_CUSTOMFILTER) {
		CM_PythonFunctionWarning("KX_2DFilterManager", "addFilter", "non-empty fragment program with non-custom filter type");
	}

	RAS_2DFilterData data;
	data.filterPassIndex = index;
	data.filterMode = type;
	data.shaderText = std::string(frag);

	KX_2DFilter *filter = static_cast<KX_2DFilter *>(AddFilter(data));

	return filter->GetProxy();
}

EXP_PYMETHODDEF_DOC(KX_2DFilterManager, removeFilter, " removeFilter(index)")
{
	int index = 0;

	if (!PyArg_ParseTuple(args, "i:removeFilter", &index)) {
		return nullptr;
	}

	RemoveFilterPass(index);

	Py_RETURN_NONE;
}

#endif  // WITH_PYTHON

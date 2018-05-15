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
 * Contributor(s): Campbell Barton
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_PythonInitTypes.cpp
 *  \ingroup ketsji
 */

#ifdef WITH_PYTHON

#include "KX_PythonInitTypes.h"

/* Only for Class::Parents */
#include "BL_BlenderShader.h"
#include "BL_Shader.h"
#include "BL_Texture.h"
#include "KX_2DFilter.h"
#include "KX_2DFilterManager.h"
#include "KX_2DFilterOffScreen.h"
#include "KX_WorldInfo.h"
#include "KX_BatchGroup.h"
#include "KX_BlenderMaterial.h"
#include "KX_BoundingBox.h"
#include "KX_Camera.h"
#include "KX_CharacterWrapper.h"
#include "KX_ConstraintWrapper.h"
#include "KX_CubeMap.h"
#include "KX_LibLoadStatus.h"
#include "KX_LightObject.h"
#include "KX_LodLevel.h"
#include "KX_LodManager.h"
#include "KX_FontObject.h"
#include "KX_Mesh.h"
#include "KX_PlanarMap.h"
#include "KX_PolyProxy.h"
#include "KX_PythonComponent.h"
#include "KX_VehicleWrapper.h"
#include "KX_VertexProxy.h"
#include "SCA_InputEvent.h"
#include "SCA_PythonJoystick.h"
#include "SCA_PythonKeyboard.h"
#include "SCA_PythonMouse.h"
#include "KX_NavMeshObject.h"
#include "KX_CollisionContactPoints.h"
#include "EXP_ListWrapper.h"
#include "Texture.h"

static void PyType_Attr_Set(PyGetSetDef *attr_getset, PyAttributeDef *attr)
{
	attr_getset->name = (char *)attr->m_name.c_str();
	attr_getset->doc = nullptr;

	attr_getset->get = reinterpret_cast<getter>(EXP_PyObjectPlus::py_get_attrdef);

	if (attr->m_access == EXP_PYATTRIBUTE_RO) {
		attr_getset->set = nullptr;
	}
	else {
		attr_getset->set = reinterpret_cast<setter>(EXP_PyObjectPlus::py_set_attrdef);
	}

	attr_getset->closure = reinterpret_cast<void *>(attr);
}

static void PyType_Ready_ADD(PyObject *dict, PyTypeObject *tp, PyAttributeDef *attributes, PyAttributeDef *attributesPtr, int init_getset)
{
	PyAttributeDef *attr;

	if (init_getset) {
		/* we need to do this for all types before calling PyType_Ready
		 * since they will call the parents PyType_Ready and those might not have initialized vars yet */

		if (tp->tp_getset == nullptr && ((attributes && !attributes->m_name.empty()) || (attributesPtr && !attributesPtr->m_name.empty()))) {
			PyGetSetDef *attr_getset;
			int attr_tot = 0;

			if (attributes) {
				for (attr = attributes; !attr->m_name.empty(); attr++, attr_tot++) {
					attr->m_usePtr = false;
				}
			}
			if (attributesPtr) {
				for (attr = attributesPtr; !attr->m_name.empty(); attr++, attr_tot++) {
					attr->m_usePtr = true;
				}
			}

			tp->tp_getset = attr_getset = reinterpret_cast<PyGetSetDef *>(PyMem_Malloc((attr_tot + 1) * sizeof(PyGetSetDef))); // XXX - Todo, free

			if (attributes) {
				for (attr = attributes; !attr->m_name.empty(); attr++, attr_getset++) {
					PyType_Attr_Set(attr_getset, attr);
				}
			}
			if (attributesPtr) {
				for (attr = attributesPtr; !attr->m_name.empty(); attr++, attr_getset++) {
					PyType_Attr_Set(attr_getset, attr);
				}
			}
			memset(attr_getset, 0, sizeof(PyGetSetDef));
		}
	}
	else {
		PyType_Ready(tp);
		PyDict_SetItemString(dict, tp->tp_name, reinterpret_cast<PyObject *>(tp));
	}

}


#define PyType_Ready_Attr(d, n, i)   PyType_Ready_ADD(d, &n::Type, n::Attributes, nullptr, i)
#define PyType_Ready_AttrPtr(d, n, i)   PyType_Ready_ADD(d, &n::Type, n::Attributes, n::AttributesPtr, i)



PyDoc_STRVAR(GameTypes_module_documentation,
             "This module provides access to the game engine data types."
             );
static struct PyModuleDef GameTypes_module_def = {
	PyModuleDef_HEAD_INIT,
	"GameTypes",  /* m_name */
	GameTypes_module_documentation,  /* m_doc */
	0,  /* m_size */
	nullptr,  /* m_methods */
	nullptr,  /* m_reload */
	nullptr,  /* m_traverse */
	nullptr,  /* m_clear */
	nullptr,  /* m_free */
};


PyMODINIT_FUNC initGameTypesPythonBinding(void)
{
	PyObject *m;
	PyObject *dict;

	m = PyModule_Create(&GameTypes_module_def);
	PyDict_SetItemString(PySys_GetObject("modules"), GameTypes_module_def.m_name, m);

	dict = PyModule_GetDict(m);

	for (int init_getset = 1; init_getset > -1; init_getset--) { /* run twice, once to init the getsets another to run PyType_Ready */
		PyType_Ready_Attr(dict, BL_Shader, init_getset);
		PyType_Ready_Attr(dict, BL_Texture, init_getset);
		PyType_Ready_Attr(dict, EXP_BaseListValue, init_getset);
		PyType_Ready_Attr(dict, EXP_BaseListWrapper, init_getset);
		PyType_Ready_Attr(dict, EXP_Value, init_getset);
		PyType_Ready_Attr(dict, KX_2DFilter, init_getset);
		PyType_Ready_Attr(dict, KX_2DFilterManager, init_getset);
		PyType_Ready_Attr(dict, KX_2DFilterOffScreen, init_getset);
		PyType_Ready_Attr(dict, KX_BatchGroup, init_getset);
		PyType_Ready_Attr(dict, KX_BlenderMaterial, init_getset);
		PyType_Ready_Attr(dict, KX_BoundingBox, init_getset);
		PyType_Ready_Attr(dict, KX_Camera, init_getset);
		PyType_Ready_Attr(dict, KX_CharacterWrapper, init_getset);
		PyType_Ready_Attr(dict, KX_ConstraintWrapper, init_getset);
		PyType_Ready_Attr(dict, KX_CubeMap, init_getset);
		PyType_Ready_Attr(dict, KX_GameObject, init_getset);
		PyType_Ready_Attr(dict, KX_LibLoadStatus, init_getset);
		PyType_Ready_Attr(dict, KX_LightObject, init_getset);
		PyType_Ready_Attr(dict, KX_LodLevel, init_getset);
		PyType_Ready_Attr(dict, KX_LodManager, init_getset);
		PyType_Ready_Attr(dict, KX_FontObject, init_getset);
		PyType_Ready_Attr(dict, KX_Mesh, init_getset);
		PyType_Ready_Attr(dict, KX_PlanarMap, init_getset);
		PyType_Ready_Attr(dict, KX_PolyProxy, init_getset);
		PyType_Ready_Attr(dict, KX_PythonComponent, init_getset);
		PyType_Ready_Attr(dict, KX_Scene, init_getset);
		PyType_Ready_Attr(dict, KX_WorldInfo, init_getset);
		PyType_Ready_Attr(dict, KX_NavMeshObject, init_getset);
		PyType_Ready_Attr(dict, KX_TextureRenderer, init_getset);
		PyType_Ready_Attr(dict, KX_VehicleWrapper, init_getset);
		PyType_Ready_Attr(dict, KX_VertexProxy, init_getset);
		PyType_Ready_Attr(dict, KX_CollisionContactPoint, init_getset);
		PyType_Ready_Attr(dict, EXP_PyObjectPlus, init_getset);
		PyType_Ready_Attr(dict, SCA_InputEvent, init_getset);
		PyType_Ready_Attr(dict, SCA_PythonJoystick, init_getset);
		PyType_Ready_Attr(dict, SCA_PythonKeyboard, init_getset);
		PyType_Ready_Attr(dict, SCA_PythonMouse, init_getset);
		PyType_Ready_Attr(dict, Texture, init_getset);
	}

#ifdef USE_MATHUTILS
	/* Init mathutils callbacks */
	KX_GameObject_Mathutils_Callback_Init();
	KX_WorldInfo_Mathutils_Callback_Init();
	KX_BlenderMaterial_Mathutils_Callback_Init();
	KX_BoundingBox_Mathutils_Callback_Init();
	BL_Texture_Mathutils_Callback_Init();
#endif

	return m;
}

#endif // WITH_PYTHON

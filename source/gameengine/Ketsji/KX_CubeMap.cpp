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

/** \file KX_CubeMap.cpp
*  \ingroup ketsji
*/

#include "KX_CubeMap.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_BlenderSceneConverter.h"

#include "RAS_Texture.h"

#include "DNA_texture_types.h"

KX_CubeMap::KX_CubeMap(KX_BlenderSceneConverter *converter, KX_GameObject *gameobj, RAS_Texture *texture, RAS_IRasterizer *rasty)
	:RAS_CubeMap(texture, rasty),
	m_viewpointObject(gameobj),
	m_autoUpdate(true)
{
	MTex *mtex = m_texture->GetMTex();

	EnvMap *env = mtex->tex->env;
	if (env->object) {
		KX_GameObject *obj = converter->FindGameObject(env->object);
		if (obj) {
			m_viewpointObject = obj;
		}
	}

	m_texture->SetCubeMap(this);
}

KX_CubeMap::~KX_CubeMap()
{
	m_texture->SetCubeMap(NULL);
}

STR_String& KX_CubeMap::GetName()
{
	return m_texture->GetName();
}

KX_GameObject *KX_CubeMap::GetViewpointObject() const
{
	return m_viewpointObject;
}

void KX_CubeMap::SetViewpointObject(KX_GameObject *gameobj)
{
	m_viewpointObject = gameobj;
}

bool KX_CubeMap::NeedUpdate() const
{
	return m_autoUpdate;
}

#ifdef WITH_PYTHON

PyTypeObject KX_CubeMap::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_CubeMap",
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
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_CubeMap::Methods[] = {
	{NULL, NULL} // Sentinel
};

PyAttributeDef KX_CubeMap::Attributes[] = {
	KX_PYATTRIBUTE_RW_FUNCTION("viewpointObject", KX_CubeMap, pyattr_get_viewpoint_object, pyattr_set_viewpoint_object),
	KX_PYATTRIBUTE_BOOL_RW("autoUpdate", KX_CubeMap, m_autoUpdate),
	KX_PYATTRIBUTE_INT_RW("ignoreLayers", 0, (1 << 20) - 1, true, KX_CubeMap, m_ignoreLayers),
	{NULL} // Sentinel
};

PyObject *KX_CubeMap::pyattr_get_viewpoint_object(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_CubeMap *self = static_cast<KX_CubeMap*>(self_v);
	return self->GetViewpointObject()->GetProxy();
}


int KX_CubeMap::pyattr_set_viewpoint_object(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_CubeMap *self = static_cast<KX_CubeMap*>(self_v);
	KX_GameObject *gameobj;

	SCA_LogicManager *logicmgr = KX_GetActiveScene()->GetLogicManager();

	if (!ConvertPythonToGameObject(logicmgr, value, &gameobj, false, "cubeMap.object = value: KX_CubeMap"))
		return PY_SET_ATTR_FAIL;
	
	self->SetViewpointObject(gameobj);
	return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON

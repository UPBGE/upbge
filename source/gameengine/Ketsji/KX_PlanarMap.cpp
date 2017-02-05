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
* Contributor(s): Ulysse Martin, Tristan Porteries, Martins Upitis.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file KX_PlanarMap.cpp
*  \ingroup ketsji
*/

#include "KX_PlanarMap.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"

#include "DNA_texture_types.h"

KX_PlanarMap::KX_PlanarMap(EnvMap *env, KX_GameObject *viewpoint, RAS_IPolyMaterial *polymat)
	:RAS_PlanarMap(viewpoint, polymat),
	m_viewpointObject(viewpoint),
	m_invalidProjection(true),
	m_enabled(true),
	m_clipStart(0.0f),
	m_clipEnd(0.0f),
	m_autoUpdate(true),
	m_forceUpdate(true)
{
	m_ignoreLayers = env->notlay;

	m_clipStart = env->clipsta;
	m_clipEnd = env->clipend;

	m_autoUpdate = (env->flag & ENVMAP_AUTO_UPDATE) != 0;
}

KX_PlanarMap::~KX_PlanarMap()
{
}

std::string KX_PlanarMap::GetName()
{
	return "KX_PlanarMap";
}

KX_GameObject *KX_PlanarMap::GetMirrorObject() const
{
	return m_viewpointObject;
}

void KX_PlanarMap::SetInvalidProjectionMatrix(bool invalid)
{
	m_invalidProjection = invalid;
}

bool KX_PlanarMap::GetInvalidProjectionMatrix() const
{
	return m_invalidProjection;
}

void KX_PlanarMap::SetProjectionMatrix(const MT_Matrix4x4& projection)
{
	m_projection = projection;
}

const MT_Matrix4x4& KX_PlanarMap::GetProjectionMatrix() const
{
	return m_projection;
}

bool KX_PlanarMap::GetEnabled() const
{
	return m_enabled;
}

int KX_PlanarMap::GetIgnoreLayers() const
{
	return m_ignoreLayers;
}

float KX_PlanarMap::GetClipStart() const
{
	return m_clipStart;
}

float KX_PlanarMap::GetClipEnd() const
{
	return m_clipEnd;
}

void KX_PlanarMap::SetClipStart(float start)
{
	m_clipStart = start;
}

void KX_PlanarMap::SetClipEnd(float end)
{
	m_clipEnd = end;
}

bool KX_PlanarMap::NeedUpdate()
{
	bool result = m_autoUpdate || m_forceUpdate;
	m_forceUpdate = false;

	return result;
}

#ifdef WITH_PYTHON

PyTypeObject KX_PlanarMap::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_PlanarMap",
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

PyMethodDef KX_PlanarMap::Methods[] = {
	KX_PYMETHODTABLE_NOARGS(KX_PlanarMap, update),
	{ NULL, NULL } // Sentinel
};

PyAttributeDef KX_PlanarMap::Attributes[] = {
	KX_PYATTRIBUTE_BOOL_RW("autoUpdate", KX_PlanarMap, m_autoUpdate),
	KX_PYATTRIBUTE_BOOL_RW("enabled", KX_PlanarMap, m_enabled),
	KX_PYATTRIBUTE_INT_RW("ignoreLayers", 0, (1 << 20) - 1, true, KX_PlanarMap, m_ignoreLayers),
	KX_PYATTRIBUTE_RW_FUNCTION("clipStart", KX_PlanarMap, pyattr_get_clip_start, pyattr_set_clip_start),
	KX_PYATTRIBUTE_RW_FUNCTION("clipEnd", KX_PlanarMap, pyattr_get_clip_end, pyattr_set_clip_end),
	KX_PYATTRIBUTE_NULL // Sentinel
};

KX_PYMETHODDEF_DOC_NOARGS(KX_PlanarMap, update, "update(): Set the planar to be updated next frame.\n")
{
	m_forceUpdate = true;
	Py_RETURN_NONE;
}

PyObject *KX_PlanarMap::pyattr_get_clip_start(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_PlanarMap *self = static_cast<KX_PlanarMap*>(self_v);
	return PyFloat_FromDouble(self->GetClipStart());
}

int KX_PlanarMap::pyattr_set_clip_start(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_PlanarMap *self = static_cast<KX_PlanarMap*>(self_v);

	const float val = PyFloat_AsDouble(value);

	if (val <= 0.0f) {
		PyErr_SetString(PyExc_AttributeError, "planar.clipStart = float: KX_PlanarMap, expected a float grater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->SetClipStart(val);
	self->SetInvalidProjectionMatrix(true);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_PlanarMap::pyattr_get_clip_end(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_PlanarMap *self = static_cast<KX_PlanarMap*>(self_v);
	return PyFloat_FromDouble(self->GetClipEnd());
}

int KX_PlanarMap::pyattr_set_clip_end(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_PlanarMap *self = static_cast<KX_PlanarMap*>(self_v);

	const float val = PyFloat_AsDouble(value);

	if (val <= 0.0f) {
		PyErr_SetString(PyExc_AttributeError, "planar.clipEnd = float: KX_PlanarMap, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->SetClipEnd(val);
	self->SetInvalidProjectionMatrix(true);

	return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON

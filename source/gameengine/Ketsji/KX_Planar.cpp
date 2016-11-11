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

/** \file KX_Planar.cpp
*  \ingroup ketsji
*/

#include "KX_Planar.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"

#include "DNA_texture_types.h"

KX_Planar::KX_Planar(Tex *tex, KX_GameObject *viewpoint, RAS_IPolyMaterial *polymat, short type, int width, int height)
	:RAS_Planar(viewpoint, polymat),
	m_viewpointObject(viewpoint),
	m_invalidProjection(true),
	m_enabled(true),
	m_clipStart(0.0f),
	m_clipEnd(0.0f),
	m_autoUpdate(true),
	m_forceUpdate(true),
	m_type(type),
	m_width(width),
	m_height(height)
{
	m_ignoreLayers = tex->notlay;

	m_clipStart = tex->clipsta;
	m_clipEnd = tex->clipend;

	m_autoUpdate = (tex->autoupdate & TEX_AUTO_UPDATE) != 0;

	m_cullReflections = (tex->planarcull & TEX_PLANAR_REFLECT_CULL) != 0;
}

KX_Planar::~KX_Planar()
{
}

static STR_String planarName = "KX_Planar";
STR_String& KX_Planar::GetName()
{
	return planarName;
}

KX_GameObject *KX_Planar::GetMirrorObject() const
{
	return m_viewpointObject;
}

void KX_Planar::SetInvalidProjectionMatrix(bool invalid)
{
	m_invalidProjection = invalid;
}

bool KX_Planar::GetInvalidProjectionMatrix() const
{
	return m_invalidProjection;
}

void KX_Planar::SetProjectionMatrix(const MT_Matrix4x4& projection)
{
	m_projection = projection;
}

const MT_Matrix4x4& KX_Planar::GetProjectionMatrix() const
{
	return m_projection;
}

bool KX_Planar::GetEnabled() const
{
	return m_enabled;
}

int KX_Planar::GetIgnoreLayers() const
{
	return m_ignoreLayers;
}

float KX_Planar::GetClipStart() const
{
	return m_clipStart;
}

float KX_Planar::GetClipEnd() const
{
	return m_clipEnd;
}

void KX_Planar::SetClipStart(float start)
{
	m_clipStart = start;
}

void KX_Planar::SetClipEnd(float end)
{
	m_clipEnd = end;
}

bool KX_Planar::NeedUpdate()
{
	bool result = m_autoUpdate || m_forceUpdate;
	m_forceUpdate = false;

	return result;
}

short KX_Planar::GetWidth()
{
	return m_width;
}

short KX_Planar::GetHeight()
{
	return m_height;
}

short KX_Planar::GetPlanarType()
{
	return m_type;
}

bool KX_Planar::GetCullReflections()
{
	return m_cullReflections;
}

#ifdef WITH_PYTHON

PyTypeObject KX_Planar::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_Planar",
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

PyMethodDef KX_Planar::Methods[] = {
	KX_PYMETHODTABLE_NOARGS(KX_Planar, update),
	{ NULL, NULL } // Sentinel
};

PyAttributeDef KX_Planar::Attributes[] = {
	KX_PYATTRIBUTE_BOOL_RW("autoUpdate", KX_Planar, m_autoUpdate),
	KX_PYATTRIBUTE_BOOL_RW("enabled", KX_Planar, m_enabled),
	KX_PYATTRIBUTE_INT_RW("ignoreLayers", 0, (1 << 20) - 1, true, KX_Planar, m_ignoreLayers),
	KX_PYATTRIBUTE_RW_FUNCTION("clipStart", KX_Planar, pyattr_get_clip_start, pyattr_set_clip_start),
	KX_PYATTRIBUTE_RW_FUNCTION("clipEnd", KX_Planar, pyattr_get_clip_end, pyattr_set_clip_end),
	{ NULL } // Sentinel
};

KX_PYMETHODDEF_DOC_NOARGS(KX_Planar, update, "update(): Set the planar to be updated next frame.\n")
{
	m_forceUpdate = true;
	Py_RETURN_NONE;
}

PyObject *KX_Planar::pyattr_get_clip_start(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_Planar *self = static_cast<KX_Planar*>(self_v);
	return PyFloat_FromDouble(self->GetClipStart());
}

int KX_Planar::pyattr_set_clip_start(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Planar *self = static_cast<KX_Planar*>(self_v);

	const float val = PyFloat_AsDouble(value);

	if (val <= 0.0f) {
		PyErr_SetString(PyExc_AttributeError, "planar.clipStart = float: KX_Planar, expected a float grater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->SetClipStart(val);
	self->SetInvalidProjectionMatrix(true);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Planar::pyattr_get_clip_end(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_Planar *self = static_cast<KX_Planar*>(self_v);
	return PyFloat_FromDouble(self->GetClipEnd());
}

int KX_Planar::pyattr_set_clip_end(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Planar *self = static_cast<KX_Planar*>(self_v);

	const float val = PyFloat_AsDouble(value);

	if (val <= 0.0f) {
		PyErr_SetString(PyExc_AttributeError, "planar.clipEnd = float: KX_Planar, expected a float greater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->SetClipEnd(val);
	self->SetInvalidProjectionMatrix(true);

	return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON

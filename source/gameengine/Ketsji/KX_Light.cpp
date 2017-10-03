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

/** \file gameengine/Ketsji/KX_Light.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include <stdio.h>
#include "DNA_scene_types.h"

#include "KX_Light.h"
#include "KX_Camera.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_ILightObject.h"

#include "KX_PyMath.h"

#include "DNA_object_types.h"
#include "DNA_lamp_types.h"

#include "BKE_scene.h"
#include "MEM_guardedalloc.h"

#include "BLI_math.h"

KX_LightObject::KX_LightObject(void *sgReplicationInfo, SG_Callbacks callbacks,
                               RAS_ILightObject *lightobj)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_lightobj(lightobj),
	m_showShadowFrustum(false)
{
}

KX_LightObject::~KX_LightObject()
{
	if (m_lightobj) {
		delete(m_lightobj);
	}
}

CValue *KX_LightObject::GetReplica()
{
	KX_LightObject *replica = new KX_LightObject(*this);

	replica->ProcessReplica();

	replica->m_lightobj = m_lightobj->Clone();

	return replica;
}

MT_Matrix4x4 KX_LightObject::GetShadowFrustumMatrix() const
{
	MT_Matrix4x4 matrix = MT_Matrix4x4::Identity();
	switch (m_lightobj->m_type) {
		case RAS_ILightObject::LIGHT_SUN:
		{
			matrix[0][0] = matrix[1][1] = matrix[2][2] = 1000.0f;
			break;
		}
		default:
		{
			const MT_Vector3& pos = NodeGetWorldPosition();
			matrix[0][0] = matrix[1][1] = matrix[2][2] = m_lightobj->m_shadowclipend;

			matrix[0][3] = pos.x();
			matrix[1][3] = pos.y();
			matrix[2][3] = pos.z();
			break;
		}
	}

	return matrix;
}

bool KX_LightObject::GetShowShadowFrustum() const
{
	return m_showShadowFrustum;
}

void KX_LightObject::SetShowShadowFrustum(bool show)
{
	m_showShadowFrustum = show;
}

void KX_LightObject::SetLayer(int layer)
{
	KX_GameObject::SetLayer(layer);
	m_lightobj->m_layer = layer;
}

#ifdef WITH_PYTHON
/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					                                 */
/* ------------------------------------------------------------------------- */

PyTypeObject KX_LightObject::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_LightObject",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	&KX_GameObject::Sequence,
	&KX_GameObject::Mapping,
	0, 0, 0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&KX_GameObject::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_LightObject::Methods[] = {
	KX_PYMETHODTABLE_NOARGS(KX_LightObject, updateShadow),
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef KX_LightObject::Attributes[] = {
	KX_PYATTRIBUTE_RW_FUNCTION("energy", KX_LightObject, pyattr_get_energy, pyattr_set_energy),
	KX_PYATTRIBUTE_RW_FUNCTION("distance", KX_LightObject, pyattr_get_distance, pyattr_set_distance),
	KX_PYATTRIBUTE_RW_FUNCTION("color", KX_LightObject, pyattr_get_color, pyattr_set_color),
	KX_PYATTRIBUTE_RW_FUNCTION("lin_attenuation", KX_LightObject, pyattr_get_lin_attenuation, pyattr_set_lin_attenuation),
	KX_PYATTRIBUTE_RW_FUNCTION("quad_attenuation", KX_LightObject, pyattr_get_quad_attenuation, pyattr_set_quad_attenuation),
	KX_PYATTRIBUTE_RW_FUNCTION("spotsize", KX_LightObject, pyattr_get_spotsize, pyattr_set_spotsize),
	KX_PYATTRIBUTE_RW_FUNCTION("spotblend", KX_LightObject, pyattr_get_spotblend, pyattr_set_spotblend),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowClipStart", KX_LightObject, pyattr_get_shadow_clip_start),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowClipEnd", KX_LightObject, pyattr_get_shadow_clip_end),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowFrustumSize", KX_LightObject, pyattr_get_shadow_frustum_size),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowBias", KX_LightObject, pyattr_get_shadow_bias),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowBleedBias", KX_LightObject, pyattr_get_shadow_bleed_bias),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowBindId", KX_LightObject, pyattr_get_shadow_bind_code),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowMapType", KX_LightObject, pyattr_get_shadow_map_type),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowColor", KX_LightObject, pyattr_get_shadow_color),
	KX_PYATTRIBUTE_RO_FUNCTION("useShadow", KX_LightObject, pyattr_get_shadow_active),
	KX_PYATTRIBUTE_RO_FUNCTION("shadowMatrix", KX_LightObject, pyattr_get_shadow_matrix),
	KX_PYATTRIBUTE_RO_FUNCTION("SPOT", KX_LightObject, pyattr_get_typeconst),
	KX_PYATTRIBUTE_RO_FUNCTION("SUN", KX_LightObject, pyattr_get_typeconst),
	KX_PYATTRIBUTE_RO_FUNCTION("NORMAL", KX_LightObject, pyattr_get_typeconst),
	KX_PYATTRIBUTE_RO_FUNCTION("HEMI", KX_LightObject, pyattr_get_typeconst),
	KX_PYATTRIBUTE_RW_FUNCTION("type", KX_LightObject, pyattr_get_type, pyattr_set_type),
	KX_PYATTRIBUTE_RW_FUNCTION("staticShadow", KX_LightObject, pyattr_get_static_shadow, pyattr_set_static_shadow),
	KX_PYATTRIBUTE_NULL // Sentinel
};

KX_PYMETHODDEF_DOC_NOARGS(KX_LightObject, updateShadow, "updateShadow(): Set the shadow to be updated next frame if the lamp uses a static shadow.\n")
{
	m_lightobj->m_requestShadowUpdate = true;
	Py_RETURN_NONE;
}

PyObject *KX_LightObject::pyattr_get_energy(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_energy);
}

int KX_LightObject::pyattr_set_energy(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);

	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		if (val < 0)
			val = 0;
		else if (val > 10)
			val = 10;

		self->m_lightobj->m_energy = val;
		return PY_SET_ATTR_SUCCESS;
	}

	PyErr_Format(PyExc_TypeError, "expected float value for attribute \"%s\"", attrdef->m_name.c_str());
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_LightObject::pyattr_get_shadow_clip_start(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_shadowclipstart);
}

PyObject *KX_LightObject::pyattr_get_shadow_clip_end(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_shadowclipend);
}

PyObject *KX_LightObject::pyattr_get_shadow_frustum_size(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_shadowfrustumsize);
}

PyObject *KX_LightObject::pyattr_get_shadow_bind_code(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyLong_FromLong(self->m_lightobj->GetShadowBindCode());
}

PyObject *KX_LightObject::pyattr_get_shadow_bias(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_shadowbias);
}

PyObject *KX_LightObject::pyattr_get_shadow_bleed_bias(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_shadowbleedbias);
}

PyObject *KX_LightObject::pyattr_get_shadow_map_type(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyLong_FromLong(self->m_lightobj->m_shadowmaptype);
}

PyObject *KX_LightObject::pyattr_get_shadow_matrix(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyObjectFrom(self->m_lightobj->GetShadowMatrix());
}

PyObject *KX_LightObject::pyattr_get_shadow_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyColorFromVector(MT_Vector3(self->m_lightobj->m_shadowcolor));
}

PyObject *KX_LightObject::pyattr_get_shadow_active(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyBool_FromLong(self->m_lightobj->HasShadow());
}

PyObject *KX_LightObject::pyattr_get_distance(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_distance);
}

int KX_LightObject::pyattr_set_distance(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);

	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		if (val < 0.01f)
			val = 0.01f;
		else if (val > 5000.f)
			val = 5000.f;

		self->m_lightobj->m_distance = val;
		return PY_SET_ATTR_SUCCESS;
	}

	PyErr_Format(PyExc_TypeError, "expected float value for attribute \"%s\"", attrdef->m_name.c_str());
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_LightObject::pyattr_get_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyColorFromVector(MT_Vector3(self->m_lightobj->m_color));
}

int KX_LightObject::pyattr_set_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);

	MT_Vector3 color;
	if (PyVecTo(value, color)) {
		color.getValue(self->m_lightobj->m_color);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_LightObject::pyattr_get_lin_attenuation(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_att1);
}

int KX_LightObject::pyattr_set_lin_attenuation(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);

	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		if (val < 0.f)
			val = 0.f;
		else if (val > 1.f)
			val = 1.f;

		self->m_lightobj->m_att1 = val;
		return PY_SET_ATTR_SUCCESS;
	}

	PyErr_Format(PyExc_TypeError, "expected float value for attribute \"%s\"", attrdef->m_name.c_str());
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_LightObject::pyattr_get_quad_attenuation(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_att2);
}

int KX_LightObject::pyattr_set_quad_attenuation(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);

	if (PyFloat_Check(value)) {
		float val = PyFloat_AsDouble(value);
		if (val < 0.f)
			val = 0.f;
		else if (val > 1.f)
			val = 1.f;

		self->m_lightobj->m_att2 = val;
		return PY_SET_ATTR_SUCCESS;
	}

	PyErr_Format(PyExc_TypeError, "expected float value for attribute \"%s\"", attrdef->m_name.c_str());
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_LightObject::pyattr_get_spotsize(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(RAD2DEG(self->m_lightobj->m_spotsize));
}

int KX_LightObject::pyattr_set_spotsize(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);

	if (PyFloat_Check(value)) {
		double val = PyFloat_AsDouble(value);
		if (val < 0.0)
			val = 0.0;
		else if (val > 180.0)
			val = 180.0;

		self->m_lightobj->m_spotsize = (float)DEG2RAD(val);
		return PY_SET_ATTR_SUCCESS;
	}

	PyErr_Format(PyExc_TypeError, "expected float value for attribute \"%s\"", attrdef->m_name.c_str());
	return PY_SET_ATTR_FAIL;
}
PyObject *KX_LightObject::pyattr_get_spotblend(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyFloat_FromDouble(self->m_lightobj->m_spotblend);
}

int KX_LightObject::pyattr_set_spotblend(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);

	if (PyFloat_Check(value)) {
		float val = (float)PyFloat_AsDouble(value);
		if (val < 0.f)
			val = 0.f;
		else if (val > 1.f)
			val = 1.f;

		self->m_lightobj->m_spotblend = val;
		return PY_SET_ATTR_SUCCESS;
	}

	PyErr_Format(PyExc_TypeError, "expected float value for attribute \"%s\"", attrdef->m_name.c_str());
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_LightObject::pyattr_get_typeconst(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	PyObject *retvalue;

	const std::string& type = attrdef->m_name;

	if (type == "SPOT") {
		retvalue = PyLong_FromLong(RAS_ILightObject::LIGHT_SPOT);
	}
	else if (type == "SUN") {
		retvalue = PyLong_FromLong(RAS_ILightObject::LIGHT_SUN);
	}
	else if (type == "NORMAL") {
		retvalue = PyLong_FromLong(RAS_ILightObject::LIGHT_NORMAL);
	}
	else if (type == "HEMI") {
		retvalue = PyLong_FromLong(RAS_ILightObject::LIGHT_HEMI);
	}
	else {
		/* should never happen */
		PyErr_SetString(PyExc_TypeError, "light.type: internal error, invalid light type");
		retvalue = nullptr;
	}

	return retvalue;
}

PyObject *KX_LightObject::pyattr_get_type(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyLong_FromLong(self->m_lightobj->m_type);
}

int KX_LightObject::pyattr_set_type(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	const int val = PyLong_AsLong(value);
	if ((val == -1 && PyErr_Occurred()) || val < 0 || val > 3) {
		PyErr_SetString(PyExc_ValueError, "light.type= val: KX_LightObject, expected an int between 0 and 2");
		return PY_SET_ATTR_FAIL;
	}

	switch (val) {
		case 0:
			self->m_lightobj->m_type = self->m_lightobj->LIGHT_SPOT;
			break;
		case 1:
			self->m_lightobj->m_type = self->m_lightobj->LIGHT_SUN;
			break;
		case 2:
			self->m_lightobj->m_type = self->m_lightobj->LIGHT_NORMAL;
			break;
		case 3:
			self->m_lightobj->m_type = self->m_lightobj->LIGHT_HEMI;
			break;
	}

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_LightObject::pyattr_get_static_shadow(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	return PyBool_FromLong(self->m_lightobj->m_staticShadow);
}

int KX_LightObject::pyattr_set_static_shadow(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_LightObject *self = static_cast<KX_LightObject *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "light.staticShadow = val: KX_LightObject, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->m_lightobj->m_staticShadow = param;
	return PY_SET_ATTR_SUCCESS;
}
#endif // WITH_PYTHON

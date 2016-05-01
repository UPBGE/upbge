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

/** \file KX_Light.h
 *  \ingroup ketsji
 */

#ifndef __KX_LIGHT_H__
#define __KX_LIGHT_H__

#include "KX_GameObject.h"

#define MAX_LIGHT_LAYERS ((1 << 20) - 1)

struct GPULamp;
struct Scene;
struct Base;
class KX_Scene;
class KX_Camera;
class RAS_IRasterizer;
class RAS_ILightObject;
class MT_Transform;

class KX_LightObject : public KX_GameObject
{
	Py_Header
protected:
	RAS_ILightObject *m_lightobj;
	/// Needed for registering and replication of lightobj.
	RAS_IRasterizer *m_rasterizer;
	Scene *m_blenderscene;
	Base *m_base;

public:
	KX_LightObject(void *sgReplicationInfo, SG_Callbacks callbacks, RAS_IRasterizer *rasterizer, RAS_ILightObject *lightobj);
	virtual ~KX_LightObject();

	virtual CValue *GetReplica();
	RAS_ILightObject *GetLightData()
	{
		return m_lightobj;
	}

	void UpdateScene(KX_Scene *kxscene);
	virtual void SetLayer(int layer);

	virtual int GetGameObjectType()
	{
		return OBJ_LIGHT;
	}

#ifdef WITH_PYTHON
	// functions
	KX_PYMETHOD_DOC_NOARGS(KX_LightObject, updateShadow);

	// attributes
	static PyObject *pyattr_get_layer(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_layer(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_energy(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_energy(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_shadow_clip_start(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_clip_end(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_frustum_size(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_bind_code(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_bias(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_bleed_bias(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_map_type(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_active(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_shadow_matrix(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_distance(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_distance(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_lin_attenuation(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_lin_attenuation(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_quad_attenuation(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_quad_attenuation(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_spotsize(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_spotsize(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_spotblend(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_spotblend(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_typeconst(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_type(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_type(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_static_shadow(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_static_shadow(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif
};

#endif  // __KX_LIGHT_H__

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

/** \file KX_LightObject.h
 *  \ingroup ketsji
 */

#ifndef __KX_LIGHT_H__
#define __KX_LIGHT_H__

#include "KX_GameObject.h"

struct GPULamp;
struct Scene;
struct Base;
class KX_Scene;
class KX_Camera;
class RAS_Rasterizer;
class RAS_ILightObject;

class KX_LightObject : public KX_GameObject
{
	Py_Header(KX_LightObject)
protected:
	RAS_ILightObject *m_lightobj;
	/// Needed for registering and replication of lightobj.
	RAS_Rasterizer *m_rasterizer;
	Scene *m_blenderscene;
	Base *m_base;

	bool m_showShadowFrustum;

public:
	KX_LightObject(void *sgReplicationInfo, SG_Callbacks callbacks, RAS_Rasterizer *rasterizer, RAS_ILightObject *lightobj);
	virtual ~KX_LightObject();

	virtual EXP_Value *GetReplica();
	RAS_ILightObject *GetLightData()
	{
		return m_lightobj;
	}

	bool GetShowShadowFrustum() const;
	void SetShowShadowFrustum(bool show);

	// Update rasterizer light settings.
	void Update();

	void UpdateScene(KX_Scene *kxscene);
	virtual void SetLayer(int layer);

	virtual ObjectTypes GetObjectType() const
	{
		return OBJECT_TYPE_LIGHT;
	}

#ifdef WITH_PYTHON
	// functions
	EXP_PYMETHOD_DOC_NOARGS(KX_LightObject, updateShadow);

	// attributes
	float pyattr_get_energy();
	void pyattr_set_energy(float value);
	float pyattr_get_shadow_clip_start();
	float pyattr_get_shadow_clip_end();
	float pyattr_get_shadow_frustum_size();
	int pyattr_get_shadow_bind_code();
	float pyattr_get_shadow_bias();
	float pyattr_get_shadow_bleed_bias();
	int pyattr_get_shadow_map_type();
	mt::vec3 pyattr_get_shadow_color();
	bool pyattr_get_shadow_active();
	mt::mat4 pyattr_get_shadow_matrix();
	float pyattr_get_distance();
	void pyattr_set_distance(float value);
	mt::vec3 pyattr_get_color();
	void pyattr_set_color(const mt::vec3& value);
	float pyattr_get_lin_attenuation();
	void pyattr_set_lin_attenuation(float value);
	float pyattr_get_quad_attenuation();
	void pyattr_set_quad_attenuation(float value);
	float pyattr_get_spotsize();
	void pyattr_set_spotsize(float value);
	float pyattr_get_spotblend();
	void pyattr_set_spotblend(float value);
	int pyattr_get_typeconst(const EXP_Attribute *attrdef);
	int pyattr_get_type();
	void pyattr_set_type(int value);
	bool pyattr_get_static_shadow();
	void pyattr_set_static_shadow(bool value);
#endif
};

#endif  // __KX_LIGHT_H__

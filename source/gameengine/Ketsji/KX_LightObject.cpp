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

/** \file gameengine/Ketsji/KX_LightObject.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include <stdio.h>

#include "KX_LightObject.h"
#include "KX_Camera.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_ILightObject.h"

#include "KX_PyMath.h"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_lamp_types.h"

#include "BKE_scene.h"
#include "MEM_guardedalloc.h"

#include "BLI_math.h"

KX_LightObject::KX_LightObject(void *sgReplicationInfo, SG_Callbacks callbacks,
                               RAS_Rasterizer *rasterizer,
                               RAS_ILightObject *lightobj)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_rasterizer(rasterizer),
	m_showShadowFrustum(false)
{
	m_lightobj = lightobj;
	m_lightobj->m_scene = sgReplicationInfo;
	m_lightobj->m_light = this;
	m_rasterizer->AddLight(m_lightobj);
	m_blenderscene = ((KX_Scene *)sgReplicationInfo)->GetBlenderScene();
	m_base = nullptr;
}

KX_LightObject::~KX_LightObject()
{
	if (m_lightobj) {
		m_rasterizer->RemoveLight(m_lightobj);
		delete(m_lightobj);
	}

	if (m_base) {
		BKE_scene_base_unlink(m_blenderscene, m_base);
		MEM_freeN(m_base);
	}
}

EXP_Value *KX_LightObject::GetReplica()
{
	KX_LightObject *replica = new KX_LightObject(*this);

	replica->ProcessReplica();

	replica->m_lightobj = m_lightobj->Clone();
	replica->m_lightobj->m_light = replica;
	m_rasterizer->AddLight(replica->m_lightobj);
	if (m_base) {
		m_base = nullptr;
	}

	return replica;
}

bool KX_LightObject::GetShowShadowFrustum() const
{
	return m_showShadowFrustum;
}

void KX_LightObject::SetShowShadowFrustum(bool show)
{
	m_showShadowFrustum = show;
}

void KX_LightObject::Update()
{
	m_lightobj->Update(NodeGetWorldTransform(), !m_bVisible);
}

void KX_LightObject::UpdateScene(KX_Scene *kxscene)
{
	m_lightobj->m_scene = (void *)kxscene;
	m_blenderscene = kxscene->GetBlenderScene();
	m_base = BKE_scene_base_add(m_blenderscene, GetBlenderObject());
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
	sizeof(EXP_PyObjectPlus_Proxy),
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
	EXP_PYMETHODTABLE_NOARGS(KX_LightObject, updateShadow),
	{nullptr, nullptr} // Sentinel
};

EXP_Attribute KX_LightObject::Attributes[] = {
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("energy", pyattr_get_energy, pyattr_set_energy, 0.0f, 10.0f, true),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("distance", pyattr_get_distance, pyattr_set_distance, 0.01f, 5000.0f, true),
	EXP_ATTRIBUTE_RW_FUNCTION("color", pyattr_get_color, pyattr_set_color),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("lin_attenuation", pyattr_get_lin_attenuation, pyattr_set_lin_attenuation, 0.0f, 1.0f, true),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("quad_attenuation", pyattr_get_quad_attenuation, pyattr_set_quad_attenuation, 0.0f, 1.0f, true),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("spotsize", pyattr_get_spotsize, pyattr_set_spotsize, 0.0f, 180.0f, true),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("spotblend", pyattr_get_spotblend, pyattr_set_spotblend, 0.0f, 1.0f, true),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowClipStart", pyattr_get_shadow_clip_start),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowClipEnd", pyattr_get_shadow_clip_end),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowFrustumSize", pyattr_get_shadow_frustum_size),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowBias", pyattr_get_shadow_bias),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowBleedBias", pyattr_get_shadow_bleed_bias),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowBindId", pyattr_get_shadow_bind_code),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowMapType", pyattr_get_shadow_map_type),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowColor", pyattr_get_shadow_color),
	EXP_ATTRIBUTE_RO_FUNCTION("useShadow", pyattr_get_shadow_active),
	EXP_ATTRIBUTE_RO_FUNCTION("shadowMatrix", pyattr_get_shadow_matrix),
	EXP_ATTRIBUTE_RO_FUNCTION("SPOT", pyattr_get_typeconst),
	EXP_ATTRIBUTE_RO_FUNCTION("SUN", pyattr_get_typeconst),
	EXP_ATTRIBUTE_RO_FUNCTION("NORMAL", pyattr_get_typeconst),
	EXP_ATTRIBUTE_RO_FUNCTION("HEMI", pyattr_get_typeconst),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("type", pyattr_get_type, pyattr_set_type, 0, 2, false),
	EXP_ATTRIBUTE_RW_FUNCTION("staticShadow", pyattr_get_static_shadow, pyattr_set_static_shadow),
	EXP_ATTRIBUTE_NULL // Sentinel
};

EXP_PYMETHODDEF_DOC_NOARGS(KX_LightObject, updateShadow, "updateShadow(): Set the shadow to be updated next frame if the lamp uses a static shadow.\n")
{
	m_lightobj->m_requestShadowUpdate = true;
	Py_RETURN_NONE;
}

float KX_LightObject::pyattr_get_energy()
{
	return m_lightobj->m_energy;
}

void KX_LightObject::pyattr_set_energy(float value)
{
	m_lightobj->m_energy = value;
}

float KX_LightObject::pyattr_get_shadow_clip_start()
{
	return m_lightobj->m_shadowclipstart;
}

float KX_LightObject::pyattr_get_shadow_clip_end()
{
	return m_lightobj->m_shadowclipend;
}

float KX_LightObject::pyattr_get_shadow_frustum_size()
{
	return m_lightobj->m_shadowfrustumsize;
}

int KX_LightObject::pyattr_get_shadow_bind_code()
{
	return m_lightobj->GetShadowBindCode();
}

float KX_LightObject::pyattr_get_shadow_bias()
{
	return m_lightobj->m_shadowbias;
}

float KX_LightObject::pyattr_get_shadow_bleed_bias()
{
	return m_lightobj->m_shadowbleedbias;
}

int KX_LightObject::pyattr_get_shadow_map_type()
{
	return m_lightobj->m_shadowmaptype;
}

mt::mat4 KX_LightObject::pyattr_get_shadow_matrix()
{
	return m_lightobj->GetShadowMatrix();
}

mt::vec3 KX_LightObject::pyattr_get_shadow_color()
{
	return mt::vec3(m_lightobj->m_shadowcolor);
}

bool KX_LightObject::pyattr_get_shadow_active()
{
	return m_lightobj->HasShadowBuffer();
}

float KX_LightObject::pyattr_get_distance()
{
	return m_lightobj->m_distance;
}

void KX_LightObject::pyattr_set_distance(float value)
{
	m_lightobj->m_distance = value;
}

mt::vec3 KX_LightObject::pyattr_get_color()
{
	return m_lightobj->m_color;
}

void KX_LightObject::pyattr_set_color(const mt::vec3& value)
{
	m_lightobj->m_color = value;
}

float KX_LightObject::pyattr_get_lin_attenuation()
{
	return m_lightobj->m_att1;
}

void KX_LightObject::pyattr_set_lin_attenuation(float value)
{
	m_lightobj->m_att1 = value;
}

float KX_LightObject::pyattr_get_quad_attenuation()
{
	return m_lightobj->m_att2;
}

void KX_LightObject::pyattr_set_quad_attenuation(float value)
{
	m_lightobj->m_att2 = value;
}

float KX_LightObject::pyattr_get_spotsize()
{
	return RAD2DEG(m_lightobj->m_spotsize);
}

void KX_LightObject::pyattr_set_spotsize(float value)
{
	m_lightobj->m_spotsize = DEG2RAD(value);
}

float KX_LightObject::pyattr_get_spotblend()
{
	return m_lightobj->m_spotblend;
}

void KX_LightObject::pyattr_set_spotblend(float value)
{
	m_lightobj->m_spotblend = value;
}

int KX_LightObject::pyattr_get_typeconst(const EXP_Attribute *attrdef)
{
	static const std::unordered_map<std::string, RAS_ILightObject::LightType> table = {
		{"SPOT", RAS_ILightObject::LIGHT_SPOT},
		{"SUN", RAS_ILightObject::LIGHT_SUN},
		{"NORMAL", RAS_ILightObject::LIGHT_NORMAL},
		{"HEMI", RAS_ILightObject::LIGHT_HEMI}
	};

	return table.at(attrdef->m_name);
}

int KX_LightObject::pyattr_get_type()
{
	return m_lightobj->m_type;
}

void KX_LightObject::pyattr_set_type(int value)
{
	switch (value) {
		case 0:
		{
			m_lightobj->m_type = m_lightobj->LIGHT_SPOT;
			break;
		}
		case 1:
		{
			m_lightobj->m_type = m_lightobj->LIGHT_SUN;
			break;
		}
		case 2:
		{
			m_lightobj->m_type = m_lightobj->LIGHT_NORMAL;
			break;
		}
		case 3:
		{
			m_lightobj->m_type = m_lightobj->LIGHT_HEMI;
			break;
		}
	}
}

bool KX_LightObject::pyattr_get_static_shadow()
{
	return m_lightobj->m_staticShadow;
}

void KX_LightObject::pyattr_set_static_shadow(bool value)
{
	m_lightobj->m_staticShadow = value;
}
#endif // WITH_PYTHON

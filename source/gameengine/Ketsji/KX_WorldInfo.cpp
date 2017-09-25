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

/** \file gameengine/Ketsji/KX_WorldInfo.cpp
 *  \ingroup ketsji
 */


#include "KX_WorldInfo.h"
#include "KX_PyMath.h"
#include "RAS_Rasterizer.h"
#include "GPU_material.h"

/* This little block needed for linking to Blender... */
#ifdef WIN32
#include "BLI_winstuff.h"
#endif

/* This list includes only data type definitions */
#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BLI_math.h"

#include "BKE_global.h"
#include "BKE_scene.h"
/* end of blender include block */

KX_WorldInfo::KX_WorldInfo(Scene *blenderscene, World *blenderworld)
	:m_scene(blenderscene)
{
	if (blenderworld) {
		m_name = blenderworld->id.name + 2;
		m_do_color_management = BKE_scene_check_color_management_enabled(blenderscene);
		m_hasworld = true;
		m_hasmist = ((blenderworld->mode) & WO_MIST ? true : false);
		m_hasEnvLight = ((blenderworld->mode) & WO_ENV_LIGHT ? true : false);
		m_savedData.horizonColor[0] = blenderworld->horr;
		m_savedData.horizonColor[1] = blenderworld->horg;
		m_savedData.horizonColor[2] = blenderworld->horb;
		m_savedData.zenithColor[0] = blenderworld->zenr;
		m_savedData.zenithColor[1] = blenderworld->zeng;
		m_savedData.zenithColor[2] = blenderworld->zenb;
		m_envLightEnergy = blenderworld->ao_env_energy;
		m_envLightColor = blenderworld->aocolor;
		m_misttype = blenderworld->mistype;
		m_miststart = blenderworld->miststa;
		m_mistdistance = blenderworld->mistdist;
		m_mistintensity = blenderworld->misi;
		setMistColor(MT_Vector3(blenderworld->horr, blenderworld->horg, blenderworld->horb));
		setHorizonColor(MT_Vector4(blenderworld->horr, blenderworld->horg, blenderworld->horb, 1.0f));
		setZenithColor(MT_Vector4(blenderworld->zenr, blenderworld->zeng, blenderworld->zenb, 1.0f));
		setAmbientColor(MT_Vector3(blenderworld->ambr, blenderworld->ambg, blenderworld->ambb));
		setExposure(blenderworld->exp);
		setRange(blenderworld->range);
	}
	else {
		m_hasworld = false;
	}
}

KX_WorldInfo::~KX_WorldInfo()
{
	// Restore saved horizon and zenith colors
	if (m_hasworld) {
		m_scene->world->horr = m_savedData.horizonColor[0];
		m_scene->world->horg = m_savedData.horizonColor[1];
		m_scene->world->horb = m_savedData.horizonColor[2];
		m_scene->world->zenr = m_savedData.zenithColor[0];
		m_scene->world->zeng = m_savedData.zenithColor[1];
		m_scene->world->zenb = m_savedData.zenithColor[2];
	}
}

std::string KX_WorldInfo::GetName()
{
	return m_name;
}

bool KX_WorldInfo::hasWorld()
{
	return m_hasworld;
}

void KX_WorldInfo::setHorizonColor(const MT_Vector4& horizoncolor)
{
	m_horizoncolor = horizoncolor;
}

void KX_WorldInfo::setZenithColor(const MT_Vector4& zenithcolor)
{
	m_zenithcolor = zenithcolor;
}

void KX_WorldInfo::setMistStart(float d)
{
	m_miststart = d;
}

void KX_WorldInfo::setMistDistance(float d)
{
	m_mistdistance = d;
}

void KX_WorldInfo::setMistIntensity(float intensity)
{
	m_mistintensity = intensity;
}

void KX_WorldInfo::setExposure(float exposure)
{
	m_exposure = exposure;
}

void KX_WorldInfo::setRange(float range)
{
	m_range = range;
}

void KX_WorldInfo::setMistColor(const MT_Vector3& mistcolor)
{
	m_mistcolor = mistcolor;

	if (m_do_color_management) {
		linearrgb_to_srgb_v3_v3(m_con_mistcolor.getValue(), m_mistcolor.getValue());
	}
	else {
		m_con_mistcolor = m_mistcolor;
	}
}

void KX_WorldInfo::setAmbientColor(const MT_Vector3& ambientcolor)
{
	m_ambientcolor = ambientcolor;

	if (m_do_color_management) {
		linearrgb_to_srgb_v3_v3(m_con_ambientcolor.getValue(), m_ambientcolor.getValue());
	}
	else {
		m_con_ambientcolor = m_ambientcolor;
	}
}

void KX_WorldInfo::UpdateBackGround(RAS_Rasterizer *rasty)
{
	if (m_hasworld) {
		// Update World values for world material created in GPU_material_world/GPU_material_old_world.
		m_scene->world->zenr = m_zenithcolor[0];
		m_scene->world->zeng = m_zenithcolor[1];
		m_scene->world->zenb = m_zenithcolor[2];
		m_scene->world->horr = m_horizoncolor[0];
		m_scene->world->horg = m_horizoncolor[1];
		m_scene->world->horb = m_horizoncolor[2];

		// Update GPUWorld values for regular materials.
		GPU_horizon_update_color(m_horizoncolor.getValue());
		GPU_zenith_update_color(m_zenithcolor.getValue());
	}
}

void KX_WorldInfo::UpdateWorldSettings(RAS_Rasterizer *rasty)
{
	if (m_hasworld) {
		rasty->SetAmbientColor(m_con_ambientcolor);
		GPU_ambient_update_color(m_ambientcolor.getValue());
		GPU_update_exposure_range(m_exposure, m_range);
		GPU_update_envlight_energy(m_envLightEnergy);

		if (m_hasmist) {
			rasty->SetFog(m_misttype, m_miststart, m_mistdistance, m_mistintensity, m_con_mistcolor);
			GPU_mist_update_values(m_misttype, m_miststart, m_mistdistance, m_mistintensity, m_mistcolor.getValue());
			GPU_mist_update_enable(true);
		}
		else {
			GPU_mist_update_enable(false);
		}
	}
}

void KX_WorldInfo::RenderBackground(RAS_Rasterizer *rasty)
{
	if (m_hasworld) {
		if (m_scene->world->skytype & (WO_SKYBLEND | WO_SKYPAPER | WO_SKYREAL)) {
			GPUMaterial *gpumat = GPU_material_world(m_scene, m_scene->world);
			float viewmat[4][4];
			rasty->GetViewMatrix().getValue(&viewmat[0][0]);
			float invviewmat[4][4];
			rasty->GetViewInvMatrix().getValue(&invviewmat[0][0]);

			static float texcofac[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
			GPU_material_bind(gpumat, 0xFFFFFFFF, m_scene->lay, 1.0f, false, viewmat, invviewmat, texcofac, false);

			rasty->Disable(RAS_Rasterizer::RAS_CULL_FACE);
			rasty->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
			rasty->SetDepthFunc(RAS_Rasterizer::RAS_ALWAYS);

			rasty->DrawOverlayPlane();

			rasty->SetDepthFunc(RAS_Rasterizer::RAS_LEQUAL);
			rasty->Enable(RAS_Rasterizer::RAS_CULL_FACE);

			GPU_material_unbind(gpumat);
		}
		else {
			if (m_do_color_management) {
				float srgbcolor[4];
				linearrgb_to_srgb_v4(srgbcolor, m_horizoncolor.getValue());
				rasty->SetClearColor(srgbcolor[0], srgbcolor[1], srgbcolor[2], srgbcolor[3]);
			}
			else {
				rasty->SetClearColor(m_horizoncolor[0], m_horizoncolor[1], m_horizoncolor[2], m_horizoncolor[3]);
			}
			rasty->Clear(RAS_Rasterizer::RAS_COLOR_BUFFER_BIT);
		}
	}
	// Else render a dummy gray background.
	else {
		/* Grey color computed by linearrgb_to_srgb_v3_v3 with a color of
		 * 0.050, 0.050, 0.050 (the default world horizon color).
		 */
		rasty->SetClearColor(0.247784f, 0.247784f, 0.247784f, 1.0f);
		rasty->Clear(RAS_Rasterizer::RAS_COLOR_BUFFER_BIT);
	}
}

#ifdef WITH_PYTHON

/* -------------------------------------------------------------------------
 * Python functions
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Python Integration Hooks
 * ------------------------------------------------------------------------- */
PyTypeObject KX_WorldInfo::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_WorldInfo",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,0,0,0,0,0,0,0,0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0,0,0,0,0,0,0,
	Methods,
	0,
	0,
	&PyObjectPlus::Type,
	0,0,0,0,0,0,
	py_base_new
};

PyMethodDef KX_WorldInfo::Methods[] = {
	{nullptr,nullptr} /* Sentinel */
};

PyAttributeDef KX_WorldInfo::Attributes[] = {
	KX_PYATTRIBUTE_BOOL_RW("mistEnable", KX_WorldInfo, m_hasmist),
	KX_PYATTRIBUTE_FLOAT_RW("mistStart", 0.0f, 10000.0f, KX_WorldInfo, m_miststart),
	KX_PYATTRIBUTE_FLOAT_RW("mistDistance", 0.001f, 10000.0f, KX_WorldInfo, m_mistdistance),
	KX_PYATTRIBUTE_FLOAT_RW("mistIntensity", 0.0f, 1.0f, KX_WorldInfo, m_mistintensity),
	KX_PYATTRIBUTE_SHORT_RW("mistType", 0, 2, true, KX_WorldInfo, m_misttype),
	KX_PYATTRIBUTE_RO_FUNCTION("KX_MIST_QUADRATIC", KX_WorldInfo, pyattr_get_mist_typeconst),
	KX_PYATTRIBUTE_RO_FUNCTION("KX_MIST_LINEAR", KX_WorldInfo, pyattr_get_mist_typeconst),
	KX_PYATTRIBUTE_RO_FUNCTION("KX_MIST_INV_QUADRATIC", KX_WorldInfo, pyattr_get_mist_typeconst),
	KX_PYATTRIBUTE_RW_FUNCTION("mistColor", KX_WorldInfo, pyattr_get_mist_color, pyattr_set_mist_color),
	KX_PYATTRIBUTE_RW_FUNCTION("horizonColor", KX_WorldInfo, pyattr_get_horizon_color, pyattr_set_horizon_color),
	KX_PYATTRIBUTE_RW_FUNCTION("backgroundColor", KX_WorldInfo, pyattr_get_background_color, pyattr_set_background_color),
	KX_PYATTRIBUTE_RW_FUNCTION("zenithColor", KX_WorldInfo, pyattr_get_zenith_color, pyattr_set_zenith_color),
	KX_PYATTRIBUTE_RW_FUNCTION("ambientColor", KX_WorldInfo, pyattr_get_ambient_color, pyattr_set_ambient_color),
	KX_PYATTRIBUTE_FLOAT_RW("exposure", 0.0f, 1.0f, KX_WorldInfo, m_exposure),
	KX_PYATTRIBUTE_FLOAT_RW("range", 0.2f, 5.0f, KX_WorldInfo, m_range),
	KX_PYATTRIBUTE_FLOAT_RW("envLightEnergy", 0.0f, FLT_MAX, KX_WorldInfo, m_envLightEnergy),
	KX_PYATTRIBUTE_BOOL_RO("envLightEnabled", KX_WorldInfo, m_hasEnvLight),
	KX_PYATTRIBUTE_SHORT_RO("envLightColor", KX_WorldInfo, m_envLightColor),
	KX_PYATTRIBUTE_NULL /* Sentinel */
};

/* Attribute get/set functions */

#ifdef USE_MATHUTILS

/*----------------------mathutils callbacks ----------------------------*/

/* subtype */
#define MATHUTILS_COL_CB_MIST_COLOR 1
#define MATHUTILS_COL_CB_HOR_COLOR 2
#define MATHUTILS_COL_CB_BACK_COLOR 3
#define MATHUTILS_COL_CB_AMBIENT_COLOR 4
#define MATHUTILS_COL_CB_ZEN_COLOR 5

static unsigned char mathutils_world_color_cb_index = -1; /* index for our callbacks */

static int mathutils_world_generic_check(BaseMathObject *bmo)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>BGE_PROXY_REF(bmo->cb_user);
	if (self == nullptr)
		return -1;
		
	return 0;
}

static int mathutils_world_color_get(BaseMathObject *bmo, int subtype)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>BGE_PROXY_REF(bmo->cb_user);
	if (self == nullptr)
		return -1;

	switch (subtype) {
		case MATHUTILS_COL_CB_MIST_COLOR:
			self->m_mistcolor.getValue(bmo->data);
			break;
		case MATHUTILS_COL_CB_HOR_COLOR:
		case MATHUTILS_COL_CB_BACK_COLOR:
			self->m_horizoncolor.getValue(bmo->data);
			break;
		case MATHUTILS_COL_CB_ZEN_COLOR:
			self->m_zenithcolor.getValue(bmo->data);
			break;
		case MATHUTILS_COL_CB_AMBIENT_COLOR:
			self->m_ambientcolor.getValue(bmo->data);
			break;
	default:
		return -1;
	}
	return 0;
}

static int mathutils_world_color_set(BaseMathObject *bmo, int subtype)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>BGE_PROXY_REF(bmo->cb_user);

	if (self == nullptr)
		return -1;

	switch (subtype) {
		case MATHUTILS_COL_CB_MIST_COLOR:
			self->setMistColor(MT_Vector3(bmo->data));
			break;
		case MATHUTILS_COL_CB_HOR_COLOR:
			self->setHorizonColor(MT_Vector4(bmo->data));
			break;
		case MATHUTILS_COL_CB_BACK_COLOR:
			self->setHorizonColor(MT_Vector4(bmo->data[0], bmo->data[1], bmo->data[2], 1.0f));
			break;
		case MATHUTILS_COL_CB_ZEN_COLOR:
			self->setZenithColor(MT_Vector4(bmo->data));
			break;
		case MATHUTILS_COL_CB_AMBIENT_COLOR:
			self->setAmbientColor(MT_Vector3(bmo->data));
			break;
	default:
		return -1;
	}
	return 0;
}

static int mathutils_world_color_get_index(BaseMathObject *bmo, int subtype, int index)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>BGE_PROXY_REF(bmo->cb_user);

	if (self == nullptr)
		return -1;

	switch (subtype) {
		case MATHUTILS_COL_CB_MIST_COLOR:
		{
			bmo->data[index] = self->m_mistcolor[index];
		}
		break;
		case MATHUTILS_COL_CB_HOR_COLOR:
		case MATHUTILS_COL_CB_BACK_COLOR:
		{
			bmo->data[index] = self->m_horizoncolor[index];
		}
			break;
		case MATHUTILS_COL_CB_ZEN_COLOR:
		{
			bmo->data[index] = self->m_zenithcolor[index];
		}
		break;
		case MATHUTILS_COL_CB_AMBIENT_COLOR:
		{
			bmo->data[index] = self->m_ambientcolor[index];
		}
		break;
	default:
		return -1;
	}
	return 0;
}

static int mathutils_world_color_set_index(BaseMathObject *bmo, int subtype, int index)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>BGE_PROXY_REF(bmo->cb_user);

	if (self == nullptr)
		return -1;

	switch (subtype) {
		case MATHUTILS_COL_CB_MIST_COLOR:
		{
			MT_Vector3 color = self->m_mistcolor;
			color[index] = bmo->data[index];
			self->setMistColor(color);
			break;
		}
		case MATHUTILS_COL_CB_HOR_COLOR:
		case MATHUTILS_COL_CB_BACK_COLOR:
		{
			MT_Vector4 color = self->m_horizoncolor;
			color[index] = bmo->data[index];
			CLAMP(color[0], 0.0f, 1.0f);
			CLAMP(color[1], 0.0f, 1.0f);
			CLAMP(color[2], 0.0f, 1.0f);
			CLAMP(color[3], 0.0f, 1.0f);
			self->setHorizonColor(color);
			break;
		}
		case MATHUTILS_COL_CB_ZEN_COLOR:
		{
			MT_Vector4 color = self->m_zenithcolor;
			color[index] = bmo->data[index];
			CLAMP(color[0], 0.0f, 1.0f);
			CLAMP(color[1], 0.0f, 1.0f);
			CLAMP(color[2], 0.0f, 1.0f);
			CLAMP(color[3], 0.0f, 1.0f);
			self->setZenithColor(color);
			break;
		}
		case MATHUTILS_COL_CB_AMBIENT_COLOR:
		{
			MT_Vector3 color = self->m_ambientcolor;
			color[index] = bmo->data[index];
			self->setAmbientColor(color);
			break;
		}
	default:
		return -1;
	}
	return 0;
}

static Mathutils_Callback mathutils_world_color_cb = {
	mathutils_world_generic_check,
	mathutils_world_color_get,
	mathutils_world_color_set,
	mathutils_world_color_get_index,
	mathutils_world_color_set_index
};

void KX_WorldInfo_Mathutils_Callback_Init()
{
	// register mathutils callbacks, ok to run more than once.
	mathutils_world_color_cb_index = Mathutils_RegisterCallback(&mathutils_world_color_cb);
}
#endif // USE_MATHUTILS

PyObject *KX_WorldInfo::pyattr_get_mist_typeconst(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	PyObject *retvalue;

	const std::string& type = attrdef->m_name;

	if (type == "KX_MIST_QUADRATIC") {
		retvalue = PyLong_FromLong(KX_MIST_QUADRATIC);
	} 
	else if (type == "KX_MIST_LINEAR") {
		retvalue = PyLong_FromLong(KX_MIST_LINEAR);
	} 
	else if (type == "KX_MIST_INV_QUADRATIC") {
		retvalue = PyLong_FromLong(KX_MIST_INV_QUADRATIC);
	}
	else {
		/* should never happen */
		PyErr_SetString(PyExc_TypeError, "invalid mist type");
		retvalue = nullptr;
	}

	return retvalue;
}

PyObject *KX_WorldInfo::pyattr_get_mist_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(
	        BGE_PROXY_FROM_REF_BORROW(self_v),
	        mathutils_world_color_cb_index, MATHUTILS_COL_CB_MIST_COLOR);
#else
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);
	return PyObjectFrom(self->m_mistcolor);
#endif
}

int KX_WorldInfo::pyattr_set_mist_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);

	MT_Vector3 color;
	if (PyVecTo(value, color))
	{
		self->setMistColor(color);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_WorldInfo::pyattr_get_horizon_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{

#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
	        BGE_PROXY_FROM_REF_BORROW(self_v), 4,
	        mathutils_world_color_cb_index, MATHUTILS_COL_CB_HOR_COLOR);
#else
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);
	return PyObjectFrom(self->m_horizoncolor);
#endif
}

int KX_WorldInfo::pyattr_set_horizon_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);

	MT_Vector4 color;
	if (PyVecTo(value, color))
	{
		self->setHorizonColor(color);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_WorldInfo::pyattr_get_background_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(
	        BGE_PROXY_FROM_REF_BORROW(self_v),
	        mathutils_world_color_cb_index, MATHUTILS_COL_CB_BACK_COLOR);
#else
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);
	return PyObjectFrom(self->m_horizoncolor.to3d());
#endif
}

int KX_WorldInfo::pyattr_set_background_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);

	MT_Vector3 color;
	if (PyVecTo(value, color)) {
		self->setHorizonColor(MT_Vector4(color[0], color[1], color[2], 1.0f));
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_WorldInfo::pyattr_get_zenith_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{

#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		BGE_PROXY_FROM_REF_BORROW(self_v), 4,
		mathutils_world_color_cb_index, MATHUTILS_COL_CB_ZEN_COLOR);
#else
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);
	return PyObjectFrom(self->m_zenithcolor);
#endif
}

int KX_WorldInfo::pyattr_set_zenith_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);

	MT_Vector4 color;
	if (PyVecTo(value, color))
	{
		self->setZenithColor(color);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_WorldInfo::pyattr_get_ambient_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(
	        BGE_PROXY_FROM_REF_BORROW(self_v),
	        mathutils_world_color_cb_index, MATHUTILS_COL_CB_AMBIENT_COLOR);
#else
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);
	return PyObjectFrom(self->m_ambientcolor);
#endif
}

int KX_WorldInfo::pyattr_set_ambient_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_WorldInfo *self = static_cast<KX_WorldInfo*>(self_v);

	MT_Vector3 color;
	if (PyVecTo(value, color))
	{
		self->setAmbientColor(color);
		return PY_SET_ATTR_SUCCESS;
	}
	return PY_SET_ATTR_FAIL;
}

#endif /* WITH_PYTHON */

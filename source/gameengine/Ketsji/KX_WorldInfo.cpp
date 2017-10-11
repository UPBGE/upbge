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
		setMistColor(mt::vec3(blenderworld->horr, blenderworld->horg, blenderworld->horb));
		setHorizonColor(mt::vec4(blenderworld->horr, blenderworld->horg, blenderworld->horb, 1.0f));
		setZenithColor(mt::vec4(blenderworld->zenr, blenderworld->zeng, blenderworld->zenb, 1.0f));
		setAmbientColor(mt::vec3(blenderworld->ambr, blenderworld->ambg, blenderworld->ambb));
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

std::string KX_WorldInfo::GetName() const
{
	return m_name;
}

bool KX_WorldInfo::hasWorld()
{
	return m_hasworld;
}

void KX_WorldInfo::setHorizonColor(const mt::vec4& horizoncolor)
{
	m_horizoncolor = horizoncolor;
}

void KX_WorldInfo::setZenithColor(const mt::vec4& zenithcolor)
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

void KX_WorldInfo::setMistColor(const mt::vec3& mistcolor)
{
	m_mistcolor = mistcolor;

	if (m_do_color_management) {
		float col[3];
		linearrgb_to_srgb_v3_v3(col, m_mistcolor.Data());
		m_con_mistcolor = mt::vec3(col);
	}
	else {
		m_con_mistcolor = m_mistcolor;
	}
}

void KX_WorldInfo::setAmbientColor(const mt::vec3& ambientcolor)
{
	m_ambientcolor = ambientcolor;

	if (m_do_color_management) {
		float col[3];
		linearrgb_to_srgb_v3_v3(col, m_ambientcolor.Data());
		m_con_ambientcolor = mt::vec3(col);
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
		GPU_horizon_update_color(m_horizoncolor.Data());
		GPU_zenith_update_color(m_zenithcolor.Data());
	}
}

void KX_WorldInfo::UpdateWorldSettings(RAS_Rasterizer *rasty)
{
	if (m_hasworld) {
		rasty->SetAmbientColor(m_con_ambientcolor);
		GPU_ambient_update_color(m_ambientcolor.Data());
		GPU_update_exposure_range(m_exposure, m_range);
		GPU_update_envlight_energy(m_envLightEnergy);

		if (m_hasmist) {
			rasty->SetFog(m_misttype, m_miststart, m_mistdistance, m_mistintensity, m_con_mistcolor);
			GPU_mist_update_values(m_misttype, m_miststart, m_mistdistance, m_mistintensity, m_mistcolor.Data());
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

			static float texcofac[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
			GPU_material_bind(gpumat, m_scene->lay, 1.0f, false, rasty->GetViewMatrix().Data(),
			                  rasty->GetViewInvMatrix().Data(), texcofac, false);

			rasty->SetFrontFace(true);
			rasty->Enable(RAS_Rasterizer::RAS_DEPTH_TEST);
			rasty->SetDepthFunc(RAS_Rasterizer::RAS_ALWAYS);

			rasty->DrawOverlayPlane();

			rasty->SetDepthFunc(RAS_Rasterizer::RAS_LEQUAL);

			GPU_material_unbind(gpumat);
		}
		else {
			if (m_do_color_management) {
				float srgbcolor[4];
				linearrgb_to_srgb_v4(srgbcolor, m_horizoncolor.Data());
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

PyMethodDef KX_WorldInfo::Methods[] = {
	{nullptr, nullptr} /* Sentinel */
};

EXP_Attribute KX_WorldInfo::Attributes[] = {
	EXP_ATTRIBUTE_RW("mistEnable", m_hasmist),
	EXP_ATTRIBUTE_RW_RANGE("mistStart",m_miststart, 0.0f, 10000.0f, false),
	EXP_ATTRIBUTE_RW_RANGE("mistDistance", m_mistdistance, 0.001f, 10000.0f, false),
	EXP_ATTRIBUTE_RW_RANGE("mistIntensity", m_mistintensity, 0.0f, 1.0f, false),
	EXP_ATTRIBUTE_RW_RANGE("mistType", m_misttype,  0, 2, false),
	EXP_ATTRIBUTE_RO_FUNCTION("KX_MIST_QUADRATIC", pyattr_get_mist_typeconst),
	EXP_ATTRIBUTE_RO_FUNCTION("KX_MIST_LINEAR", pyattr_get_mist_typeconst),
	EXP_ATTRIBUTE_RO_FUNCTION("KX_MIST_INV_QUADRATIC", pyattr_get_mist_typeconst),
	EXP_ATTRIBUTE_RW_FUNCTION("mistColor", pyattr_get_mist_color, pyattr_set_mist_color),
	EXP_ATTRIBUTE_RW_FUNCTION("horizonColor", pyattr_get_horizon_color, pyattr_set_horizon_color),
	EXP_ATTRIBUTE_RW_FUNCTION("backgroundColor", pyattr_get_background_color, pyattr_set_background_color),
	EXP_ATTRIBUTE_RW_FUNCTION("zenithColor", pyattr_get_zenith_color, pyattr_set_zenith_color),
	EXP_ATTRIBUTE_RW_FUNCTION("ambientColor", pyattr_get_ambient_color, pyattr_set_ambient_color),
	EXP_ATTRIBUTE_RW_RANGE("exposure", m_exposure, 0.0f, 1.0f, false),
	EXP_ATTRIBUTE_RW_RANGE("range", m_range, 0.2f, 5.0f, false),
	EXP_ATTRIBUTE_RW_RANGE("envLightEnergy", m_envLightEnergy, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RO("envLightEnabled", m_hasEnvLight),
	EXP_ATTRIBUTE_RO("envLightColor", m_envLightColor),
	EXP_ATTRIBUTE_NULL /* Sentinel */
};

int KX_WorldInfo::pyattr_get_mist_typeconst(const EXP_Attribute *attrdef)
{
	static const std::unordered_map<std::string, MistType> table = {
		{"KX_MIST_QUADRATIC", KX_MIST_QUADRATIC},
		{"KX_MIST_LINEAR", KX_MIST_LINEAR},
		{"KX_MIST_INV_QUADRATIC", KX_MIST_INV_QUADRATIC}
	};

	return table.at(attrdef->m_name);
}

mt::vec3 KX_WorldInfo::pyattr_get_mist_color()
{
	return m_mistcolor;
}

void KX_WorldInfo::pyattr_set_mist_color(const mt::vec3& value)
{
	setMistColor(value);
}

mt::vec4 KX_WorldInfo::pyattr_get_horizon_color()
{
	return m_horizoncolor;
}

void KX_WorldInfo::pyattr_set_horizon_color(const mt::vec4& value)
{
	setHorizonColor(value);
}

mt::vec3 KX_WorldInfo::pyattr_get_background_color()
{
	return m_horizoncolor.xyz();
}

void KX_WorldInfo::pyattr_set_background_color(const mt::vec3& value)
{
	setHorizonColor(mt::vec4(value.x, value.y, value.z, 1.0f));
}

mt::vec4 KX_WorldInfo::pyattr_get_zenith_color()
{
	return m_zenithcolor;
}

void KX_WorldInfo::pyattr_set_zenith_color(const mt::vec4& value)
{
	setZenithColor(value);
}

mt::vec3 KX_WorldInfo::pyattr_get_ambient_color()
{
	return m_ambientcolor;
}

void KX_WorldInfo::pyattr_set_ambient_color(const mt::vec3& value)
{
	setAmbientColor(value);
}

#endif /* WITH_PYTHON */

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

/** \file KX_WorldInfo.h
 *  \ingroup ketsji
 */

#ifndef __KX_WORLDINFO_H__
#define __KX_WORLDINFO_H__

#include "mathfu.h"
#include "EXP_Value.h"

#ifdef USE_MATHUTILS
void KX_WorldInfo_Mathutils_Callback_Init(void);
#endif

class RAS_Rasterizer;
struct Scene;
struct World;

class KX_WorldInfo : public EXP_Value, public mt::SimdClassAllocator
{
	Py_Header

	std::string m_name;
	Scene *m_scene;
	bool m_do_color_management;
	bool m_hasworld;
	bool m_hasmist;
	bool m_hasEnvLight;
	short m_misttype;
	short m_envLightColor;
	float m_miststart;
	float m_mistdistance;
	float m_mistintensity;
	float m_range;
	float m_exposure;
	float m_envLightEnergy;
	mt::vec3 m_mistcolor;
	mt::vec4 m_horizoncolor;
	mt::vec4 m_zenithcolor;
	mt::vec3 m_ambientcolor;
	mt::vec3 m_con_mistcolor;
	mt::vec3 m_con_ambientcolor;

public:
	/**
	 * Mist options
	 */
	enum MistType {
		KX_MIST_QUADRATIC,
		KX_MIST_LINEAR,
		KX_MIST_INV_QUADRATIC,
	};

	KX_WorldInfo(Scene *blenderscene, World *blenderworld);
	~KX_WorldInfo();

	struct {
		mt::vec3 horizonColor;
		mt::vec3 zenithColor;
	} m_savedData;

	virtual std::string GetName() const;
	bool hasWorld();
	void setMistStart(float d);
	void setMistDistance(float d);
	void setMistIntensity(float intensity);
	void setExposure(float exposure);
	void setRange(float range);
	void setMistColor(const mt::vec3& mistcolor);
	void setHorizonColor(const mt::vec4& horizoncolor);
	void setZenithColor(const mt::vec4& zenithcolor);
	void setAmbientColor(const mt::vec3& ambientcolor);
	void UpdateBackGround(RAS_Rasterizer *rasty);
	void UpdateWorldSettings(RAS_Rasterizer *rasty);
	void RenderBackground(RAS_Rasterizer *rasty);

#ifdef WITH_PYTHON
	/* attributes */
	static PyObject *pyattr_get_mist_typeconst(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_mist_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_mist_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_horizon_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_horizon_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_background_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_background_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_zenith_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_zenith_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_ambient_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_ambient_color(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif
};

#endif  /* __KX_WORLDINFO_H__ */

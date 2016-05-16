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

#include "MT_Scalar.h"
#include "MT_Vector3.h"
#include "KX_KetsjiEngine.h"
#include "EXP_PyObjectPlus.h"

#ifdef USE_MATHUTILS
void KX_WorldInfo_Mathutils_Callback_Init(void);
#endif

struct Scene;
struct World;

class KX_WorldInfo : public PyObjectPlus
{
	Py_Header

	STR_String m_name;
	Scene *m_scene;
	bool m_do_color_management;
	bool m_hasworld;
	bool m_hasmist;
	short m_misttype;
	float m_miststart;
	float m_mistdistance;
	float m_mistintensity;
	MT_Vector3 m_mistcolor;
	MT_Vector3 m_horizoncolor;
	MT_Vector3 m_zenithcolor;
	MT_Vector3 m_ambientcolor;
	MT_Vector3 m_con_mistcolor;
	MT_Vector3 m_con_ambientcolor;

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
		MT_Vector3 horizonColor;
		MT_Vector3 zenithColor;
	} m_savedData;

	const STR_String &GetName();
	bool hasWorld();
	void setUseMist(bool enable);
	void setMistType(short type);
	void setMistStart(float d);
	void setMistDistance(float d);
	void setMistIntensity(float intensity);
	void setMistColor(const MT_Vector3& mistcolor);
	void setHorizonColor(const MT_Vector3& horizoncolor);
	void setZenithColor(const MT_Vector3& zenithcolor);
	void setAmbientColor(const MT_Vector3& ambientcolor);
	void UpdateBackGround(RAS_IRasterizer *rasty);
	void UpdateWorldSettings(RAS_IRasterizer *rasty);
	void RenderBackground(RAS_IRasterizer *rasty);

#ifdef WITH_PYTHON
	/* attributes */
	static PyObject *pyattr_get_mist_typeconst(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_mist_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_mist_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_horizon_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_horizon_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_zenith_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_zenith_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_ambient_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_ambient_color(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	virtual PyObject *py_repr(void);
#endif
};

#endif  /* __KX_WORLDINFO_H__ */

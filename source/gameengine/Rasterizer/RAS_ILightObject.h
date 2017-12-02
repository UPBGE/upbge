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
 * Contributor(s): Mitchell Stokes
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_ILightObject.h
 *  \ingroup bgerast
 */

#ifndef __RAS_LIGHTOBJECT_H__
#define __RAS_LIGHTOBJECT_H__

#include "MT_Vector2.h"

/* eevee utils (put this in master class so it
 * can be used both in RAS_OpenGLLight and
 * KX_KetsjiEngine
 */
#define LERP(t, a, b) ((a) + (t) * ((b) - (a)))
#define MAX_CASCADE_NUM 4

typedef struct EEVEE_LightData {
	short light_id, shadow_id;
} EEVEE_LightData;

typedef struct EEVEE_ShadowCubeData {
	short light_id, shadow_id, cube_id, layer_id;
} EEVEE_ShadowCubeData;

typedef struct EEVEE_ShadowCascadeData {
	short light_id, shadow_id, cascade_id, layer_id;
	float viewprojmat[MAX_CASCADE_NUM][4][4]; /* World->Lamp->NDC : used for rendering the shadow map. */
	float radius[MAX_CASCADE_NUM];
} EEVEE_ShadowCascadeData;
/* end of eevee utils */

class RAS_Rasterizer;

class MT_Vector3;
class MT_Transform;
class MT_Matrix4x4;
class MT_Matrix3x3;

/* Note about these KX in RAS file:
 * I don't think it's abnormal to keep
 * KX here as RAS_ILightObject is used
 * both in RAS_OpenGLLight and KX_KetsjiEngine
 * (youle)
 */
class KX_LightObject;
class KX_Scene;

struct EEVEE_Light;
struct EEVEE_LampsInfo;
struct EEVEE_LampEngineData;
struct Object;
struct Image;

class RAS_ILightObject
{
public:
	// WARNING: have to match DNA enums for shader identification.
	enum LightType {
		LIGHT_NORMAL = 0, // LA_LOCAL
		LIGHT_SUN = 1, // LA_SUN
		LIGHT_SPOT = 2, // LA_SPOT
		LIGHT_HEMI = 3, // LA_HEMI
		LIGHT_AREA = 4 // LA_AREA
	};

	enum AreaShapeType {
		AREA_SQUARE,
		AREA_RECT,
		AREA_CUBE,
		AREA_BOX
	};

	int		m_layer;

	float	m_energy;
	float	m_distance;
	bool m_hasShadow;
	float	m_shadowclipstart;
	float	m_shadowfrustumsize;
	float	m_shadowclipend;
	float	m_shadowbias;
	float m_shadowBleedExp;
	float	m_shadowbleedbias;
	short	m_shadowmaptype;
	float	m_shadowcolor[3];

	float	m_color[3];

	float	m_att1;
	float	m_att2;
	float	m_coeff_const, m_coeff_lin, m_coeff_quad;
	float	m_spotsize;
	float	m_spotblend;

	MT_Vector2 m_areaSize;

	LightType	m_type;
	AreaShapeType m_areaShape;
	
	bool	m_nodiffuse;
	bool	m_nospecular;

	bool m_staticShadow;
	bool m_requestShadowUpdate;

	virtual ~RAS_ILightObject() {}
	virtual RAS_ILightObject* Clone() = 0;

	virtual bool HasShadow() const = 0;
	virtual bool NeedShadowUpdate() = 0;
	virtual int GetShadowBindCode() = 0;
	virtual MT_Matrix4x4 GetShadowMatrix() = 0;
	virtual MT_Matrix4x4 GetViewMat() = 0;
	virtual MT_Matrix4x4 GetWinMat() = 0;
	virtual int GetShadowLayer() = 0;
	virtual void UpdateLight(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led) = 0;
	virtual void UpdateShadowsCube(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led) = 0;
	virtual void UpdateShadowsCascade(KX_LightObject *kxlight, EEVEE_LampsInfo *linfo, EEVEE_LampEngineData *led, KX_Scene *scene) = 0;
	virtual Image *GetTextureImage(short texslot) = 0;
};

#endif  /* __RAS_LIGHTOBJECT_H__ */

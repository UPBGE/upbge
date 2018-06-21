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

#include "mathfu.h"

class RAS_ICanvas;

class KX_Camera;

struct Image;

class RAS_ILightObject
{
public:
	enum LightType {
		LIGHT_SPOT,
		LIGHT_SUN,
		LIGHT_NORMAL,
		LIGHT_HEMI
	};

	bool	m_modified;
	int		m_layer;
	void	*m_scene;
	void	*m_light;

	float	m_energy;
	float	m_distance;
	float	m_shadowclipstart;
	float	m_shadowfrustumsize;
	float	m_shadowclipend;
	float	m_shadowbias;
	float	m_shadowbleedbias;
	short	m_shadowmaptype;
	mt::vec3 m_shadowcolor;

	mt::vec3 m_color;

	float	m_att1;
	float	m_att2;
	float	m_coeff_const, m_coeff_lin, m_coeff_quad;
	float	m_spotsize;
	float	m_spotblend;

	LightType	m_type;
	
	bool	m_nodiffuse;
	bool	m_nospecular;

	bool m_staticShadow;
	bool m_requestShadowUpdate;

	virtual ~RAS_ILightObject() = default;
	virtual RAS_ILightObject* Clone() = 0;

	virtual bool HasShadowBuffer() = 0;
	virtual bool NeedShadowUpdate() = 0;
	virtual int GetShadowBindCode() = 0;
	virtual mt::mat4 GetShadowMatrix() = 0;
	virtual mt::mat4 GetViewMat() = 0;
	virtual mt::mat4 GetWinMat() = 0;
	virtual int GetShadowLayer() = 0;
	virtual void GetShadowMatrix(mt::mat4& viewMat, mt::mat4& projMat) = 0;
	virtual void BindShadowBuffer() = 0;
	virtual void UnbindShadowBuffer() = 0;
	virtual Image *GetTextureImage(short texslot) = 0;
	virtual void Update(const mt::mat3x4& trans, bool hide) = 0;
};

#endif  /* __RAS_LIGHTOBJECT_H__ */

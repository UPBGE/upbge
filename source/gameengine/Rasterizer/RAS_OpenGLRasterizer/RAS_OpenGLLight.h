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

#include "RAS_ILightObject.h"

struct GPULamp;
struct Image;
struct DRWShadingGroup;

class RAS_OpenGLLight : public RAS_ILightObject
{
// 	GPULamp *GetGPULamp();

	DRWShadingGroup *m_shGroup;

public:
	RAS_OpenGLLight();
	virtual ~RAS_OpenGLLight();

	RAS_OpenGLLight *Clone()
	{
		return new RAS_OpenGLLight(*this);
	}

	virtual bool HasShadow() const;
	virtual bool NeedShadowUpdate();
	virtual int GetShadowBindCode();
	virtual MT_Matrix4x4 GetViewMat();
	virtual MT_Matrix4x4 GetWinMat();
	virtual MT_Matrix4x4 GetShadowMatrix();
	virtual int GetShadowLayer();
	virtual void BindShadowBuffer(RAS_Rasterizer *rasty, const MT_Vector3& pos, Object *ob, EEVEE_LampsInfo *linfo,
		EEVEE_LampEngineData *led, RAS_SceneLayerData *layerData, int shadowid);
	virtual void UnbindShadowBuffer(RAS_Rasterizer *rasty, RAS_SceneLayerData *layerData, int shadowid);
	virtual Image *GetTextureImage(short texslot);
	virtual void Update(EEVEE_Light& lightData, int shadowid, const MT_Matrix3x3& rot, const MT_Vector3& pos, const MT_Vector3& scale);
	void SetShadowUpdateState(short state);
};

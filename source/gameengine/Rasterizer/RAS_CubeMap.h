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

/** \file RAS_CubeMap.h
*  \ingroup bgerast
*/

#include "glew-mx.h"
#include <vector>
#include <stdio.h>
#include "MT_Matrix4x4.h"

class KX_GameObject;
class RAS_Texture;
class RAS_IRasterizer;

struct GPUFrameBuffer;
struct GPUTexture;
struct MTex;

class RAS_CubeMap
{
private:
	RAS_Texture *m_texture;
	GPUTexture *m_cubeMapTexture;
	MTex *m_mtex;
	KX_GameObject *m_gameobj;
	GPUFrameBuffer *m_fbo;

	MT_Matrix4x4 m_proj;

	/* layer to cull */
	short m_layer;

public:
	RAS_CubeMap(KX_GameObject *gameobj, RAS_IRasterizer *rasty);
	virtual ~RAS_CubeMap();

	RAS_Texture *FindCubeMap();

	KX_GameObject *GetGameObj();

	void BeginRender();
	void EndRender();

	void BindFace(RAS_IRasterizer *rasty, unsigned short index, const MT_Vector3& objpos);
	void UnbindFace();

	void SetFaceViewMatPos(MT_Vector3 pos, int faceindex);

	MT_Matrix4x4 GetProj();
	short GetLayer();
};

class RAS_CubeMapManager
{
protected:
	
	std::vector<RAS_CubeMap *> m_cubeMaps;

public:
	RAS_CubeMapManager();
	virtual ~RAS_CubeMapManager();

	static MT_Matrix4x4 facesViewMat[6];
	static MT_Matrix3x3 camOri[6];
	static MT_Matrix3x3 camOri2[6];

	void AddCubeMap(RAS_CubeMap *cubeMap);
	void RemoveCubeMap(RAS_CubeMap *cubeMap);

	void RestoreFrameBuffer();
};


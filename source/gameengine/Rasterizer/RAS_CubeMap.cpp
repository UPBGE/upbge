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

/** \file RAS_CubeMap.cpp
*  \ingroup bgerast
*/

#include "RAS_CubeMap.h"
#include "RAS_MeshObject.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_IRasterizer.h"

#include "GPU_texture.h"
#include "GPU_framebuffer.h"
#include "GPU_draw.h"

#include "glew-mx.h"

#include "BKE_image.h"
#include "BKE_global.h"

#include "KX_GameObject.h"

MT_Matrix4x4 bottomFaceViewMat(
	-1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f);
MT_Matrix4x4 topFaceViewMat(
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, -1.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f);
MT_Matrix4x4 rightFaceViewMat(
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f,
	0.0f, -1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f);
MT_Matrix4x4 leftFaceViewMat(
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, -1.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f);
MT_Matrix4x4 backFaceViewMat(
	0.0f, 0.0f, 1.0f, 0.0f,
	0.0f, -1.0f, 0.0f, 0.0f,
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f);
MT_Matrix4x4 frontFaceViewMat(
	0.0f, 0.0f, -1.0f, 0.0f,
	0.0f, -1.0f, 0.0f, 0.0f,
	-1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 1.0f);

static MT_Matrix3x3 bottomCamOri(
	-1.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f,
	0.0f, 0.0f, 1.0f);
static MT_Matrix3x3 topCamOri(
	1.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f,
	0.0f, 0.0f, -1.0f);
static MT_Matrix3x3 rightCamOri(
	1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f,
	0.0f, -1.0f, 0.0f);
static MT_Matrix3x3 leftCamOri(
	1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, -1.0f,
	0.0f, 1.0f, 0.0f);
static MT_Matrix3x3 backCamOri(
	0.0f, 0.0f, 1.0f,
	0.0f, -1.0f, 0.0f,
	1.0f, 0.0f, 0.0f);
static MT_Matrix3x3 frontCamOri(
	0.0f, 0.0f, -1.0f,
	0.0f, -1.0f, 0.0f,
	-1.0f, 0.0f, 0.0f);

MT_Matrix4x4 RAS_CubeMapManager::facesViewMat[6] = { topFaceViewMat, bottomFaceViewMat, frontFaceViewMat, backFaceViewMat, rightFaceViewMat, leftFaceViewMat };
MT_Matrix3x3 RAS_CubeMapManager::camOri[6] = { topCamOri, bottomCamOri, frontCamOri, backCamOri, rightCamOri, leftCamOri };
MT_Matrix3x3 RAS_CubeMapManager::camOri2[6] = { topCamOri, bottomCamOri, frontCamOri, backCamOri, leftCamOri, rightCamOri };

RAS_CubeMap::RAS_CubeMap(void *clientobj, RAS_Texture *texture, RAS_IRasterizer *rasty)
	:m_texture(texture),
	m_cubeMapTexture(NULL),
	m_clientobj(clientobj)
{
	m_fbo = GPU_framebuffer_create();

	MTex *mtex = m_texture->GetMTex();
	float clipend = mtex->tex->env->clipend;
	m_layer = mtex->tex->env->notlay;

	m_cubeMapTexture = m_texture->GetGPUTexture();
	// Increment reference to make sure the gpu texture will not be freed by someone else.
	GPU_texture_ref(m_cubeMapTexture);

	m_proj = rasty->GetFrustumMatrix(-0.001f, 0.001f, -0.001f, 0.001f, 0.001f, clipend, 1.0f);
}

RAS_CubeMap::~RAS_CubeMap()
{
	GPU_texture_free(m_cubeMapTexture);
	GPU_free_image(m_texture->GetImage());
	GPU_framebuffer_free(m_fbo);
}

void *RAS_CubeMap::GetClientObject()
{
	return m_clientobj;
}

const MT_Matrix4x4& RAS_CubeMap::GetProjection()
{
	return m_proj;
}

short RAS_CubeMap::GetLayer()
{
	return m_layer;
}

void RAS_CubeMap::SetFaceViewMatPos(MT_Vector3 pos, int faceindex)
{
	MT_Vector3 newpos = RAS_CubeMapManager::camOri[faceindex] * pos;
	RAS_CubeMapManager::facesViewMat[faceindex][0][3] = -newpos[0];
	RAS_CubeMapManager::facesViewMat[faceindex][1][3] = -newpos[1];
	RAS_CubeMapManager::facesViewMat[faceindex][2][3] = -newpos[2];
}

void RAS_CubeMap::BeginRender()
{
}

void RAS_CubeMap::EndRender()
{
	GPU_texture_generate_mipmap(m_cubeMapTexture, 0);
}

void RAS_CubeMap::BindFace(RAS_IRasterizer *rasty, unsigned short index, const MT_Vector3& objpos)
{
	static GLenum m_cube_map_target[6] = {
		GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB,
		GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB,
		GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB,
	};

	GPU_framebuffer_texture_attach_target(m_fbo, m_cubeMapTexture, m_cube_map_target[index], 0, NULL);
	GPU_texture_bind_as_framebuffer(m_cubeMapTexture);

	rasty->Clear(RAS_IRasterizer::RAS_COLOR_BUFFER_BIT);

	SetFaceViewMatPos(objpos, index);
	rasty->SetCubeMatrix(RAS_CubeMapManager::facesViewMat[index]);
}

void RAS_CubeMap::UnbindFace()
{
	GPU_framebuffer_texture_detach(m_cubeMapTexture);
	GPU_framebuffer_texture_unbind(m_fbo, m_cubeMapTexture);
}

RAS_CubeMapManager::RAS_CubeMapManager()
{
}

RAS_CubeMapManager::~RAS_CubeMapManager()
{
	for (std::vector<RAS_CubeMap *>::iterator it = m_cubeMaps.begin(), end = m_cubeMaps.end(); it != end; ++it) {
		delete *it;
	}
}

void RAS_CubeMapManager::AddCubeMap(RAS_CubeMap *cubeMap)
{
	m_cubeMaps.push_back(cubeMap);
}

void RAS_CubeMapManager::RemoveCubeMap(void *clientobj)
{
	for (std::vector<RAS_CubeMap *>::iterator it = m_cubeMaps.begin(), end = m_cubeMaps.end(); it != end; ++it) {
		RAS_CubeMap *cubeMap = *it;
		if (cubeMap->GetClientObject() == clientobj) {
			delete cubeMap;
			m_cubeMaps.erase(it);
			break;
		}
	}
}

void RAS_CubeMapManager::RestoreFrameBuffer()
{
	GPU_framebuffer_restore();
}

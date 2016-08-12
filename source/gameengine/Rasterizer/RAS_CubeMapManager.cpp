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
* Contributor(s): Ulysse Martin, Tristan Porteries.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file RAS_CubeMapManager.cpp
*  \ingroup bgerast
*/

#include "RAS_CubeMapManager.h"
#include "RAS_CubeMap.h"
#include "RAS_IRasterizer.h"

#include "GPU_texture.h"
#include "GPU_framebuffer.h"
#include "GPU_draw.h"

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

MT_Matrix4x4 RAS_CubeMapManager::facesViewMat[6] = {topFaceViewMat, bottomFaceViewMat, frontFaceViewMat, backFaceViewMat, rightFaceViewMat, leftFaceViewMat};
MT_Matrix3x3 RAS_CubeMapManager::camOri[6] = {
	topFaceViewMat.to3x3(),
	bottomFaceViewMat.to3x3(),
	frontFaceViewMat.to3x3(),
	backFaceViewMat.to3x3(),
	leftFaceViewMat.to3x3(),
	rightFaceViewMat.to3x3()
};

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

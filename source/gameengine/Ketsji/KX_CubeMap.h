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

/** \file KX_CubeMap.h
 *  \ingroup ketsji
 */

#ifndef __KX_CUBEMAP_H__
#define __KX_CUBEMAP_H__

#include "KX_TextureRenderer.h"

class KX_CubeMap : public KX_TextureRenderer, public mt::SimdClassAllocator
{
	Py_Header

private:
	/// The camera projection matrix depending on clip start/end.
	mt::mat4 m_projection;
	/// True if the projection matrix is invalid and need to be recomputed.
	bool m_invalidProjection;

public:
	enum {
		NUM_FACES = 6
	};

	/// Face view matrices in 3x3 matrices.
	static const mt::mat3 faceViewMatrices3x3[NUM_FACES];

	KX_CubeMap(MTex *mtex, KX_GameObject *viewpoint);
	virtual ~KX_CubeMap();

	virtual std::string GetName();

	virtual void InvalidateProjectionMatrix();
	virtual mt::mat4 GetProjectionMatrix(RAS_Rasterizer *rasty, const KX_CameraRenderSchedule& cameraData);

	virtual LayerUsage EnsureLayers(int viewportCount);
	virtual bool PrepareFace(const mt::mat4& sceneViewMat, unsigned short face, mt::mat3x4& camTrans);
};

#endif  // __KX_CUBEMAP_H__

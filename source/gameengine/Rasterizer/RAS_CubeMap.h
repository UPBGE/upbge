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

/** \file RAS_CubeMap.h
*  \ingroup bgerast
*/

#ifndef __RAS_CUBEMAP_H__
#define __RAS_CUBEMAP_H__

#include "MT_Matrix4x4.h"

class RAS_Texture;
class RAS_IRasterizer;

struct GPUFrameBuffer;
struct GPURenderBuffer;
struct GPUTexture;

class RAS_CubeMap
{
private:
	/// Cube map texture to attach to frame buffer objects.
	GPUTexture *m_gpuTex;
	GPUFrameBuffer *m_fbos[6];
	GPURenderBuffer *m_rbs[6];

	// True if we regenerate mipmap every render.
	bool m_useMipmap;

	/// Recreate and attach frame buffer objects and render buffers to the cube map texture.
	void AttachTexture();
	/// Free and detach frame buffer objects and render buffer to the cube map texture.
	void DetachTexture();
	/** Obtain the latest cube map texture, if it's not the same as before,
	 * then detach the previous cube map texture and attach the new one.
	 */
	void GetValidTexture();

protected:
	/// The material texture owning the cube map texture.
	RAS_Texture *m_texture;

public:
	/// Face view matrices in 4x4 matrices.
	static const MT_Matrix4x4 faceViewMatrices4x4[6];
	/// Face view matrices in 3x3 matrices (used for camera orientation).
	static const MT_Matrix3x3 faceViewMatrices3x3[6];

	RAS_CubeMap(RAS_Texture *texture);
	virtual ~RAS_CubeMap();

	void BeginRender();
	void EndRender();

	void BindFace(RAS_IRasterizer *rasty, unsigned short index, const MT_Vector3& objpos);
	void UnbindFace();
};

#endif  // __RAS_CUBEMAP_H__

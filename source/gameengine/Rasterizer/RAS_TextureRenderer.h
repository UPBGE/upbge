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
* Contributor(s): Ulysse Martin, Tristan Porteries, Martins Upitis.
*
* ***** END GPL LICENSE BLOCK *****
*/

/** \file RAS_TextureRenderer.h
*  \ingroup bgerast
*/

#ifndef __RAS_TEXTURE_RENDERER_H__
#define __RAS_TEXTURE_RENDERER_H__

#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"

#include <vector>

class RAS_Texture;
class RAS_IRasterizer;

struct GPUFrameBuffer;
struct GPURenderBuffer;
struct GPUTexture;

class RAS_TextureRenderer
{
protected:
	class Face
	{
	private:
		/// Planar FBO and RBO
		GPUFrameBuffer *m_fbo;
		GPURenderBuffer *m_rb;
		int m_target;

	public:
		Face(int target);
		~Face();

		/// Bind the face frame buffer object.
		void Bind();
		/// Recreate and attach frame buffer object and render buffer to the texture.
		void AttachTexture(GPUTexture *tex);
		/// Free and detach frame buffer object and render buffer to the texture.
		void DetachTexture(GPUTexture *tex);
	};

	/// All the material texture users.
	std::vector<RAS_Texture *> m_textureUsers;

	std::vector<Face> m_faces;

private:
	/// Planar texture to attach to frame buffer objects.
	GPUTexture *m_gpuTex;
	/** Obtain the latest planar texture, if it's not the same as before,
	* then detach the previous planar texture and attach the new one.
	*/
	void GetValidTexture();

	/// Use mipmapping?
	bool m_useMipmap;

public:
	RAS_TextureRenderer();
	virtual ~RAS_TextureRenderer();

	unsigned short GetNumFaces() const;

	const std::vector<RAS_Texture *>& GetTextureUsers() const;

	void AddTextureUser(RAS_Texture *texture);

	virtual void BeginRender(RAS_IRasterizer *rasty);
	virtual void EndRender(RAS_IRasterizer *rasty);

	void BindFace(unsigned short index);
};

#endif  // __RAS_TEXTURE_RENDERER_H__

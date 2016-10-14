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

/** \file RAS_Planar.h
*  \ingroup bgerast
*/

#ifndef __RAS_PLANAR_H__
#define __RAS_PLANAR_H__

#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"

#include <vector>

class RAS_Texture;
class RAS_IRasterizer;
class RAS_IPolyMaterial;
class KX_GameObject;

struct GPUFrameBuffer;
struct GPURenderBuffer;
struct GPUTexture;

class RAS_Planar
{
public:


private:
	/// Planar texture to attach to frame buffer objects.
	GPUTexture *m_gpuTex;

	/// Planar FBO and RBO
	GPUFrameBuffer *m_fbo;
	GPURenderBuffer *m_rb;

	/// Recreate and attach frame buffer objects and render buffers to the planar texture.
	void AttachTexture();
	/// Free and detach frame buffer objects and render buffer to the planar texture.
	void DetachTexture();
	/** Obtain the latest planar texture, if it's not the same as before,
	* then detach the previous planar texture and attach the new one.
	*/
	void GetValidTexture();

	/// mirror center position in local space
	MT_Vector3 m_mirrorPos;

	/// mirror normal vector
	MT_Vector3 m_mirrorZ;

	/// Use mipmapping?
	bool m_useMipmap;

protected:
	/// All the material texture users.
	std::vector<RAS_Texture *> m_textureUsers;

public:

	RAS_Planar(KX_GameObject *gameobj, RAS_IPolyMaterial *mat);
	virtual ~RAS_Planar();

	const std::vector<RAS_Texture *>& GetTextureUsers() const;

	void AddTextureUser(RAS_Texture *texture);

	void BeginRender();
	void EndRender();

	void BindFace(RAS_IRasterizer *rasty);

	void EnableClipPlane(MT_Vector3 &mirrorWorldZ, MT_Scalar &mirrorPlaneDTerm, int planartype);
	void DisableClipPlane(int planartype);

	MT_Vector3 GetMirrorPos();
	MT_Vector3 GetMirrorZ();
};

#endif  // __RAS_PLANAR_H__

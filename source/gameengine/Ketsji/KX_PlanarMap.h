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

/** \file KX_PlanarMap.h
*  \ingroup ketsji
*/

#ifndef __KX_PLANAR_H__
#define __KX_PLANAR_H__

#include "KX_TextureRenderer.h"

class KX_PlanarMap : public KX_TextureRenderer, public mt::SimdClassAllocator
{
	Py_Header

private:
	/// Mirror normal vector.
	mt::vec3 m_normal;
	/// Clip plane equation values.
	mt::vec4 m_clipPlane;

	enum Type {
		REFLECTION,
		REFRACTION
	} m_type;

public:
	KX_PlanarMap(MTex *mtex, KX_GameObject *viewpoint);
	virtual ~KX_PlanarMap();

	virtual std::string GetName();

	void ComputeClipPlane(const mt::vec3& mirrorObjWorldPos, const mt::mat3& mirrorObjWorldOri);

	virtual void InvalidateProjectionMatrix();
	virtual mt::mat4 GetProjectionMatrix(RAS_Rasterizer *rasty, KX_Scene *scene, KX_Camera *sceneCamera,
			const RAS_Rect& viewport, const RAS_Rect& area, RAS_Rasterizer::StereoMode stereoMode, RAS_Rasterizer::StereoEye eye);

	const mt::vec3& GetNormal() const;
	void SetNormal(const mt::vec3& normal);

	virtual void BeginRender(RAS_Rasterizer *rasty, unsigned short layer);
	virtual void EndRender(RAS_Rasterizer *rasty, unsigned short layer);
	virtual void BeginRenderFace(RAS_Rasterizer *rasty, unsigned short layer, unsigned short face);

	virtual LayerUsage EnsureLayers(int viewportCount);
	virtual bool Prepare(KX_Camera *sceneCamera, RAS_Rasterizer::StereoEye eye, KX_Camera *camera);
	virtual bool PrepareFace(KX_Camera *camera, unsigned short index);

#ifdef WITH_PYTHON
	static PyObject *pyattr_get_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif  // WITH_PYTHON
};

#endif  // __KX_PLANAR_H__

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

#include <unordered_map>

class KX_PlanarMap : public KX_TextureRenderer, public mt::SimdClassAllocator
{
	Py_Header

private:
	/// Mirror normal vector.
	mt::vec3 m_normal;
	/// Clip plane equation values.
	mt::vec4 m_clipPlane;

	std::unordered_map<KX_Camera *, mt::mat4> m_projections;

	enum Type {
		REFLECTION,
		REFRACTION
	} m_type;

public:
	KX_PlanarMap(EnvMap *env, KX_GameObject *viewpoint);
	virtual ~KX_PlanarMap();

	virtual std::string GetName() const;

	void ComputeClipPlane(const mt::vec3& mirrorObjWorldPos, const mt::mat3& mirrorObjWorldOri);

	virtual void InvalidateProjectionMatrix();
	virtual const mt::mat4& GetProjectionMatrix(RAS_Rasterizer *rasty, KX_Scene *scene, KX_Camera *sceneCamera,
													const RAS_Rect& viewport, const RAS_Rect& area);

	virtual void BeginRenderFace(RAS_Rasterizer *rasty) override;
	virtual void EndRenderFace(RAS_Rasterizer *rasty) override;

	const mt::vec3& GetNormal() const;
	void SetNormal(const mt::vec3& normal);

	virtual bool SetupCamera(KX_Camera *sceneCamera, KX_Camera *camera);
	virtual bool SetupCameraFace(KX_Camera *camera, unsigned short index);

#ifdef WITH_PYTHON
	static PyObject *pyattr_get_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif  // WITH_PYTHON
};

#endif  // __KX_PLANAR_H__

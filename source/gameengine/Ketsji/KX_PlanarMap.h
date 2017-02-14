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

class KX_PlanarMap : public KX_TextureRenderer
{
	Py_Header

private:
	/// mirror normal vector
	MT_Vector3 m_normal;
	MT_Vector4 m_clipPlane;

	enum Type {
		REFLECTION,
		REFRACTION
	} m_type;

public:
	KX_PlanarMap(EnvMap *env, KX_GameObject *viewpoint);
	virtual ~KX_PlanarMap();

	virtual std::string GetName();

	void ComputeClipPlane(const MT_Vector3& mirrorObjWorldPos, const MT_Matrix3x3& mirrorObjWorldOri);

	virtual void BeginRender(RAS_IRasterizer *rasty);
	virtual void EndRender(RAS_IRasterizer *rasty);

	const MT_Vector3& GetNormal() const;
	void SetNormal(const MT_Vector3& normal);

	virtual bool SetupCamera(KX_Scene *scene, KX_Camera *camera);
	virtual bool SetupCameraFace(KX_Scene *scene, KX_Camera *camera, unsigned short index);

#ifdef WITH_PYTHON
	static PyObject *pyattr_get_normal(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_normal(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif  // WITH_PYTHON
};

#endif  // __KX_PLANAR_H__

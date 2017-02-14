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

/** \file KX_PlanarMap.cpp
*  \ingroup ketsji
*/

#include "KX_PlanarMap.h"
#include "KX_Camera.h"

#include "RAS_IRasterizer.h"
#include "RAS_Texture.h"

KX_PlanarMap::KX_PlanarMap(EnvMap *env, KX_GameObject *viewpoint)
	:KX_TextureRenderer(env, viewpoint),
	m_normal(0.0f, 0.0f, 1.0f)
{
	m_faces.emplace_back(RAS_Texture::GetTexture2DType());

	switch (env->mode) {
		case ENVMAP_REFLECTION:
		{
			m_type = REFLECTION;
			break;
		}
		case ENVMAP_REFRACTION:
		{
			m_type = REFRACTION;
			break;
		}
	}
}

KX_PlanarMap::~KX_PlanarMap()
{
}

std::string KX_PlanarMap::GetName()
{
	return "KX_PlanarMap";
}

void KX_PlanarMap::ComputeClipPlane(const MT_Vector3& mirrorObjWorldPos, const MT_Matrix3x3& mirrorObjWorldOri)
{
	const MT_Vector3 normal = mirrorObjWorldOri * m_normal;

	m_clipPlane.x() = normal.x();
	m_clipPlane.y() = normal.y();
	m_clipPlane.z() = normal.z();
	m_clipPlane.w() = -(m_clipPlane.x() * mirrorObjWorldPos.x() +
					    m_clipPlane.y() * mirrorObjWorldPos.y() +
					    m_clipPlane.z() * mirrorObjWorldPos.z());
}

void KX_PlanarMap::BeginRender(RAS_IRasterizer *rasty)
{
	KX_TextureRenderer::BeginRender(rasty);

	if (m_type == REFLECTION) {
		rasty->SetInvertFrontFace(true);
	}
	rasty->EnableClipPlane(0, -m_clipPlane);
}

void KX_PlanarMap::EndRender(RAS_IRasterizer *rasty)
{
	if (m_type == REFLECTION) {
		rasty->SetInvertFrontFace(false);
	}
	rasty->DisableClipPlane(0);

	KX_TextureRenderer::EndRender(rasty);
}

const MT_Vector3& KX_PlanarMap::GetNormal() const
{
	return m_normal;
}

bool KX_PlanarMap::SetupCamera(KX_Scene *scene, KX_Camera *camera)
{
	KX_GameObject *mirror = GetViewpointObject();
	KX_Camera *observer = scene->GetActiveCamera();

	// mirror mode, compute camera position and orientation
	// convert mirror position and normal in world space
	const MT_Matrix3x3& mirrorObjWorldOri = mirror->NodeGetWorldOrientation();
	const MT_Vector3& mirrorObjWorldPos = mirror->NodeGetWorldPosition();

	MT_Vector3 cameraWorldPos = observer->NodeGetWorldPosition();
	observer->NodeSetWorldPosition(cameraWorldPos);

	// Update clip plane to possible new normal or viewpoint object.
	ComputeClipPlane(mirrorObjWorldPos, mirrorObjWorldOri);

	const float d = m_clipPlane.x() * cameraWorldPos.x() +
			  m_clipPlane.y() * cameraWorldPos.y() +
			  m_clipPlane.z() * cameraWorldPos.z() +
			  m_clipPlane.w();

	if (d < 0.0) {
		return false;
	}

	const MT_Matrix3x3 mirrorObjWorldOriInverse = mirrorObjWorldOri.inverse();
	MT_Matrix3x3 cameraWorldOri = observer->NodeGetWorldOrientation();

	static const MT_Matrix3x3 unmir(1.0f, 0.0f, 0.0f,
									0.0f, 1.0f, 0.0f,
									0.0f, 0.0f, -1.0f);

	if (m_type == REFLECTION) {
		// Get vector from mirror to camera in mirror space.
		cameraWorldPos = (cameraWorldPos - mirrorObjWorldPos) * mirrorObjWorldOri;

		cameraWorldPos = mirrorObjWorldPos + cameraWorldPos * unmir * mirrorObjWorldOriInverse;
		cameraWorldOri.transpose();
		cameraWorldOri = cameraWorldOri * mirrorObjWorldOri * unmir * mirrorObjWorldOriInverse;
		cameraWorldOri.transpose();
	}

	// Set render camera position and orientation.
	camera->NodeSetWorldPosition(cameraWorldPos);
	camera->NodeSetGlobalOrientation(cameraWorldOri);

	return true;
}

bool KX_PlanarMap::SetupCameraFace(KX_Scene *scene, KX_Camera *camera, unsigned short index)
{
	return true;
}

#ifdef WITH_PYTHON

PyTypeObject KX_PlanarMap::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_PlanarMap",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_PlanarMap::Methods[] = {
	{NULL, NULL} // Sentinel
};

PyAttributeDef KX_PlanarMap::Attributes[] = {
	KX_PYATTRIBUTE_NULL // Sentinel
};

#endif  // WITH_PYTHON

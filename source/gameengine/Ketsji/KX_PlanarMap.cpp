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
#include "KX_PyMath.h"

#include "RAS_Rasterizer.h"
#include "RAS_Texture.h"

KX_PlanarMap::KX_PlanarMap(MTex *mtex, KX_GameObject *viewpoint)
	:KX_TextureRenderer(mtex, viewpoint, LAYER_UNIQUE),
	m_normal(mt::axisZ3)
{
	switch (m_mtex->tex->env->mode) {
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

void KX_PlanarMap::ComputeClipPlane(const mt::vec3& mirrorObjWorldPos, const mt::mat3& mirrorObjWorldOri)
{
	const mt::vec3 normal = mirrorObjWorldOri * m_normal;

	m_clipPlane.x = normal.x;
	m_clipPlane.y = normal.y;
	m_clipPlane.z = normal.z;
	m_clipPlane.w = -(m_clipPlane.x * mirrorObjWorldPos.x +
	                  m_clipPlane.y * mirrorObjWorldPos.y +
	                  m_clipPlane.z * mirrorObjWorldPos.z);
}

void KX_PlanarMap::InvalidateProjectionMatrix()
{
}

mt::mat4 KX_PlanarMap::GetProjectionMatrix(RAS_Rasterizer *rasty, const KX_CameraRenderSchedule& cameraData)
{
	RAS_FrameFrustum frustum = cameraData.m_frameFrustum;
	frustum.camnear = m_clipStart;
	frustum.camfar = m_clipEnd;

	mt::mat4 projection;
	if (cameraData.m_perspective) {
		projection = rasty->GetFrustumMatrix(cameraData.m_stereoMode, cameraData.m_eye, cameraData.m_focalLength,
				frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
	}
	else {
		projection = rasty->GetOrthoMatrix(
				frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
	}

	return projection;
}

void KX_PlanarMap::BeginRenderFace(RAS_Rasterizer* rasty, unsigned short layer, unsigned short face)
{
	RAS_TextureRenderer::BeginRender(rasty, layer);
	RAS_TextureRenderer::BeginRenderFace(rasty, layer, face);

	switch (m_type) {
		case REFLECTION:
		{
			rasty->EnableClipPlane(0, m_clipPlane);
			break;
		}
		case REFRACTION:
		{
			rasty->EnableClipPlane(0, -m_clipPlane);
			break;
		}
	}

	rasty->SetInvertFrontFace((m_type == REFLECTION));
}

void KX_PlanarMap::EndRenderFace(RAS_Rasterizer* rasty, unsigned short layer, unsigned short face)
{
	rasty->SetInvertFrontFace(false);
	rasty->DisableClipPlane(0);

	RAS_TextureRenderer::EndRender(rasty, layer);
}

const mt::vec3& KX_PlanarMap::GetNormal() const
{
	return m_normal;
}

void KX_PlanarMap::SetNormal(const mt::vec3& normal)
{
	m_normal = normal.Normalized();
}

RAS_TextureRenderer::LayerUsage KX_PlanarMap::EnsureLayers(int viewportCount)
{
	// Create as much layers as viewports in the scene, cause the rendering depends on the camera transform.
	const unsigned short size = m_layers.size();
	if (size < viewportCount) {
		m_layers.resize(viewportCount);
		for (unsigned short i = size; i < viewportCount; ++i) {
			m_layers[i] = Layer({RAS_Texture::GetTexture2DType()}, RAS_Texture::GetTexture2DType(), 
					m_mtex->tex->ima, m_useMipmap, m_useLinear);
		}
	}

	return m_layerUsage;
}

bool KX_PlanarMap::PrepareFace(const mt::mat4& sceneViewMat, unsigned short face, mt::mat3x4& camTrans)
{
	// Compute camera position and orientation.
	const mt::mat3& mirrorObjWorldOri = m_viewpointObject->NodeGetWorldOrientation();
	const mt::vec3& mirrorObjWorldPos = m_viewpointObject->NodeGetWorldPosition();
	const mt::mat4 cameraMat = sceneViewMat.Inverse();

	// Use the position and orientation from the view matrix to take care of stereo.
	mt::vec3 cameraWorldPos = cameraMat.TranslationVector3D();

	// Update clip plane to possible new normal or viewpoint object.
	ComputeClipPlane(mirrorObjWorldPos, mirrorObjWorldOri);

	const float d = m_clipPlane.x * cameraWorldPos.x +
	                m_clipPlane.y * cameraWorldPos.y +
	                m_clipPlane.z * cameraWorldPos.z +
	                m_clipPlane.w;

	// Check if the scene camera is in the right plane side.
	if (d < 0.0) {
		return false;
	}

	const mt::mat3 mirrorObjWorldOriInverse = mirrorObjWorldOri.Inverse();
	mt::mat3 cameraWorldOri = mt::mat3::ToRotationMatrix(cameraMat);

	static const mt::mat3 unmir(1.0f, 0.0f, 0.0f,
	                            0.0f, 1.0f, 0.0f,
	                            0.0f, 0.0f, -1.0f);

	if (m_type == REFLECTION) {
		// Get vector from mirror to camera in mirror space.
		cameraWorldPos = (cameraWorldPos - mirrorObjWorldPos) * mirrorObjWorldOri;

		cameraWorldPos = mirrorObjWorldPos + cameraWorldPos * unmir * mirrorObjWorldOriInverse;
		cameraWorldOri = cameraWorldOri.Transpose() * mirrorObjWorldOri * unmir * mirrorObjWorldOriInverse;
		cameraWorldOri = cameraWorldOri.Transpose();
	}

	// Set render camera position and orientation.
	camTrans = mt::mat3x4(cameraWorldOri, cameraWorldPos);

	return true;
}

#ifdef WITH_PYTHON

PyTypeObject KX_PlanarMap::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_PlanarMap",
	sizeof(EXP_PyObjectPlus_Proxy),
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
	&KX_TextureRenderer::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_PlanarMap::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef KX_PlanarMap::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("normal", KX_PlanarMap, pyattr_get_normal, pyattr_set_normal),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *KX_PlanarMap::pyattr_get_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PlanarMap *self = static_cast<KX_PlanarMap *>(self_v);
	return PyObjectFrom(self->GetNormal());
}

int KX_PlanarMap::pyattr_set_normal(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_PlanarMap *self = static_cast<KX_PlanarMap *>(self_v);

	mt::vec3 normal;
	if (!PyVecTo(value, normal)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetNormal(normal);

	return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON

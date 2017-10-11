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

#include "DNA_texture_types.h"

KX_PlanarMap::KX_PlanarMap(EnvMap *env, KX_GameObject *viewpoint)
	:KX_TextureRenderer(env, viewpoint),
	m_normal(mt::axisZ3)
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

std::string KX_PlanarMap::GetName() const
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
	m_projections.clear();
}

const mt::mat4& KX_PlanarMap::GetProjectionMatrix(RAS_Rasterizer *rasty, KX_Scene *scene, KX_Camera *sceneCamera,
                                                  const RAS_Rect& viewport, const RAS_Rect& area)
{
	std::unordered_map<KX_Camera *, mt::mat4>::const_iterator projectionit = m_projections.find(sceneCamera);
	if (projectionit != m_projections.end()) {
		return projectionit->second;
	}

	mt::mat4& projection = m_projections[sceneCamera];

	RAS_FrameFrustum frustum;
	const bool orthographic = !sceneCamera->GetCameraData()->m_perspective;

	if (orthographic) {
		RAS_FramingManager::ComputeOrtho(
			scene->GetFramingType(),
			area,
			viewport,
			sceneCamera->GetScale(),
			m_clipStart,
			m_clipEnd,
			sceneCamera->GetSensorFit(),
			sceneCamera->GetShiftHorizontal(),
			sceneCamera->GetShiftVertical(),
			frustum);
	}
	else {
		RAS_FramingManager::ComputeFrustum(
			scene->GetFramingType(),
			area,
			viewport,
			sceneCamera->GetLens(),
			sceneCamera->GetSensorWidth(),
			sceneCamera->GetSensorHeight(),
			sceneCamera->GetSensorFit(),
			sceneCamera->GetShiftHorizontal(),
			sceneCamera->GetShiftVertical(),
			m_clipStart,
			m_clipEnd,
			frustum);
	}

	if (!sceneCamera->GetViewport()) {
		const float camzoom = sceneCamera->GetZoom();
		frustum.x1 *= camzoom;
		frustum.x2 *= camzoom;
		frustum.y1 *= camzoom;
		frustum.y2 *= camzoom;
	}

	if (orthographic) {
		projection = rasty->GetOrthoMatrix(
			frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
	}
	else {
		projection = rasty->GetFrustumMatrix(frustum.x1, frustum.x2, frustum.y1, frustum.y2, frustum.camnear, frustum.camfar);
	}

	return projection;
}

void KX_PlanarMap::BeginRenderFace(RAS_Rasterizer *rasty)
{
	KX_TextureRenderer::BeginRenderFace(rasty);

	if (m_type == REFLECTION) {
		rasty->SetInvertFrontFace(true);
		rasty->EnableClipPlane(0, m_clipPlane);
	}
	else {
		rasty->EnableClipPlane(0, -m_clipPlane);
	}
}

void KX_PlanarMap::EndRenderFace(RAS_Rasterizer *rasty)
{
	if (m_type == REFLECTION) {
		rasty->SetInvertFrontFace(false);
	}
	rasty->DisableClipPlane(0);

	KX_TextureRenderer::EndRenderFace(rasty);
}

const mt::vec3& KX_PlanarMap::GetNormal() const
{
	return m_normal;
}

void KX_PlanarMap::SetNormal(const mt::vec3& normal)
{
	m_normal = normal.Normalized();
}

bool KX_PlanarMap::SetupCamera(KX_Camera *sceneCamera, KX_Camera *camera)
{
	KX_GameObject *mirror = GetViewpointObject();

	// Compute camera position and orientation.
	const mt::mat3& mirrorObjWorldOri = mirror->NodeGetWorldOrientation();
	const mt::vec3& mirrorObjWorldPos = mirror->NodeGetWorldPosition();

	mt::vec3 cameraWorldPos = sceneCamera->NodeGetWorldPosition();

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
	mt::mat3 cameraWorldOri = sceneCamera->NodeGetWorldOrientation();

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
	camera->NodeSetWorldPosition(cameraWorldPos);
	camera->NodeSetGlobalOrientation(cameraWorldOri);

	return true;
}

bool KX_PlanarMap::SetupCameraFace(KX_Camera *camera, unsigned short index)
{
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

EXP_Attribute KX_PlanarMap::Attributes[] = {
	EXP_ATTRIBUTE_RW("normal", m_normal),
	EXP_ATTRIBUTE_NULL // Sentinel
};

#endif  // WITH_PYTHON

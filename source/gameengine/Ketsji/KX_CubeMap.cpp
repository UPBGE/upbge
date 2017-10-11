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

/** \file KX_CubeMap.cpp
 *  \ingroup ketsji
 */

#include "KX_CubeMap.h"
#include "KX_Camera.h"

#include "RAS_Rasterizer.h"
#include "RAS_Texture.h"

static const mt::mat3 topFaceViewMat(
	1.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f,
	0.0f, 0.0f, -1.0f);

static const mt::mat3 bottomFaceViewMat(
	-1.0f, 0.0f, 0.0f,
	0.0f, -1.0f, 0.0f,
	0.0f, 0.0f, 1.0f);

static const mt::mat3 frontFaceViewMat(
	0.0f, 0.0f, -1.0f,
	0.0f, -1.0f, 0.0f,
	-1.0f, 0.0f, 0.0f);

static const mt::mat3 backFaceViewMat(
	0.0f, 0.0f, 1.0f,
	0.0f, -1.0f, 0.0f,
	1.0f, 0.0f, 0.0f);

static const mt::mat3 rightFaceViewMat(
	1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f,
	0.0f, -1.0f, 0.0f);

static const mt::mat3 leftFaceViewMat(
	1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, -1.0f,
	0.0f, 1.0f, 0.0f);

const mt::mat3 KX_CubeMap::faceViewMatrices3x3[KX_CubeMap::NUM_FACES] = {
	topFaceViewMat,
	bottomFaceViewMat,
	frontFaceViewMat,
	backFaceViewMat,
	rightFaceViewMat,
	leftFaceViewMat
};

KX_CubeMap::KX_CubeMap(EnvMap *env, KX_GameObject *viewpoint)
	:KX_TextureRenderer(env, viewpoint),
	m_invalidProjection(true)
{
	for (int target : RAS_Texture::GetCubeMapTargets()) {
		m_faces.emplace_back(target);
	}
}

KX_CubeMap::~KX_CubeMap()
{
}

std::string KX_CubeMap::GetName() const
{
	return "KX_CubeMap";
}

void KX_CubeMap::InvalidateProjectionMatrix()
{
	m_invalidProjection = true;
}

const mt::mat4& KX_CubeMap::GetProjectionMatrix(RAS_Rasterizer *rasty, KX_Scene *UNUSED(scene), KX_Camera *UNUSED(sceneCamera),
                                                const RAS_Rect& UNUSED(viewport), const RAS_Rect& UNUSED(area))
{
	if (m_invalidProjection) {
		m_projection = rasty->GetFrustumMatrix(-m_clipStart, m_clipStart, -m_clipStart, m_clipStart, m_clipStart, m_clipEnd);
		m_invalidProjection = false;
	}

	return m_projection;
}

bool KX_CubeMap::SetupCamera(KX_Camera *sceneCamera, KX_Camera *camera)
{
	KX_GameObject *viewpoint = GetViewpointObject();
	const mt::vec3& position = viewpoint->NodeGetWorldPosition();

	camera->NodeSetWorldPosition(position);

	return true;
}

bool KX_CubeMap::SetupCameraFace(KX_Camera *camera, unsigned short index)
{
	camera->NodeSetGlobalOrientation(faceViewMatrices3x3[index]);

	return true;
}

#ifdef WITH_PYTHON

PyTypeObject KX_CubeMap::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_CubeMap",
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

PyMethodDef KX_CubeMap::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

EXP_Attribute KX_CubeMap::Attributes[] = {
	EXP_ATTRIBUTE_NULL // Sentinel
};

#endif  // WITH_PYTHON

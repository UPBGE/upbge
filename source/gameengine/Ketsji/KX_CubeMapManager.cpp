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

/** \file gameengine/Ketsji/KX_CubeMapManager.cpp
 *  \ingroup ketsji
 */

#include "KX_CubeMapManager.h"
#include "KX_Camera.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_CubeMap.h"

#include "EXP_ListValue.h"

#include "RAS_IRasterizer.h"
#include "RAS_Texture.h"

#include "DNA_texture_types.h"

KX_CubeMapManager::KX_CubeMapManager(KX_Scene *scene)
	:m_scene(scene)
{
	const RAS_CameraData& camdata = RAS_CameraData();
	m_camera = new KX_Camera(m_scene, KX_Scene::m_callbacks, camdata, true, true);
	m_camera->SetName("__cubemap_cam__");
}

KX_CubeMapManager::~KX_CubeMapManager()
{
	for (std::vector<KX_CubeMap *>::iterator it = m_cubeMaps.begin(), end = m_cubeMaps.end(); it != end; ++it) {
		delete *it;
	}

	m_camera->Release();
}

void KX_CubeMapManager::AddCubeMap(RAS_Texture *texture, KX_GameObject *gameobj)
{
	for (std::vector<KX_CubeMap *>::iterator it = m_cubeMaps.begin(), end = m_cubeMaps.end(); it != end; ++it) {
		KX_CubeMap *cubeMap = *it;
		const std::vector<RAS_Texture *>& textures = cubeMap->GetTextureUsers();
		for (std::vector<RAS_Texture *>::const_iterator it = textures.begin(), end = textures.end(); it != end; ++it) {
			if ((*it)->GetTex() == texture->GetTex()) {
				cubeMap->AddTextureUser(texture);
				return;
			}
		}
	}

	EnvMap *env = texture->GetTex()->env;
	KX_CubeMap *cubeMap = new KX_CubeMap(env, gameobj);
	cubeMap->AddTextureUser(texture);
	texture->SetCubeMap(cubeMap);
	m_cubeMaps.push_back(cubeMap);
}

void KX_CubeMapManager::InvalidateCubeMapViewpoint(KX_GameObject *gameobj)
{
	for (std::vector<KX_CubeMap *>::iterator it = m_cubeMaps.begin(), end = m_cubeMaps.end(); it != end; ++it) {
		KX_CubeMap *cubeMap = *it;
		if (cubeMap->GetViewpointObject() == gameobj) {
			cubeMap->SetViewpointObject(NULL);
		}
	}
}

void KX_CubeMapManager::RenderCubeMap(RAS_IRasterizer *rasty, KX_CubeMap *cubeMap)
{
	KX_GameObject *viewpoint = cubeMap->GetViewpointObject();

	// Doesn't need (or can) update.
	if (!cubeMap->NeedUpdate() || !cubeMap->GetEnabled() || !viewpoint) {
		return;
	}

	const MT_Vector3& position = viewpoint->NodeGetWorldPosition();

	/* We hide the viewpoint object in the case backface culling is disabled -> we can't see through
	 * the object faces if the camera is inside the gameobject.
	 */
	viewpoint->SetVisible(false, true);

	// For Culling we need first to set the camera position at the object position.
	m_camera->NodeSetWorldPosition(position);

	/* When we update clipstart or clipend values,
	 * or if the projection matrix is not computed yet,
	 * we have to compute projection matrix.
	 */
	if (cubeMap->GetInvalidProjectionMatrix()) {
		const float clipstart = cubeMap->GetClipStart();
		const float clipend = cubeMap->GetClipEnd();
		const MT_Matrix4x4& proj = rasty->GetFrustumMatrix(-clipstart, clipstart, -clipstart, clipstart, clipstart, clipend, 1.0f, true);
		cubeMap->SetProjectionMatrix(proj);
		cubeMap->SetInvalidProjectionMatrix(false);
	}

	// Else we use the projection matrix stored in the cube map.
	rasty->SetProjectionMatrix(cubeMap->GetProjectionMatrix());
	m_camera->SetProjectionMatrix(cubeMap->GetProjectionMatrix());

	cubeMap->BeginRender();

	for (unsigned short i = 0; i < 6; ++i) {
		cubeMap->BindFace(rasty, i);

		// For Culling we need also to set the camera orientation.
		m_camera->NodeSetGlobalOrientation(RAS_CubeMap::faceViewMatrices3x3[i]);
		m_camera->NodeUpdateGS(0.0f);

		// Setup camera modelview matrix for culling planes.
		const MT_Transform trans(m_camera->GetWorldToCamera());
		const MT_Matrix4x4 viewmat(trans);
		m_camera->SetModelviewMatrix(viewmat);

		rasty->SetViewMatrix(viewmat, RAS_CubeMap::faceViewMatrices3x3[i], position, MT_Vector3(1.0f, 1.0f, 1.0f), true);

		m_scene->CalculateVisibleMeshes(rasty, m_camera, ~cubeMap->GetIgnoreLayers());

		/* Update animations to use the culling of each faces, BL_ActionManager avoid redundants
		 * updates internally. */
		KX_GetActiveEngine()->UpdateAnimations(m_scene);

		// Now the objects are culled and we can render the scene.
		m_scene->GetWorldInfo()->RenderBackground(rasty);
		m_scene->RenderBuckets(trans, rasty);
	}

	cubeMap->EndRender();

	viewpoint->SetVisible(true, true);
}

void KX_CubeMapManager::Render(RAS_IRasterizer *rasty)
{
	if (m_cubeMaps.size() == 0 || rasty->GetDrawingMode() != RAS_IRasterizer::RAS_TEXTURED) {
		return;
	}

	const RAS_IRasterizer::DrawType drawmode = rasty->GetDrawingMode();
	rasty->SetDrawingMode(RAS_IRasterizer::RAS_CUBEMAP);

	// Disable scissor to not bother with scissor box.
	rasty->Disable(RAS_IRasterizer::RAS_SCISSOR_TEST);

	// Copy current stereo mode.
	const RAS_IRasterizer::StereoMode steremode = rasty->GetStereoMode();
	// Disable stereo for realtime cube maps.
	rasty->SetStereoMode(RAS_IRasterizer::RAS_STEREO_NOSTEREO);

	for (std::vector<KX_CubeMap *>::iterator it = m_cubeMaps.begin(), end = m_cubeMaps.end(); it != end; ++it) {
		RenderCubeMap(rasty, *it);
	}

	// Restore previous stereo mode.
	rasty->SetStereoMode(steremode);

	rasty->Enable(RAS_IRasterizer::RAS_SCISSOR_TEST);

	rasty->SetDrawingMode(drawmode);
}

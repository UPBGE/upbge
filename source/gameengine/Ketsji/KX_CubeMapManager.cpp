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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/BL_Texture.cpp
 *  \ingroup ketsji
 */

#include "KX_CubeMapManager.h"
#include "KX_Camera.h"
#include "KX_Scene.h"
#include "KX_Globals.h"

#include "EXP_ListValue.h"

#include "RAS_CubeMap.h"
#include "RAS_IRasterizer.h"

KX_CubeMapManager::KX_CubeMapManager(KX_Scene *scene)
	:m_scene(scene),
	m_initCubeMaps(false)
{
	RAS_CameraData camdata = RAS_CameraData();
	m_camera = new KX_Camera(m_scene, KX_Scene::m_callbacks, camdata);
}

KX_CubeMapManager::~KX_CubeMapManager()
{
	delete m_camera;
}

void KX_CubeMapManager::RenderCubeMap(RAS_IRasterizer *rasty, RAS_CubeMap *cubeMap)
{
	KX_GameObject *gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)cubeMap->GetClientObject());
	MT_Vector3 pos = gameobj->NodeGetWorldPosition();

	/* We hide the gameobject in the case backface culling is disabled -> we can't see through
	 * the object faces if the camera is inside the gameobject
	 */
	gameobj->SetVisible(false, true);

	/* For Culling we need first to set the camera position at the object position */
	m_camera->NodeSetWorldPosition(pos);
	
	/* For each cubeMap, environment texture clipend value is not necessarly the same
	 * so we have to get/set the right projection matrix from cubeMap each time
	 * we render a cubeMap
	 */
	rasty->SetProjectionMatrix(cubeMap->GetProjection());
	m_camera->SetProjectionMatrix(cubeMap->GetProjection());

	cubeMap->BeginRender();

	for (unsigned short i = 0; i < 6; ++i) {
		cubeMap->BindFace(rasty, i, pos);

		/* For Culling we need also to set the camera orientation */
		m_camera->NodeSetGlobalOrientation(camOri[i]);
		m_camera->NodeUpdateGS(0.0f);
		MT_Transform trans(m_camera->GetWorldToCamera());
		MT_Matrix4x4 viewmat(trans);
		m_camera->SetModelviewMatrix(viewmat);
		m_scene->CalculateVisibleMeshes(rasty, m_camera, cubeMap->GetLayer());

		/* Update animations to use the culling of each faces, BL_ActionManager avoid redundants
		 * updates internally. */
		KX_GetActiveEngine()->UpdateAnimations(m_scene);

		/* Now the objects are culled and we can render the scene */
		m_scene->GetWorldInfo()->RenderBackground(rasty);
		m_scene->RenderBuckets(trans, rasty);

		cubeMap->UnbindFace();
	}

	cubeMap->EndRender();

	gameobj->SetVisible(true, true);
}

void KX_CubeMapManager::Render(RAS_IRasterizer *rasty)
{
	if (!m_initCubeMaps) {
		m_scene->CreateGameobjWithCubeMapList(rasty);
		m_initCubeMaps = true;
	}

	// Copy current stereo mode.
	const RAS_IRasterizer::StereoMode steremode = rasty->GetStereoMode();
	rasty->SetStereoMode(RAS_IRasterizer::RAS_STEREO_NOSTEREO);

	for (std::vector<RAS_CubeMap *>::iterator it = m_cubeMaps.begin(), end = m_cubeMaps.end(); it != end; ++it) {
		RenderCubeMap(rasty, *it);
	}

	// Restore stereo mode.
	rasty->SetStereoMode(steremode);

	RestoreFrameBuffer();
}

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

/** \file gameengine/Ketsji/KX_PlanarManager.cpp
*  \ingroup ketsji
*/

#include "KX_PlanarManager.h"
#include "KX_Camera.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_Planar.h"

#include "EXP_ListValue.h"

#include "RAS_IRasterizer.h"
#include "RAS_Texture.h"

#include "DNA_texture_types.h"

KX_PlanarManager::KX_PlanarManager(KX_Scene *scene)
	:m_scene(scene)
{
	const RAS_CameraData& camdata = RAS_CameraData();
	m_camera = new KX_Camera(m_scene, KX_Scene::m_callbacks, camdata, true, true);
	m_camera->SetName("__planar_cam__");
}

KX_PlanarManager::~KX_PlanarManager()
{
	for (std::vector<KX_Planar *>::iterator it = m_planars.begin(), end = m_planars.end(); it != end; ++it) {
		delete *it;
	}

	m_camera->Release();
}

void KX_PlanarManager::AddPlanar(RAS_Texture *texture, KX_GameObject *gameobj, int type)
{
	for (std::vector<KX_Planar *>::iterator it = m_planars.begin(), end = m_planars.end(); it != end; ++it) {
		KX_Planar *planar = *it;
		const std::vector<RAS_Texture *>& textures = planar->GetTextureUsers();
		for (std::vector<RAS_Texture *>::const_iterator it = textures.begin(), end = textures.end(); it != end; ++it) {
			if ((*it)->GetTex() == texture->GetTex()) {
				planar->AddTextureUser(texture);
				return;
			}
		}
	}

	Tex *tex = texture->GetTex();
	KX_Planar *kxplanar = new KX_Planar(tex, gameobj, type);
	kxplanar->AddTextureUser(texture);
	texture->SetPlanar(kxplanar);
	m_planars.push_back(kxplanar);
}

void KX_PlanarManager::RenderPlanar(RAS_IRasterizer *rasty, KX_Planar *planar)
{
	KX_GameObject *viewpoint = planar->GetViewpointObject();

	// Doesn't need (or can) update.
	if (!planar->NeedUpdate() || !planar->GetEnabled() || !viewpoint) {
		return;
	}

	const MT_Vector3& position = viewpoint->NodeGetWorldPosition();

	// For Culling we need first to set the camera position at the object position.
	m_camera->NodeSetWorldPosition(position);



	// Else we use the projection matrix stored in the planar.
	rasty->SetProjectionMatrix(planar->GetProjectionMatrix());
	m_camera->SetProjectionMatrix(planar->GetProjectionMatrix());

	planar->BeginRender();

	
	planar->BindFace(rasty, 0);

	// For Culling we need also to set the camera orientation.
	m_camera->NodeSetGlobalOrientation(planar->GetViewpointObject()->NodeGetWorldOrientation());
	m_camera->NodeUpdateGS(0.0f);

	// Setup camera modelview matrix for culling planes.
	const MT_Transform trans(m_camera->GetWorldToCamera());
	const MT_Matrix4x4 viewmat(trans);
	m_camera->SetModelviewMatrix(viewmat);

	rasty->SetViewMatrix(viewmat, planar->GetViewpointObject()->NodeGetWorldOrientation(), position, MT_Vector3(1.0f, 1.0f, 1.0f), true);

	m_scene->CalculateVisibleMeshes(rasty, m_camera);

	/* Update animations to use the culling of each faces, BL_ActionManager avoid redundants
	* updates internally. */
	KX_GetActiveEngine()->UpdateAnimations(m_scene);

	// Now the objects are culled and we can render the scene.
	m_scene->GetWorldInfo()->RenderBackground(rasty);
	m_scene->RenderBuckets(trans, rasty);

	planar->EndRender();
}

void KX_PlanarManager::Render(RAS_IRasterizer *rasty)
{
	if (m_planars.size() == 0 || rasty->GetDrawingMode() != RAS_IRasterizer::RAS_TEXTURED) {
		return;
	}

	// Disable scissor to not bother with scissor box.
	rasty->Disable(RAS_IRasterizer::RAS_SCISSOR_TEST);

	// Copy current stereo mode.
	const RAS_IRasterizer::StereoMode steremode = rasty->GetStereoMode();
	// Disable stereo for realtime planar.
	rasty->SetStereoMode(RAS_IRasterizer::RAS_STEREO_NOSTEREO);

	for (std::vector<KX_Planar *>::iterator it = m_planars.begin(), end = m_planars.end(); it != end; ++it) {
		RenderPlanar(rasty, *it);
	}

	// Restore previous stereo mode.
	rasty->SetStereoMode(steremode);

	rasty->Enable(RAS_IRasterizer::RAS_SCISSOR_TEST);
}

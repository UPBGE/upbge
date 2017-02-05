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

/** \file gameengine/Ketsji/KX_TextureProbeManager.cpp
 *  \ingroup ketsji
 */

#include "KX_TextureProbeManager.h"
#include "KX_Camera.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_CubeMap.h"
#include "KX_PlanarMap.h"

#include "RAS_IRasterizer.h"
#include "RAS_Texture.h"

#include "DNA_texture_types.h"

KX_TextureProbeManager::KX_TextureProbeManager(KX_Scene *scene)
	:m_scene(scene)
{
	const RAS_CameraData& camdata = RAS_CameraData();
	m_camera = new KX_Camera(m_scene, KX_Scene::m_callbacks, camdata, true, true);
	m_camera->SetName("__probe_cam__");
}

KX_TextureProbeManager::~KX_TextureProbeManager()
{
	for (KX_TextureProbe *probe : m_probes) {
		delete probe;
	}

	m_camera->Release();
}

void KX_TextureProbeManager::InvalidateViewpoint(KX_GameObject *gameobj)
{
	for (KX_TextureProbe *probe : m_probes) {
		if (probe->GetViewpointObject() == gameobj) {
			probe->SetViewpointObject(NULL);
		}
	}
}

void KX_TextureProbeManager::AddProbe(ProbeType type, RAS_Texture *texture, KX_GameObject *viewpoint)
{
	/* Don't Add probe several times for the same texture. If the texture is shared by several objects,
	 * we just add a "textureUser" to signal that the probe texture will be shared by several objects.
	 */
	for (KX_TextureProbe *probe : m_probes) {
		for (RAS_Texture *textureUser : probe->GetTextureUsers()) {
			if (textureUser->GetTex() == texture->GetTex()) {
				probe->AddTextureUser(texture);
				return;
			}
		}
	}

	EnvMap *env = texture->GetTex()->env;
	KX_TextureProbe *probe;
	switch (type) {
		case CUBE:
		{
			probe = new KX_CubeMap(env, viewpoint);
			break;
		}
		case PLANAR:
		{
			probe = new KX_PlanarMap(env, viewpoint);
			break;
		}
	}

	probe->AddTextureUser(texture);
	m_probes.push_back(probe);
}

void KX_TextureProbeManager::RenderProbe(RAS_IRasterizer *rasty, KX_TextureProbe *probe)
{
	KX_GameObject *viewpoint = probe->GetViewpointObject();
	// Doesn't need (or can) update.
	if (!probe->NeedUpdate() || !probe->GetEnabled() || !viewpoint) {
		return;
	}

	// Begin rendering stuff
	probe->BeginRender(rasty);

	const bool visible = viewpoint->GetVisible();
	/* We hide the viewpoint object in the case backface culling is disabled -> we can't see through
	 * the object faces if the camera is inside the gameobject.
	 */
	viewpoint->SetVisible(false, false);

	// Set camera lod distance factor from probe value.
	m_camera->SetLodDistanceFactor(probe->GetLodDistanceFactor());
	// Set camera setting shared by all the probe's faces.
	if (!probe->SetupCamera(m_scene, m_camera)) {
		return;
	}

	/* When we update clipstart or clipend values,
	 * or if the projection matrix is not computed yet,
	 * we have to compute projection matrix.
	 */
	if (probe->GetInvalidProjectionMatrix()) {
		const float clipstart = probe->GetClipStart();
		const float clipend = probe->GetClipEnd();
		const MT_Matrix4x4& proj = rasty->GetFrustumMatrix(-clipstart, clipstart, -clipstart, clipstart, clipstart, clipend, 1.0f, true);
		probe->SetProjectionMatrix(proj);
		probe->SetInvalidProjectionMatrix(false);
	}

	const MT_Matrix4x4& projmat = probe->GetProjectionMatrix();
	m_camera->SetProjectionMatrix(projmat);

	for (unsigned short i = 0; i < probe->GetNumFaces(); ++i) {
		// Set camera settings unique per faces.
		if (!probe->SetupCameraFace(m_scene, m_camera, i)) {
			continue;
		}

		m_camera->NodeUpdateGS(0.0f);

		probe->BindFace(rasty, i);

		const MT_Transform camtrans(m_camera->GetWorldToCamera());
		const MT_Matrix4x4 viewmat(camtrans);

		rasty->SetViewMatrix(viewmat, m_camera->NodeGetWorldOrientation(), m_camera->NodeGetWorldPosition(), m_camera->NodeGetLocalScaling(), m_camera->GetCameraData()->m_perspective);
		m_camera->SetModelviewMatrix(viewmat);

		m_scene->CalculateVisibleMeshes(rasty, m_camera, ~probe->GetIgnoreLayers());

		/* Updating the lod per face is normally not expensive because a cube map normally show every objects
		 * but here we update only visible object of a face including the clip end and start.
		 */
		m_scene->UpdateObjectLods(m_camera);

		/* Update animations to use the culling of each faces, BL_ActionManager avoid redundants
		 * updates internally. */
		KX_GetActiveEngine()->UpdateAnimations(m_scene);

		// Now the objects are culled and we can render the scene.
		m_scene->GetWorldInfo()->RenderBackground(rasty);
		// Send a NULL off screen because we use a set of FBO with shared textures, not an off screen.
		m_scene->RenderBuckets(camtrans, rasty, NULL);
	}

	viewpoint->SetVisible(visible, false);

	probe->EndRender(rasty);
}

void KX_TextureProbeManager::Render(RAS_IRasterizer *rasty)
{
	if (m_probes.size() == 0 || rasty->GetDrawingMode() != RAS_IRasterizer::RAS_TEXTURED) {
		return;
	}

	const RAS_IRasterizer::DrawType drawmode = rasty->GetDrawingMode();
	rasty->SetDrawingMode(RAS_IRasterizer::RAS_PROBE);

	// Disable scissor to not bother with scissor box.
	rasty->Disable(RAS_IRasterizer::RAS_SCISSOR_TEST);

	// Copy current stereo mode.
	const RAS_IRasterizer::StereoMode steremode = rasty->GetStereoMode();
	// Disable stereo for realtime probe.
	rasty->SetStereoMode(RAS_IRasterizer::RAS_STEREO_NOSTEREO);

	for (KX_TextureProbe *probe : m_probes) {
		RenderProbe(rasty, probe);
	}

	// Restore previous stereo mode.
	rasty->SetStereoMode(steremode);

	rasty->Enable(RAS_IRasterizer::RAS_SCISSOR_TEST);

	rasty->SetDrawingMode(drawmode);
}

void KX_TextureProbeManager::Merge(KX_TextureProbeManager *other)
{
	m_probes.insert(m_probes.end(), other->m_probes.begin(), other->m_probes.end());
	other->m_probes.clear();
}

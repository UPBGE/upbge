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

/** \file gameengine/Ketsji/KX_TextureRendererManager.cpp
 *  \ingroup ketsji
 */

#include "KX_TextureRendererManager.h"
#include "KX_Camera.h"
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "KX_CubeMap.h"
#include "KX_PlanarMap.h"

#include "RAS_Rasterizer.h"
#include "RAS_OffScreen.h"
#include "RAS_Texture.h"

#include "DNA_texture_types.h"

#include "CM_Message.h"

KX_TextureRendererManager::KX_TextureRendererManager(KX_Scene *scene)
	:m_scene(scene)
{
	const RAS_CameraData& camdata = RAS_CameraData();
	m_camera = new KX_Camera(m_scene, KX_Scene::m_callbacks, camdata, true);
	m_camera->SetName("__renderer_cam__");
}

KX_TextureRendererManager::~KX_TextureRendererManager()
{
	for (unsigned short i = 0; i < CATEGORY_MAX; ++i) {
		for (KX_TextureRenderer *renderer : m_renderers[i]) {
			delete renderer;
		}
	}

	m_camera->Release();
}

void KX_TextureRendererManager::InvalidateViewpoint(KX_GameObject *gameobj)
{
	for (unsigned short i = 0; i < CATEGORY_MAX; ++i) {
		for (KX_TextureRenderer *renderer : m_renderers[i]) {
			if (renderer->GetViewpointObject() == gameobj) {
				renderer->SetViewpointObject(nullptr);
			}
		}
	}
}

void KX_TextureRendererManager::AddRenderer(RendererType type, RAS_Texture *texture, KX_GameObject *viewpoint)
{
	/* Don't Add renderer several times for the same texture. If the texture is shared by several objects,
	 * we just add a "textureUser" to signal that the renderer texture will be shared by several objects.
	 */
	for (unsigned short i = 0; i < CATEGORY_MAX; ++i) {
		for (KX_TextureRenderer *renderer : m_renderers[i]) {
			if (renderer->EqualTextureUser(texture)) {
				renderer->AddTextureUser(texture);

				KX_GameObject *origviewpoint = renderer->GetViewpointObject();
				if (viewpoint != origviewpoint) {
					CM_Warning("texture renderer (" << texture->GetName() << ") uses different viewpoint objects (" <<
					           (origviewpoint ? origviewpoint->GetName() : "<None>") << " and " << viewpoint->GetName() << ").");
				}
				return;
			}
		}
	}

	EnvMap *env = texture->GetTex()->env;
	KX_TextureRenderer *renderer;
	switch (type) {
		case CUBE:
		{
			renderer = new KX_CubeMap(env, viewpoint);
			m_renderers[VIEWPORT_INDEPENDENT].push_back(renderer);
			break;
		}
		case PLANAR:
		{
			renderer = new KX_PlanarMap(env, viewpoint);
			m_renderers[VIEWPORT_DEPENDENT].push_back(renderer);
			break;
		}
	}

	renderer->AddTextureUser(texture);
}

bool KX_TextureRendererManager::RenderRenderer(RAS_Rasterizer *rasty, KX_TextureRenderer *renderer,
                                               KX_Camera *sceneCamera, const RAS_Rect& viewport, const RAS_Rect& area)
{
	KX_GameObject *viewpoint = renderer->GetViewpointObject();
	// Doesn't need (or can) update.
	if (!renderer->NeedUpdate() || !renderer->GetEnabled() || !viewpoint) {
		return false;
	}

	// Set camera setting shared by all the renderer's faces.
	if (!renderer->SetupCamera(sceneCamera, m_camera)) {
		return false;
	}

	const bool visible = viewpoint->GetVisible();
	/* We hide the viewpoint object in the case backface culling is disabled -> we can't see through
	 * the object faces if the camera is inside the gameobject.
	 */
	viewpoint->SetVisible(false, false);

	// Set camera lod distance factor from renderer value.
	m_camera->SetLodDistanceFactor(renderer->GetLodDistanceFactor());

	/* When we update clipstart or clipend values,
	 * or if the projection matrix is not computed yet,
	 * we have to compute projection matrix.
	 */
	const mt::mat4& projmat = renderer->GetProjectionMatrix(rasty, m_scene, sceneCamera, viewport, area);
	m_camera->SetProjectionMatrix(projmat);
	rasty->SetProjectionMatrix(projmat);

	// Begin rendering stuff
	renderer->BeginRender(rasty);

	for (unsigned short i = 0; i < renderer->GetNumFaces(); ++i) {
		// Set camera settings unique per faces.
		if (!renderer->SetupCameraFace(m_camera, i)) {
			continue;
		}

		m_camera->NodeUpdate();

		renderer->BindFace(i);

		const mt::mat3x4 camtrans(m_camera->GetWorldToCamera());
		const mt::mat4 viewmat = mt::mat4::FromAffineTransform(camtrans);

		rasty->SetViewMatrix(viewmat);
		m_camera->SetModelviewMatrix(viewmat);

		const std::vector<KX_GameObject *> objects = m_scene->CalculateVisibleMeshes(m_camera, ~renderer->GetIgnoreLayers());

		/* Updating the lod per face is normally not expensive because a cube map normally show every objects
		 * but here we update only visible object of a face including the clip end and start.
		 */
		m_scene->UpdateObjectLods(m_camera, objects);

		/* Update animations to use the culling of each faces, BL_ActionManager avoid redundants
		 * updates internally. */
		KX_GetActiveEngine()->UpdateAnimations(m_scene);

		renderer->BeginRenderFace(rasty);

		// Now the objects are culled and we can render the scene.
		m_scene->GetWorldInfo()->RenderBackground(rasty);
		// Send a nullptr off screen because we use a set of FBO with shared textures, not an off screen.
		m_scene->RenderBuckets(objects, RAS_Rasterizer::RAS_TEXTURED, camtrans, rasty, nullptr);

		renderer->EndRenderFace(rasty);
	}

	viewpoint->SetVisible(visible, false);

	renderer->EndRender(rasty);

	return true;
}

void KX_TextureRendererManager::Render(RendererCategory category, RAS_Rasterizer *rasty, RAS_OffScreen *offScreen,
                                       KX_Camera *sceneCamera, const RAS_Rect& viewport, const RAS_Rect& area)
{
	const std::vector<KX_TextureRenderer *>& renderers = m_renderers[category];
	if (renderers.empty() || rasty->GetDrawingMode() != RAS_Rasterizer::RAS_TEXTURED) {
		return;
	}

	// Disable scissor to not bother with scissor box.
	rasty->Disable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	// Check if at least one renderer was rendered.
	bool rendered = false;
	for (KX_TextureRenderer *renderer : renderers) {
		rendered |= RenderRenderer(rasty, renderer, sceneCamera, viewport, area);
	}

	rasty->Enable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	if (offScreen && rendered) {
		// Restore the off screen bound before rendering the texture renderers.
		offScreen->Bind();
	}
}

void KX_TextureRendererManager::Merge(KX_TextureRendererManager *other)
{
	for (unsigned short i = 0; i < CATEGORY_MAX; ++i) {
		m_renderers[i].insert(m_renderers[i].end(), other->m_renderers[i].begin(), other->m_renderers[i].end());
		other->m_renderers[i].clear();
	}
}

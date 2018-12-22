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
	for (KX_TextureRenderer *renderer : m_renderers) {
		delete renderer;
	}

	m_camera->Release();
}

void KX_TextureRendererManager::InvalidateViewpoint(KX_GameObject *gameobj)
{
	for (KX_TextureRenderer *renderer : m_renderers) {
		if (renderer->GetViewpointObject() == gameobj) {
			renderer->SetViewpointObject(nullptr);
		}
	}
}

void KX_TextureRendererManager::ReloadTextures()
{
	for (KX_TextureRenderer *renderer : m_renderers) {
		renderer->ReloadTexture();
	}
}

void KX_TextureRendererManager::AddRenderer(RendererType type, RAS_Texture *texture, KX_GameObject *viewpoint)
{
	// Find a shared renderer (using the same material texture) or create a new one.
	for (KX_TextureRenderer *renderer : m_renderers) {
		if (texture->GetMTex() == renderer->GetMTex()) {
			texture->SetRenderer(renderer);
			KX_GameObject *origviewpoint = renderer->GetViewpointObject();
			if (viewpoint != origviewpoint) {
				CM_Warning("texture renderer (" << texture->GetName() << ") uses different viewpoint objects (" <<
				           (origviewpoint ? origviewpoint->GetName() : "<None>") << " and " << viewpoint->GetName() << ").");
			}
			return;
		}
	}

	MTex *mtex = texture->GetMTex();
	KX_TextureRenderer *renderer;
	switch (type) {
		case CUBE:
		{
			renderer = new KX_CubeMap(mtex, viewpoint);
			break;
		}
		case PLANAR:
		{
			renderer = new KX_PlanarMap(mtex, viewpoint);
			break;
		}
	}

	texture->SetRenderer(renderer);
	m_renderers.push_back(renderer);
}

void KX_TextureRendererManager::RenderRenderer(RAS_Rasterizer *rasty, KX_TextureRenderer *renderer,
		const std::vector<const KX_CameraRenderData *>& cameraDatas)
{
	KX_GameObject *viewpoint = renderer->GetViewpointObject();
	// Doesn't need (or can) update.
	if (!renderer->NeedUpdate() || !renderer->GetEnabled() || !viewpoint) {
		return;
	}

	const int visibleLayers = ~renderer->GetIgnoreLayers();

	const bool visible = viewpoint->GetVisible();
	/* We hide the viewpoint object in the case backface culling is disabled -> we can't see through
	 * the object faces if the camera is inside the gameobject.
	 */
	viewpoint->SetVisible(false, false);

	// Set camera lod distance factor from renderer value.
	m_camera->SetLodDistanceFactor(renderer->GetLodDistanceFactor());

	// Ensure the number of layers for all viewports or use a unique layer.
	const unsigned short numViewport = cameraDatas.size();
	RAS_TextureRenderer::LayerUsage usage = renderer->EnsureLayers(numViewport);
	const unsigned short numlay = (usage == RAS_TextureRenderer::LAYER_SHARED) ? 1 : numViewport;

	for (unsigned short layer = 0; layer < numlay; ++layer) {
		/* Two cases are possible :
		 * - Only one layer is present for any number of viewports,
		 *   in this case the renderer must not care about the viewport (e.g cube map).
		 * - Multiple layers are present, one per viewport, in this case
		 *   the renderer could care of the viewport and the index of the layer
		 *   match the index of the viewport in the scene.
		 */

		const KX_CameraRenderData *cameraData = cameraDatas[layer];
		KX_Camera *sceneCamera = cameraData->m_renderCamera;
		RAS_Rasterizer::StereoEye eye = cameraData->m_eye;

		// Set camera setting shared by all the renderer's faces.
		if (!renderer->Prepare(sceneCamera, eye, m_camera)) {
			continue;
		}

		/* When we update clipstart or clipend values,
		* or if the projection matrix is not computed yet,
		* we have to compute projection matrix.
		*/
		const mt::mat4 projmat = renderer->GetProjectionMatrix(rasty, m_scene, sceneCamera,
				cameraData->m_viewport, cameraData->m_area, cameraData->m_stereoMode, eye);
		m_camera->SetProjectionMatrix(projmat, eye);
		rasty->SetProjectionMatrix(projmat);

		// Begin rendering stuff
		renderer->BeginRender(rasty, layer);

		for (unsigned short face = 0, numface = renderer->GetNumFaces(layer); face < numface; ++face) {
			// Set camera settings unique per faces.
			if (!renderer->PrepareFace(m_camera, face)) {
				continue;
			}

			m_camera->NodeUpdate();

			const mt::mat3x4 camtrans(m_camera->GetWorldToCamera());
			const mt::mat4 viewmat = mt::mat4::FromAffineTransform(camtrans);
			rasty->SetViewMatrix(viewmat);
			m_camera->SetModelviewMatrix(viewmat, eye);

			renderer->BeginRenderFace(rasty, layer, face);

			const std::vector<KX_GameObject *> objects = m_scene->CalculateVisibleMeshes(m_camera, eye, visibleLayers);

			/* Updating the lod per face is normally not expensive because a cube map normally show every objects
			* but here we update only visible object of a face including the clip end and start.
			*/
			m_scene->UpdateObjectLods(m_camera, objects);

			/* Update animations to use the culling of each faces, BL_ActionManager avoid redundants
			* updates internally. */
			KX_GetActiveEngine()->UpdateAnimations(m_scene);

			// Now the objects are culled and we can render the scene.
			m_scene->GetWorldInfo()->RenderBackground(rasty);

			m_scene->RenderBuckets(objects, RAS_Rasterizer::RAS_RENDERER, camtrans, layer, rasty, nullptr);
		}

		renderer->EndRender(rasty, layer);
	}

	viewpoint->SetVisible(visible, false);
}

void KX_TextureRendererManager::Render(RAS_Rasterizer *rasty, const KX_SceneRenderData& sceneData)
{
	if (m_renderers.empty() || rasty->GetDrawingMode() != RAS_Rasterizer::RAS_TEXTURED) {
		return;
	}

	// Get the number of viewports.
	const unsigned short viewportCount = sceneData.m_cameraDataList[RAS_Rasterizer::RAS_STEREO_LEFTEYE].size() +
	sceneData.m_cameraDataList[RAS_Rasterizer::RAS_STEREO_RIGHTEYE].size();
	// Construct a list of all the camera data by the viewport index order.
	std::vector<const KX_CameraRenderData *> cameraDatas(viewportCount);
	for (unsigned short eye = RAS_Rasterizer::RAS_STEREO_LEFTEYE; eye < RAS_Rasterizer::RAS_STEREO_MAXEYE; ++eye) {
		for (const KX_CameraRenderData& cameraData : sceneData.m_cameraDataList[eye]) {
			cameraDatas[cameraData.m_index] = &cameraData;
		}
	}

	// Disable scissor to not bother with scissor box.
	rasty->Disable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	for (KX_TextureRenderer *renderer : m_renderers) {
		RenderRenderer(rasty, renderer, cameraDatas);
	}

	rasty->Enable(RAS_Rasterizer::RAS_SCISSOR_TEST);
}

void KX_TextureRendererManager::Merge(KX_TextureRendererManager *other)
{
	m_renderers.insert(m_renderers.end(), other->m_renderers.begin(), other->m_renderers.end());
	other->m_renderers.clear();
}

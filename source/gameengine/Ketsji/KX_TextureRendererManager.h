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

/** \file KX_TextureRendererManager.h
*  \ingroup ketsji
*/

#ifndef __KX_TEXTURE_RENDERER_MANAGER_H__
#define __KX_TEXTURE_RENDERER_MANAGER_H__

#include <vector>

class KX_GameObject;
class KX_Camera;
class KX_Scene;
class KX_TextureRenderer;

class RAS_Rasterizer;
class RAS_OffScreen;
class RAS_Texture;
class RAS_Rect;

class KX_TextureRendererManager
{
public:
	enum RendererCategory {
		VIEWPORT_DEPENDENT = 0,
		VIEWPORT_INDEPENDENT,
		CATEGORY_MAX
	};

private:
	/// All existing renderers of this scene by categories.
	std::vector<KX_TextureRenderer *> m_renderers[CATEGORY_MAX];
	/// The camera used for renderers render, it's own by the renderer manager.
	KX_Camera *m_camera;
	/// The scene we are rendering for.
	KX_Scene *m_scene;

	/// Render a texture renderer, return true if the render was proceeded.
	bool RenderRenderer(RAS_Rasterizer *rasty, KX_TextureRenderer *renderer,
						KX_Camera *sceneCamera, const RAS_Rect& viewport, const RAS_Rect& area);

public:
	enum RendererType {
		CUBE,
		PLANAR
	};

	KX_TextureRendererManager(KX_Scene *scene);
	virtual ~KX_TextureRendererManager();

	/// Invalidate renderers using the given game object as viewpoint object.
	void InvalidateViewpoint(KX_GameObject *gameobj);
	void InvalidateRenderersProjectionMatrix();

	/** Add and create a renderer if none existing renderer was using the same
	* texture containing in the material texture passed.
	*/
	void AddRenderer(RendererType type, RAS_Texture *texture, KX_GameObject *viewpoint);

	/** Execute all the texture renderer.
	 * \param category The category of renderers to render.
	 * \param offScreen The off screen bound before rendering the texture renderers.
	 * \param sceneCamera The scene camera currently rendering the scene, used only in case of
	 * VIEWPORT_DEPENDENT category.
	 * \param viewport The viewport render area.
	 * \param area The windows render area.
	 */
	void Render(RendererCategory category, RAS_Rasterizer *rasty, RAS_OffScreen *offScreen,
				KX_Camera *sceneCamera, const RAS_Rect& viewport, const RAS_Rect& area);

	/// Merge the content of an other renderer manager, used during lib loading.
	void Merge(KX_TextureRendererManager *other);
};

#endif // __KX_TEXTURE_RENDERER_MANAGER_H__

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

#ifndef __KX_TEXTURE_PROBE_MANAGER_H__
#define __KX_TEXTURE_PROBE_MANAGER_H__

#include <vector>

class KX_GameObject;
class KX_Camera;
class KX_Scene;
class KX_TextureRenderer;

class RAS_IRasterizer;
class RAS_Texture;

class KX_TextureRendererManager
{
private:
	/// All existing renderers of this scene.
	std::vector<KX_TextureRenderer *> m_renderers;
	/// The camera used for renderers render, it's own by the renderer manager.
	KX_Camera *m_camera;
	/// The scene we are rendering for.
	KX_Scene *m_scene;

	void RenderProbe(RAS_IRasterizer *rasty, KX_TextureRenderer *renderer);

public:
	enum ProbeType {
		CUBE,
		PLANAR
	};

	KX_TextureRendererManager(KX_Scene *scene);
	virtual ~KX_TextureRendererManager();

	/// Invalidate renderers using the given game object as viewpoint object.
	void InvalidateViewpoint(KX_GameObject *gameobj);

	/** Add and create a renderer if none existing renderer was using the same
	* texture containing in the material texture passed.
	*/
	void AddProbe(ProbeType type, RAS_Texture *texture, KX_GameObject *viewpoint);

	void Render(RAS_IRasterizer *rasty);

	/// Merge the content of an other renderer manager, used during lib loading.
	void Merge(KX_TextureRendererManager *other);
};

#endif // __KX_TEXTURE_PROBE_MANAGER_H__

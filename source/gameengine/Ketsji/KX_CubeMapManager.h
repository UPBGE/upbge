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

/** \file KX_CubeMapManager.h
 *  \ingroup ketsji
 */

#ifndef __KX_CUBEMAPMANAGER_H__
#define __KX_CUBEMAPMANAGER_H__

#include <vector>

class KX_GameObject;
class KX_Camera;
class KX_Scene;
class KX_CubeMap;

class RAS_IRasterizer;
class RAS_Texture;

class KX_CubeMapManager
{
private:
	/// All existing realtime cube maps of this scene.
	std::vector<KX_CubeMap *> m_cubeMaps;

	/** The camera used for realtime cube map render.
	 * This camera is own by the cube map manager.
	 */
	KX_Camera *m_camera;

	/// The scene we are rendering for.
	KX_Scene *m_scene;

	void RenderCubeMap(RAS_IRasterizer *rasty, KX_CubeMap *cubemap);

public:
	KX_CubeMapManager(KX_Scene *scene);
	virtual ~KX_CubeMapManager();

	/** Add and create a cube map if none existing cube map was using the same
	 * texture containing in the material texture passed.
	 */
	void AddCubeMap(RAS_Texture *texture, KX_GameObject *viewpoint, KX_GameObject *cubemapobj);
	/// Invalidate cube map using the given game object as viewpoint object.
	void InvalidateCubeMapViewpoint(KX_GameObject *gameobj);

	std::vector<KX_CubeMap *>GetCubeMaps();

	void Render(RAS_IRasterizer *rasty);

	/// Merge the content of an other cube map manager, used during lib loading.
	void Merge(KX_CubeMapManager *other);
};

#endif // __KX_CUBEMAPMANAGER_H__

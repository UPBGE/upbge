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

/** \file BL_Texture.h
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

class KX_CubeMapManager
{
private:
	std::vector<KX_CubeMap *> m_cubeMaps;

	KX_Camera *m_camera;
	KX_Scene *m_scene;

	void RenderCubeMap(RAS_IRasterizer *rasty, KX_CubeMap *cubemap);

public:
	KX_CubeMapManager(KX_Scene *scene);
	virtual ~KX_CubeMapManager();

	void AddCubeMap(KX_CubeMap *cubeMap);
	void RemoveCubeMap(KX_GameObject *gameobj);

	void Render(RAS_IRasterizer *rasty);

};

#endif // __KX_CUBEMAPMANAGER_H__

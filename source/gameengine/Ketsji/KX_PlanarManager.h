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

/** \file KX_PlanarManager.h
*  \ingroup ketsji
*/

#ifndef __KX_PLANARMANAGER_H__
#define __KX_PLANARMANAGER_H__

#include <vector>

class KX_GameObject;
class KX_Camera;
class KX_Scene;
class KX_Planar;

class RAS_IRasterizer;
class RAS_Texture;
class RAS_IPolyMaterial;

class KX_PlanarManager
{
private:
	/// All existing realtime planars of this scene.
	std::vector<KX_Planar *> m_planars;

	/** The camera used for realtime planars render.
	* This camera is own by the planar manager.
	*/
	KX_Camera *m_camera;

	/// The scene we are rendering for.
	KX_Scene *m_scene;

	void RenderPlanar(RAS_IRasterizer *rasty, KX_Planar *planar);

public:
	KX_PlanarManager(KX_Scene *scene);
	virtual ~KX_PlanarManager();

	/** Add and create a planar if none existing planar was using the same
	* texture containing in the material texture passed.
	*/
	void AddPlanar(RAS_Texture *texture, KX_GameObject *gameobj, RAS_IPolyMaterial *polymat, short type, int width, int height);

	void Render(RAS_IRasterizer *rasty);
};

#endif // __KX_PLANARMANAGER_H__

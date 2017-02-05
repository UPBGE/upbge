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

/** \file KX_TextureProbeManager.h
*  \ingroup ketsji
*/

#ifndef __KX_TEXTURE_PROBE_MANAGER_H__
#define __KX_TEXTURE_PROBE_MANAGER_H__

#include <vector>

class KX_GameObject;
class KX_Camera;
class KX_Scene;
class KX_TextureProbe;

class RAS_IRasterizer;
class RAS_Texture;

class KX_TextureProbeManager
{
private:
	/// All existing probes of this scene.
	std::vector<KX_TextureProbe *> m_probes;
	/// The camera used for probes render, it's own by the probe manager.
	KX_Camera *m_camera;
	/// The scene we are rendering for.
	KX_Scene *m_scene;

	void RenderProbe(RAS_IRasterizer *rasty, KX_TextureProbe *probe);

public:
	enum ProbeType {
		CUBE,
		PLANAR
	};

	KX_TextureProbeManager(KX_Scene *scene);
	virtual ~KX_TextureProbeManager();

	/// Invalidate probes using the given game object as viewpoint object.
	void InvalidateViewpoint(KX_GameObject *gameobj);

	/** Add and create a probe if none existing probe was using the same
	* texture containing in the material texture passed.
	*/
	void AddProbe(ProbeType type, RAS_Texture *texture, KX_GameObject *viewpoint);

	void Render(RAS_IRasterizer *rasty);

	/// Merge the content of an other probe manager, used during lib loading.
	void Merge(KX_TextureProbeManager *other);
};

#endif // __KX_TEXTURE_PROBE_MANAGER_H__

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

/** \file KX_TextureRenderer.h
 *  \ingroup ketsji
 */

#ifndef __KX_TEXTURE_RENDERER_H__
#define __KX_TEXTURE_RENDERER_H__

#include "EXP_Value.h"
#include "RAS_TextureRenderer.h"

class KX_GameObject;
class KX_Camera;
class KX_Scene;
class RAS_Rect;

struct EnvMap;

class KX_TextureRenderer : public EXP_Value, public RAS_TextureRenderer
{
	Py_Header(KX_TextureRenderer)

protected:
	/// View clip start.
	float m_clipStart;
	/// View clip end.
	float m_clipEnd;

private:
	/// The object used to render from its position.
	KX_GameObject *m_viewpointObject;

	/// The texture renderer is enabled for render.
	bool m_enabled;
	/// Layers to ignore during render.
	int m_ignoreLayers;


	/// Distance factor for level of detail.
	float m_lodDistanceFactor;

	/// True if the renderer is updated every frame.
	bool m_autoUpdate;
	/** True if the renderer need to be updated for the next frame.
	 * Generally used when m_autoUpdate is to false.
	 */
	bool m_forceUpdate;

public:
	KX_TextureRenderer(EnvMap *env, KX_GameObject *viewpoint);
	virtual ~KX_TextureRenderer();

	virtual std::string GetName() const;

	KX_GameObject *GetViewpointObject() const;
	void SetViewpointObject(KX_GameObject *gameobj);

	virtual void InvalidateProjectionMatrix() = 0;

	float GetLodDistanceFactor() const;
	void SetLodDistanceFactor(float lodfactor);

	virtual const mt::mat4& GetProjectionMatrix(RAS_Rasterizer *rasty, KX_Scene *scene, KX_Camera *sceneCamera,
													const RAS_Rect& viewport, const RAS_Rect& area) = 0;

	bool GetEnabled() const;
	int GetIgnoreLayers() const;

	float GetClipStart() const;
	float GetClipEnd() const;
	void SetClipStart(float start);
	void SetClipEnd(float end);

	// Return true when the texture renderer need to be updated.
	bool NeedUpdate();

	/// Setup camera position and orientation shared by all the faces, returns true when the render will be made.
	virtual bool SetupCamera(KX_Camera *sceneCamera, KX_Camera *camera) = 0;
	/// Setup camera position and orientation unique per faces, returns true when the render will be made.
	virtual bool SetupCameraFace(KX_Camera *camera, unsigned short index) = 0;

#ifdef WITH_PYTHON
	EXP_PYMETHOD_DOC_NOARGS(KX_TextureRenderer, update);

	PyObject *pyattr_get_viewpoint_object();
	bool pyattr_set_viewpoint_object(PyObject *value);
	float pyattr_get_clip_start();
	void pyattr_set_clip_start(float value);
	float pyattr_get_clip_end();
	void pyattr_set_clip_end(float value);
#endif
};

#endif  // __KX_TEXTURE_RENDERER_H__

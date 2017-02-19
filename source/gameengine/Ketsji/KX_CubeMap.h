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

/** \file KX_CubeMap.h
 *  \ingroup ketsji
 */

#ifndef __KX_CUBEMAP_H__
#define __KX_CUBEMAP_H__

#include "RAS_CubeMap.h"
#include "EXP_Value.h"

#include "MT_Matrix4x4.h"

class KX_GameObject;

struct EnvMap;

class KX_CubeMap : public CValue, public RAS_CubeMap
{
	Py_Header

private:
	/// The object used to render from its position.
	KX_GameObject *m_viewpointObject;

	/// Object which has the cubemap texture
	KX_GameObject *m_cubeMapObject;

	/// The camera projection matrix depending on clip start/end.
	MT_Matrix4x4 m_projection;

	/// True if the projection matrix is invalid and need to be recomputed.
	bool m_invalidProjection;

	/// The cube map is used by the user.
	bool m_enabled;
	/// Layers to ignore during render.
	int m_ignoreLayers;

	/// View clip start.
	float m_clipStart;
	/// View clip end.
	float m_clipEnd;

	/// Distance factor for level of detail.
	float m_lodDistanceFactor;

	/// True if the realtime cube map is updated every frame.
	bool m_autoUpdate;
	/** True if the realtime cube map need to be updated for the next frame.
	 * Generally used when m_autoUpdate is to false.
	 */
	bool m_forceUpdate;

public:
	KX_CubeMap(EnvMap *env, KX_GameObject *viewpoint, KX_GameObject *cubemapobj);
	virtual ~KX_CubeMap();

	virtual std::string GetName();

	KX_GameObject *GetViewpointObject() const;
	void SetViewpointObject(KX_GameObject *gameobj);

	float GetClipStart() const;
	float GetClipEnd() const;
	void SetClipStart(float start);
	void SetClipEnd(float end);

	float GetLodDistanceFactor() const;
	void SetLodDistanceFactor(float lodfactor);

	void SetInvalidProjectionMatrix(bool invalid);
	bool GetInvalidProjectionMatrix() const;
	void SetProjectionMatrix(const MT_Matrix4x4& projection);
	const MT_Matrix4x4& GetProjectionMatrix() const;

	bool GetEnabled() const;
	int GetIgnoreLayers() const;

	KX_GameObject *GetGameObject();

	// Return true when this cube map need to be updated.
	bool NeedUpdate();

#ifdef WITH_PYTHON
	KX_PYMETHOD_DOC_NOARGS(KX_CubeMap, update);

	static PyObject *pyattr_get_viewpoint_object(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_viewpoint_object(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_clip_start(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_clip_start(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_clip_end(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_clip_end(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif
};

#endif  // __KX_CUBEMAP_H__

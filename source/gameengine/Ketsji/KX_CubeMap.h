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

class KX_BlenderSceneConverter;
class KX_GameObject;

class KX_CubeMap : public CValue, public RAS_CubeMap
{
	Py_Header
private:
	KX_GameObject *m_viewpointObject;

	MT_Matrix4x4 m_projection;

	bool m_invalidProjection;
	/// Layers to ignore during render.
	int m_ignoreLayers;

	float m_clipStart;
	float m_clipEnd;

	bool m_autoUpdate;
	bool m_forceUpdate;

public:
	KX_CubeMap(KX_GameObject *viewpoint, RAS_Texture *texture, RAS_IRasterizer *rasty);
	virtual ~KX_CubeMap();

	virtual STR_String& GetName();

	KX_GameObject *GetViewpointObject() const;
	void SetViewpointObject(KX_GameObject *gameobj);

	float GetClipStart() const;
	float GetClipEnd() const;
	void SetClipStart(float start);
	void SetClipEnd(float end);

	void SetInvalidProjectionMatrix(bool invalid);
	bool GetInvalidProjectionMatrix() const;
	void SetProjectionMatrix(const MT_Matrix4x4& projection);
	const MT_Matrix4x4& GetProjectionMatrix() const;

	int GetIgnoreLayers() const;

	// Return true when this cube map need to be updated.
	bool NeedUpdate();

#ifdef WITH_PYTHON
	KX_PYMETHOD_DOC_NOARGS(KX_CubeMap, update);

	static PyObject *pyattr_get_viewpoint_object(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_viewpoint_object(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_clip_start(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_clip_start(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_clip_end(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_clip_end(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif
};

#endif  // __KX_CUBEMAP_H__

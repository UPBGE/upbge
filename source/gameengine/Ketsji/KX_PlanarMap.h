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

/** \file KX_PlanarMap.h
*  \ingroup ketsji
*/

#ifndef __KX_PLANAR_H__
#define __KX_PLANAR_H__

#include "RAS_PlanarMap.h"
#include "EXP_Value.h"

#include "MT_Matrix4x4.h"

class KX_GameObject;
class RAS_IPolyMaterial;

struct EnvMap;

class KX_PlanarMap : public CValue, public RAS_PlanarMap
{
	Py_Header

private:
	/// The object used to render from its position.
	KX_GameObject *m_viewpointObject;

	/// The camera projection matrix depending on clip start/end.
	MT_Matrix4x4 m_projection;

	/// True if the projection matrix is invalid and need to be recomputed.
	bool m_invalidProjection;

	/// The planar is used by the user.
	bool m_enabled;
	/// Layers to ignore during render.
	int m_ignoreLayers;

	/// View clip start.
	float m_clipStart;
	/// View clip end.
	float m_clipEnd;

	/// True if the realtime planar is updated every frame.
	bool m_autoUpdate;
	/** True if the realtime planar need to be updated for the next frame.
	* Generally used when m_autoUpdate is to false.
	*/
	bool m_forceUpdate;

public:
	KX_PlanarMap(EnvMap *env, KX_GameObject *viewpoint, RAS_IPolyMaterial *polymat);
	virtual ~KX_PlanarMap();

	virtual std::string GetName();

	KX_GameObject *GetMirrorObject() const;

	float GetClipStart() const;
	float GetClipEnd() const;

	void SetClipStart(float start);
	void SetClipEnd(float end);

	void SetInvalidProjectionMatrix(bool invalid);
	bool GetInvalidProjectionMatrix() const;
	void SetProjectionMatrix(const MT_Matrix4x4& projection);
	const MT_Matrix4x4& GetProjectionMatrix() const;

	bool GetEnabled() const;
	int GetIgnoreLayers() const;

	// Return true when this planar need to be updated.
	bool NeedUpdate();

#ifdef WITH_PYTHON
	KX_PYMETHOD_DOC_NOARGS(KX_PlanarMap, update);

	static PyObject *pyattr_get_clip_start(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_clip_start(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_clip_end(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_clip_end(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif
};

#endif  // __KX_PLANAR_H__

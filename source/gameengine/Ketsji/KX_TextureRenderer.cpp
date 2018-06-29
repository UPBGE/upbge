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

/** \file KX_TextureRenderer.cpp
 *  \ingroup ketsji
 */

#include "KX_TextureRenderer.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"

#include "GPU_draw.h"

#include "DNA_texture_types.h"

KX_TextureRenderer::KX_TextureRenderer(MTex *mtex, KX_GameObject *viewpoint, LayerUsage layerUsage)
	:RAS_TextureRenderer((mtex->tex->env->filtering == ENVMAP_MIPMAP_MIPMAP),
			(mtex->tex->env->filtering == ENVMAP_MIPMAP_LINEAR), layerUsage),
	m_mtex(mtex),
	m_clipStart(mtex->tex->env->clipsta),
	m_clipEnd(mtex->tex->env->clipend),
	m_viewpointObject(viewpoint),
	m_enabled(true),
	m_ignoreLayers(mtex->tex->env->notlay),
	m_lodDistanceFactor(mtex->tex->env->lodfactor),
	m_autoUpdate(mtex->tex->env->flag & ENVMAP_AUTO_UPDATE),
	m_forceUpdate(true)
{
}

KX_TextureRenderer::~KX_TextureRenderer()
{
}

std::string KX_TextureRenderer::GetName()
{
	return "KX_TextureRenderer";
}

MTex *KX_TextureRenderer::GetMTex() const
{
	return m_mtex;
}

KX_GameObject *KX_TextureRenderer::GetViewpointObject() const
{
	return m_viewpointObject;
}

void KX_TextureRenderer::SetViewpointObject(KX_GameObject *gameobj)
{
	m_viewpointObject = gameobj;
}

bool KX_TextureRenderer::GetEnabled() const
{
	return m_enabled;
}

int KX_TextureRenderer::GetIgnoreLayers() const
{
	return m_ignoreLayers;
}

float KX_TextureRenderer::GetClipStart() const
{
	return m_clipStart;
}

float KX_TextureRenderer::GetClipEnd() const
{
	return m_clipEnd;
}

void KX_TextureRenderer::SetClipStart(float start)
{
	m_clipStart = start;
}

void KX_TextureRenderer::SetClipEnd(float end)
{
	m_clipEnd = end;
}

float KX_TextureRenderer::GetLodDistanceFactor() const
{
	return m_lodDistanceFactor;
}

void KX_TextureRenderer::SetLodDistanceFactor(float lodfactor)
{
	m_lodDistanceFactor = lodfactor;
}

bool KX_TextureRenderer::NeedUpdate()
{
	bool result = m_autoUpdate || m_forceUpdate;
	// Disable the force update for the next render.
	m_forceUpdate = false;

	return result;
}

#ifdef WITH_PYTHON

PyTypeObject KX_TextureRenderer::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_TextureRenderer",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_TextureRenderer::Methods[] = {
	EXP_PYMETHODTABLE_NOARGS(KX_TextureRenderer, update),
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef KX_TextureRenderer::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("viewpointObject", KX_TextureRenderer, pyattr_get_viewpoint_object, pyattr_set_viewpoint_object),
	EXP_PYATTRIBUTE_BOOL_RW("autoUpdate", KX_TextureRenderer, m_autoUpdate),
	EXP_PYATTRIBUTE_BOOL_RW("enabled", KX_TextureRenderer, m_enabled),
	EXP_PYATTRIBUTE_INT_RW("ignoreLayers", 0, (1 << 20) - 1, true, KX_TextureRenderer, m_ignoreLayers),
	EXP_PYATTRIBUTE_RW_FUNCTION("clipStart", KX_TextureRenderer, pyattr_get_clip_start, pyattr_set_clip_start),
	EXP_PYATTRIBUTE_RW_FUNCTION("clipEnd", KX_TextureRenderer, pyattr_get_clip_end, pyattr_set_clip_end),
	EXP_PYATTRIBUTE_FLOAT_RW("lodDistanceFactor", 0.0f, FLT_MAX, KX_TextureRenderer, m_lodDistanceFactor),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

EXP_PYMETHODDEF_DOC_NOARGS(KX_TextureRenderer, update, "update(): Set the texture rendered to be updated next frame.\n")
{
	m_forceUpdate = true;
	Py_RETURN_NONE;
}

PyObject *KX_TextureRenderer::pyattr_get_viewpoint_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_TextureRenderer *self = static_cast<KX_TextureRenderer *>(self_v);
	KX_GameObject *gameobj = self->GetViewpointObject();
	if (gameobj) {
		return gameobj->GetProxy();
	}
	Py_RETURN_NONE;
}

int KX_TextureRenderer::pyattr_set_viewpoint_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_TextureRenderer *self = static_cast<KX_TextureRenderer *>(self_v);
	KX_GameObject *gameobj = nullptr;

	SCA_LogicManager *logicmgr = KX_GetActiveScene()->GetLogicManager();

	if (!ConvertPythonToGameObject(logicmgr, value, &gameobj, true, "renderer.object = value: KX_TextureRenderer")) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetViewpointObject(gameobj);
	return PY_SET_ATTR_SUCCESS;
}


PyObject *KX_TextureRenderer::pyattr_get_clip_start(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_TextureRenderer *self = static_cast<KX_TextureRenderer *>(self_v);
	return PyFloat_FromDouble(self->GetClipStart());
}

int KX_TextureRenderer::pyattr_set_clip_start(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_TextureRenderer *self = static_cast<KX_TextureRenderer *>(self_v);

	const float val = PyFloat_AsDouble(value);

	if (val <= 0.0f) {
		PyErr_SetString(PyExc_AttributeError, "cubeMap.clipStart = float: KX_TextureRenderer, expected a float grater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->SetClipStart(val);
	self->InvalidateProjectionMatrix();

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_TextureRenderer::pyattr_get_clip_end(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_TextureRenderer *self = static_cast<KX_TextureRenderer *>(self_v);
	return PyFloat_FromDouble(self->GetClipEnd());
}

int KX_TextureRenderer::pyattr_set_clip_end(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_TextureRenderer *self = static_cast<KX_TextureRenderer *>(self_v);

	const float val = PyFloat_AsDouble(value);

	if (val <= 0.0f) {
		PyErr_SetString(PyExc_AttributeError, "cubeMap.clipEnd = float: KX_TextureRenderer, expected a float grater than zero");
		return PY_SET_ATTR_FAIL;
	}

	self->SetClipEnd(val);
	self->InvalidateProjectionMatrix();

	return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON

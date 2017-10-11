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

#include "DNA_texture_types.h"

KX_TextureRenderer::KX_TextureRenderer(EnvMap *env, KX_GameObject *viewpoint)
	:m_clipStart(env->clipsta),
	m_clipEnd(env->clipend),
	m_viewpointObject(viewpoint),
	m_enabled(true),
	m_ignoreLayers(env->notlay),
	m_lodDistanceFactor(env->lodfactor),
	m_forceUpdate(true)
{
	m_autoUpdate = (env->flag & ENVMAP_AUTO_UPDATE) != 0;
}

KX_TextureRenderer::~KX_TextureRenderer()
{
}

std::string KX_TextureRenderer::GetName() const
{
	return "KX_TextureRenderer";
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

EXP_Attribute KX_TextureRenderer::Attributes[] = {
	EXP_ATTRIBUTE_RW_FUNCTION("viewpointObject", pyattr_get_viewpoint_object, pyattr_set_viewpoint_object),
	EXP_ATTRIBUTE_RW("autoUpdate", m_autoUpdate),
	EXP_ATTRIBUTE_RW("enabled", m_enabled),
	EXP_ATTRIBUTE_RW_RANGE("ignoreLayers", m_ignoreLayers, 0, (1 << 20) - 1, false),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("clipStart", pyattr_get_clip_start, pyattr_set_clip_start, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("clipEnd", pyattr_get_clip_end, pyattr_set_clip_end, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_RANGE("lodDistanceFactor", m_lodDistanceFactor, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_NULL // Sentinel
};

EXP_PYMETHODDEF_DOC_NOARGS(KX_TextureRenderer, update, "update(): Set the texture rendered to be updated next frame.\n")
{
	m_forceUpdate = true;
	Py_RETURN_NONE;
}

PyObject *KX_TextureRenderer::pyattr_get_viewpoint_object()
{
	return EXP_ConvertToPython(m_viewpointObject);
}

bool KX_TextureRenderer::pyattr_set_viewpoint_object(PyObject *value)
{
	KX_GameObject *gameobj = nullptr;

	if (!ConvertPythonToGameObject(KX_GetActiveScene(), value, &gameobj, true, "renderer.object = value: KX_TextureRenderer")) {
		return false;
	}

	SetViewpointObject(gameobj);
	return true;
}

float KX_TextureRenderer::pyattr_get_clip_start()
{
	return m_clipStart;
}

void KX_TextureRenderer::pyattr_set_clip_start(float value)
{
	SetClipStart(value);
	InvalidateProjectionMatrix();
}

float KX_TextureRenderer::pyattr_get_clip_end()
{
	return m_clipEnd;
}

void KX_TextureRenderer::pyattr_set_clip_end(float value)
{
	SetClipEnd(value);
	InvalidateProjectionMatrix();
}

#endif  // WITH_PYTHON

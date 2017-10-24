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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_WorldInfo.cpp
 *  \ingroup ketsji
 */


#include "KX_WorldInfo.h"
#include "KX_PyMath.h"
#include "RAS_Rasterizer.h"
#include "GPU_material.h"

/* This little block needed for linking to Blender... */
#ifdef WIN32
#include "BLI_winstuff.h"
#endif

extern "C" {
#  include "eevee_private.h"
#  include "DRW_render.h"
}

/* This list includes only data type definitions */
#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BLI_math.h"

#include "BKE_global.h"
#include "BKE_scene.h"
/* end of blender include block */

KX_WorldInfo::KX_WorldInfo(Scene *blenderscene, World *blenderworld)
	:m_scene(blenderscene)
{
	m_name = blenderworld->id.name + 2;
}

KX_WorldInfo::~KX_WorldInfo()
{
}

const std::string& KX_WorldInfo::GetName()
{
	return m_name;
}

void KX_WorldInfo::RenderBackground()
{
	DRW_draw_pass(EEVEE_engine_data_get()->psl->background_pass);
}

#ifdef WITH_PYTHON

/* -------------------------------------------------------------------------
 * Python functions
 * ------------------------------------------------------------------------- */
PyObject *KX_WorldInfo::py_repr(void)
{
	return PyUnicode_FromStdString(GetName());
}

/* -------------------------------------------------------------------------
 * Python Integration Hooks
 * ------------------------------------------------------------------------- */
PyTypeObject KX_WorldInfo::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_WorldInfo",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,0,0,0,0,0,0,0,0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0,0,0,0,0,0,0,
	Methods,
	0,
	0,
	&PyObjectPlus::Type,
	0,0,0,0,0,0,
	py_base_new
};

PyMethodDef KX_WorldInfo::Methods[] = {
	{nullptr,nullptr} /* Sentinel */
};

PyAttributeDef KX_WorldInfo::Attributes[] = {
	KX_PYATTRIBUTE_NULL /* Sentinel */
};

#endif /* WITH_PYTHON */

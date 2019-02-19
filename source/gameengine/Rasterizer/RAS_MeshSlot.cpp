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

/** \file gameengine/Rasterizer/RAS_MeshSlot.cpp
 *  \ingroup bgerast
 */

#include "RAS_MeshSlot.h"
#include "RAS_MaterialShader.h"
#include "RAS_TexVert.h"
#include "RAS_MeshObject.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"
#include "DNA_scene_types.h"

#include "KX_Globals.h"
#include "KX_Scene.h"
#include "KX_Camera.h"

extern "C" {
#  include "../gpu/intern/gpu_codegen.h"
}

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

// mesh slot
RAS_MeshSlot::RAS_MeshSlot(RAS_MeshObject *mesh, RAS_DisplayArrayBucket *arrayBucket)
	:m_displayArrayBucket(arrayBucket),
	m_mesh(mesh),
	m_pDerivedMesh(nullptr)
{
}

RAS_MeshSlot::~RAS_MeshSlot()
{
}

void RAS_MeshSlot::SetDisplayArrayBucket(RAS_DisplayArrayBucket *arrayBucket)
{
	m_displayArrayBucket = arrayBucket;
}

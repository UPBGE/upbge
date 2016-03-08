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

/** \file RAS_MeshMaterial.h
 *  \ingroup bgerast
 */

#ifndef __RAS_MESH_MATERIAL_H__
#define __RAS_MESH_MATERIAL_H__

#include <map>

class RAS_MaterialBucket;
class RAS_MeshSlot;

// Used by RAS_MeshObject, to point to it's slots in a bucket
class RAS_MeshMaterial
{
public:
	RAS_MeshSlot *m_baseslot;
	RAS_MaterialBucket *m_bucket;
	/// The material index position in the mesh.
	unsigned int m_index;

	/// the KX_GameObject is used as a key here
	std::map<void *, RAS_MeshSlot *> m_slots;

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_MeshMaterial")
#endif
};

#endif  // __RAS_MESH_MATERIAL_H__

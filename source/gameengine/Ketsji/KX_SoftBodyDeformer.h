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

/** \file KX_SoftBodyDeformer.h
 *  \ingroup bgeconv
 */

#ifndef __KX_SOFTBODYDEFORMER_H__
#define __KX_SOFTBODYDEFORMER_H__

#include "RAS_Deformer.h"

class KX_GameObject;

class KX_SoftBodyDeformer : public RAS_Deformer
{
	KX_GameObject *m_gameobj;
	/** Set to true to request an AABB update in Apply(mat).
	 * Used to compute a fully AABB and not for only one material.
	 */
	bool m_needUpdateAabb;

public:
	KX_SoftBodyDeformer(RAS_Mesh *pMeshObject, KX_GameObject *gameobj);
	virtual ~KX_SoftBodyDeformer();

	virtual void Apply(RAS_DisplayArray *array);
	virtual bool Update()
	{
		m_bDynamic = true;
		return true;
	}
	virtual void UpdateBuckets()
	{
		// invalidate the AABB for each read acces.
		m_needUpdateAabb = true;
		// this is to update the mesh slots outside the rasterizer,
		// no need to do it for this deformer, it's done in any case in Apply()
	}

	virtual bool SkipVertexTransform()
	{
		return true;
	}
};

#endif


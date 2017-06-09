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

/** \file RAS_Deformer.h
 *  \ingroup bgerast
 */

#ifndef __RAS_DEFORMER_H__
#define __RAS_DEFORMER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* get rid of stupid stl-visual compiler debug warning */
#endif

#include "RAS_BoundingBox.h"
#include "RAS_IDisplayArray.h" // For RAS_IDisplayArrayList.
#include "RAS_DisplayArrayBucket.h" // For RAS_DisplayArrayBucketList.

#include <map>

struct DerivedMesh;
class RAS_MeshObject;
class RAS_IPolyMaterial;
class RAS_MeshMaterial;
class SCA_IObject;

class RAS_Deformer
{
public:
	RAS_Deformer(RAS_MeshObject *mesh);
	virtual ~RAS_Deformer();

	void InitializeDisplayArrays();

	virtual void Relink(std::map<SCA_IObject *, SCA_IObject *>& map) = 0;
	virtual void Apply(RAS_MeshMaterial *meshmat, RAS_IDisplayArray *array) = 0;
	virtual bool Update(void)=0;
	virtual void UpdateBuckets(void)=0;
	virtual RAS_Deformer *GetReplica()=0;
	virtual void ProcessReplica();

	// true when deformer produces varying vertex (shape or armature)
	bool IsDynamic()
	{
		return m_bDynamic;
	}

	virtual bool SkipVertexTransform()
	{
		return false;
	}

	RAS_BoundingBox *GetBoundingBox() const
	{
		return m_boundingBox;
	}

	RAS_MeshObject *GetMesh() const;

	RAS_IDisplayArray *GetDisplayArray(unsigned short index) const;
	RAS_DisplayArrayBucket *GetDisplayArrayBucket(unsigned short index) const;

protected:
	RAS_MeshObject *m_mesh;
	bool m_bDynamic;

	RAS_IDisplayArrayList m_displayArrayList;
	RAS_DisplayArrayBucketList m_displayArrayBucketList;

	/// Deformer bounding box.
	RAS_BoundingBox *m_boundingBox;
};

#endif


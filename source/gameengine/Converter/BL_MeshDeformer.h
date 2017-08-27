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

/** \file BL_MeshDeformer.h
 *  \ingroup bgeconv
 */

#ifndef __BL_MESHDEFORMER_H__
#define __BL_MESHDEFORMER_H__

#include "RAS_Deformer.h"
#include "BL_DeformableGameObject.h"
#include "DNA_object_types.h"
#include "DNA_key_types.h"
#include "MT_Vector3.h"
#include <stdlib.h>

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* get rid of stupid stl-visual compiler debug warning */
#endif

struct Object;
struct Mesh;
class BL_DeformableGameObject;
class RAS_MeshObject;
class RAS_IPolyMaterial;

class BL_MeshDeformer : public RAS_Deformer
{
public:
	void VerifyStorage();
	void RecalcNormals();
	virtual void Relink(std::map<SCA_IObject *, SCA_IObject *>& map);

	BL_MeshDeformer(BL_DeformableGameObject *gameobj, Object *obj, RAS_MeshObject *meshobj);
	virtual ~BL_MeshDeformer();
	virtual void Apply(RAS_MeshMaterial *meshmat, RAS_IDisplayArray *array);
	virtual bool Update()
	{
		return false;
	}
	virtual void UpdateBuckets()
	{
	}
	virtual RAS_Deformer *GetReplica()
	{
		return nullptr;
	}
	virtual void ProcessReplica();
	Mesh *GetMesh()
	{
		return m_bmesh;
	}

protected:
	Mesh *m_bmesh;

	// this is so m_transverts doesn't need to be converted
	// before deformation
	std::vector<std::array<float, 3> > m_transverts;
	std::vector<std::array<float, 3> > m_transnors;
	Object *m_objMesh;

	BL_DeformableGameObject *m_gameobj;
	double m_lastDeformUpdate;
};

#endif


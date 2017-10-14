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

/** \file BL_SkinDeformer.h
 *  \ingroup bgeconv
 */

#ifndef __BL_SKINDEFORMER_H__
#define __BL_SKINDEFORMER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* get rid of stupid stl-visual compiler debug warning */
#endif  /* WIN32 */

#include "BL_MeshDeformer.h"
#include "BL_ArmatureObject.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "BKE_armature.h"

struct Object;
struct bPoseChannel;
class RAS_Mesh;
class RAS_IPolyMaterial;

class BL_SkinDeformer : public BL_MeshDeformer
{
public:
	BL_SkinDeformer(KX_GameObject *gameobj,
					Object *bmeshobj_old,
					Object *bmeshobj_new,
					RAS_Mesh *mesh,
					BL_ArmatureObject *arma);

	virtual ~BL_SkinDeformer();
	virtual void Update(unsigned short reason);
	virtual unsigned short NeedUpdate() const;
	void UpdateInternal(bool shape_applied);

protected:
	BL_ArmatureObject *m_armobj; // Our parent object
	double m_lastArmaUpdate;
	float m_obmat[4][4]; // the reference matrix for skeleton deform
	bool m_copyNormals; // dirty flag so we know if UpdateTransverts() needs to copy normal information (used for BGEDeformVerts())
	std::vector<bPoseChannel *> m_dfnrToPC;
	short m_deformflags;

	void BlenderDeformVerts();
	void BGEDeformVerts();

	virtual void UpdateTransverts();
	void UpdateDisplayArrays();
};

#endif  /* __BL_SKINDEFORMER_H__ */

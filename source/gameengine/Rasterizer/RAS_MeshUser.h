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
 * The Original Code is: all of this file.
 *
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_MeshUser.h
 *  \ingroup bgerast
 */

#ifndef __RAS_MESH_USER_H__
#define __RAS_MESH_USER_H__

#include "RAS_MeshSlot.h"

#include <memory>

class RAS_BoundingBox;
class RAS_BatchGroup;
class RAS_Deformer;

class RAS_MeshUser : public mt::SimdClassAllocator
{
private:
	/// Lamp layer.
	unsigned int m_layer;
	/// Object pass index.
	short m_passIndex;
	/// Random value of this user.
	float m_random;
	/// OpenGL face wise.
	bool m_frontFace;
	/// Object color.
	mt::vec4 m_color;
	/// Object transformation matrix.
	mt::mat4 m_matrix;
	/// Bounding box corresponding to a mesh or deformer.
	RAS_BoundingBox *m_boundingBox;
	/// Client object owner of this mesh user.
	void *m_clientObject;
	/// Unique mesh slots used for render of this object.
	std::vector<RAS_MeshSlot> m_meshSlots;
	/// Possible batching groups shared between mesh users.
	RAS_BatchGroup *m_batchGroup;
	/// Deformer of this mesh user modifying the display array of the mesh slots.
	std::unique_ptr<RAS_Deformer> m_deformer;

public:
	RAS_MeshUser(void *clientobj, RAS_BoundingBox *boundingBox, RAS_Deformer *deformer);
	virtual ~RAS_MeshUser();

	void NewMeshSlot(RAS_DisplayArrayBucket *arrayBucket);
	unsigned int GetLayer() const;
	short GetPassIndex() const;
	float GetRandom() const;
	bool GetFrontFace() const;
	const mt::vec4& GetColor() const;
	const mt::mat4& GetMatrix() const;
	RAS_BoundingBox *GetBoundingBox() const;
	void *GetClientObject() const;
	std::vector<RAS_MeshSlot>& GetMeshSlots();
	RAS_BatchGroup *GetBatchGroup() const;
	RAS_Deformer *GetDeformer();

	void SetLayer(unsigned int layer);
	void SetPassIndex(short index);
	void SetFrontFace(bool frontFace);
	void SetColor(const mt::vec4& color);
	void SetMatrix(const mt::mat4& matrix);
	void SetBatchGroup(RAS_BatchGroup *batchGroup);

	void ActivateMeshSlots();
};

#endif  // __RAS_MESH_USER_H__

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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_BoundingBox.h
 *  \ingroup bgerast
 */

#ifndef __RAS_BOUNDING_BOX_H__
#define __RAS_BOUNDING_BOX_H__

#include "RAS_DisplayArray.h"

class RAS_BoundingBoxManager;

class RAS_BoundingBox : public mt::SimdClassAllocator
{
protected:
	/// True when the bounding box is modified.
	bool m_modified;

	/// The AABB minimum.
	mt::vec3 m_aabbMin;
	/// The AABB maximum.
	mt::vec3 m_aabbMax;

	/// The number of mesh user using this bounding box.
	int m_users;
	/// The manager of all the bounding boxes of a scene.
	RAS_BoundingBoxManager *m_manager;

public:
	RAS_BoundingBox(RAS_BoundingBoxManager *manager);
	virtual ~RAS_BoundingBox();

	virtual RAS_BoundingBox *GetReplica();
	void ProcessReplica();

	/// Notice that the bounding box is used by one more mesh user.
	void AddUser();
	/// Notice that the bounding box is left by one mesh user.
	void RemoveUser();

	/// Change the bounding box manager. Used only for the libloading scene merge.
	void SetManager(RAS_BoundingBoxManager *manager);

	/** Return true when the bounding box AABB was set or when the display
	 * array were modified in case of RAS_MeshBoundingBox instance.
	 */
	bool GetModified() const;
	/// Set the bounding box unmodified.
	void ClearModified();

	void GetAabb(mt::vec3& aabbMin, mt::vec3& aabbMax) const;
	void SetAabb(const mt::vec3& aabbMin, const mt::vec3& aabbMax);
	/// Compute the AABB of the bounding box AABB mixed with the passed AABB.
	void ExtendAabb(const mt::vec3& aabbMin, const mt::vec3& aabbMax);

	void CopyAabb(RAS_BoundingBox *other);

	virtual void Update(bool force);
};

class RAS_MeshBoundingBox : public RAS_BoundingBox
{
private:
	/// Display arrays used to compute the AABB.
	struct DisplayArraySlot
	{
		RAS_DisplayArray *m_displayArray;
		CM_UpdateClient<RAS_DisplayArray> m_arrayUpdateClient;
	};

	/// The sub AABB per display array.
	std::vector<DisplayArraySlot> m_slots;

public:
	RAS_MeshBoundingBox(RAS_BoundingBoxManager *manager, const RAS_DisplayArrayList& displayArrayList);
	virtual ~RAS_MeshBoundingBox();

	virtual RAS_BoundingBox *GetReplica();

	/** Check if one of the display array was modified, and then recompute the AABB.
	 * \param force Force the AABB computation even if none display arrays are modified.
	 */
	virtual void Update(bool force);
};

typedef std::vector<RAS_BoundingBox *> RAS_BoundingBoxList;

#endif  // __RAS_BOUNDING_BOX_H__

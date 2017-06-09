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

/** \file RAS_BatchGroup.h
 *  \ingroup bgerast
 */

#ifndef __RAS_BATCH_GROUP_H__
#define __RAS_BATCH_GROUP_H__

#include "RAS_DisplayArrayBucket.h"

class RAS_IPolyMaterial;
class RAS_IBatchDisplayArray;

class RAS_BatchGroup
{
private:
	/// The reference counter.
	short m_users;

	/// A batch contained the merged display array for all the display array used for a given material.
	class Batch
	{
	public:
		Batch();

		/// The display array bucket owning the merged display array.
		RAS_DisplayArrayBucket *m_displayArrayBucket;
		/// The merged display array.
		RAS_IBatchDisplayArray *m_displayArray;

		/// The original display array bucket per mesh slots.
		std::map<RAS_MeshSlot *, RAS_DisplayArrayBucket *> m_originalDisplayArrayBucketList;
		/// All the mesh slots sorted by batch index.
		RAS_MeshSlotList m_meshSlots;
	};

	/// The batch per material.
	std::map<RAS_IPolyMaterial *, Batch> m_batchs;

	/** Merge the display array of the passed mesh slot.
	 * \param slot The mesh slot using the display array to merge.
	 * \param mat The transform matrix to apply to vertices during merging.
	 * \return false on failure.
	 */
	bool MergeMeshSlot(Batch& batch, RAS_MeshSlot *slot, const mt::mat4& mat);

	/** Split the part representing the display array containing in the passed mesh slot.
	 * \param slot The mesh slot using the display array to split.
	 * \return false on failure.
	 */
	bool SplitMeshSlot(RAS_MeshSlot *slot);

public:
	RAS_BatchGroup();
	virtual ~RAS_BatchGroup();

	/// Notice the batch group that it is used by one more mesh user.
	RAS_BatchGroup *AddMeshUser();
	/// Notice the batch group that it is unused by one less mesh user.
	RAS_BatchGroup *RemoveMeshUser();

	/** Merge the display array of the mesh slots contained in the mesh user.
	 * \param meshUser The mesh user to merge mesh slots from.
	 * \param mat The object matrix to use in display array merging. It's not the matrix from
	 * the mesh user because this one can be not updated.
	 * \return false on failure.
	 */
	bool MergeMeshUser(RAS_MeshUser *meshUser, const mt::mat4& mat);

	/** Split the display array of the mesh slots contained in the mesh user.
	 * \param meshUser THe mesh user to merge mesh slots from.
	 * \return false on failure.
	 */
	bool SplitMeshUser(RAS_MeshUser *meshUser);

	/** Restore the display array (bucket) of all the mesh slots using this batch group.
	 * The side effect is to make the batch group unused and then deleted from reference
	 * counting.
	 */
	void Destruct();
};


#endif  // __RAS_BATCH_GROUP_H__

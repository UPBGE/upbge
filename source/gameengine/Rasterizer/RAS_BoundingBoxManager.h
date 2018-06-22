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

/** \file RAS_BoundingBoxManager.h
 *  \ingroup bgerast
 */

#ifndef __RAS_BOUNDING_BOX_MANAGER_H__
#define __RAS_BOUNDING_BOX_MANAGER_H__

#include "RAS_BoundingBox.h"

class RAS_BoundingBoxManager
{
	// To manage the insert remove in m_boundingBoxList and m_activeBoundingBoxList.
	friend RAS_BoundingBox;
private:
	/// All the existing bounding boxes for this manager.
	RAS_BoundingBoxList m_boundingBoxList;
	/** All the bounding boxes used by at least one mesh user.
	 * These bounding boxes will be updated each frames.
	 */
	RAS_BoundingBoxList m_activeBoundingBoxList;

public:
	RAS_BoundingBoxManager();
	~RAS_BoundingBoxManager();

	/** Create the bounding box from the bounding box manager. The reason to
	 * do that is that the scene owning the bounding box manager will be freed
	 * before the deformers and meshes.
	 */
	RAS_BoundingBox *CreateBoundingBox();
	/** Create mesh bounding box.
	 * \param arrayList The display arrays composing the mesh.
	 * \see CreateBoundingBox.
	 */
	RAS_BoundingBox *CreateMeshBoundingBox(const RAS_DisplayArrayList& arrayList);

	/** Update all the active bounding boxes.
	 * \param force Force updating bounding box even if the display arrays are not modified.
	 */
	void Update(bool force);
	/// Set all the active bounding box unmodified.
	void ClearModified();

	/** Merge an other bounding box manager.
	 * \param other The bounding box manager to merge data from. This manager is empty after the merge.
	 */
	void Merge(RAS_BoundingBoxManager *other);
};

#endif  // __RAS_MESH_BOUNDING_BOX_MANAGER_H__

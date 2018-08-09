#ifndef __RAS_SORTED_MESH_SLOT_H__
#define __RAS_SORTED_MESH_SLOT_H__

#include "RAS_MeshSlot.h" // For RAS_MeshSlotList.

class RAS_SortedMeshSlot
{
public:
	/// depth
	float m_z;

	union {
		RAS_MeshSlot *m_ms;
		RAS_MeshSlotUpwardNode *m_node;
	};

	RAS_SortedMeshSlot() = default;
	RAS_SortedMeshSlot(RAS_MeshSlot *ms, const mt::vec3& pnorm);
	RAS_SortedMeshSlot(RAS_MeshSlotUpwardNode *node, const mt::vec3& pnorm);
	RAS_SortedMeshSlot(RAS_MeshUser *meshUser, RAS_MeshSlot *ms, const mt::vec3& pnorm);

	static std::vector<RAS_SortedMeshSlot> Sort(const RAS_MeshSlotList& slots, const mt::mat3x4& trans);
	static std::vector<RAS_SortedMeshSlot> Sort(const RAS_UpwardTreeLeafs& leafs, const mt::mat3x4& trans);
	static std::vector<RAS_SortedMeshSlot> Sort(std::vector<RAS_SortedMeshSlot>& slots);
};

using RAS_SortedMeshSlotList = std::vector<RAS_SortedMeshSlot>;

#endif  // __RAS_SORTED_MESH_SLOT_H__

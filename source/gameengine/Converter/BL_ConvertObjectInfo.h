#ifndef __BL_CONVERT_OBJECT_INFO__
#define __BL_CONVERT_OBJECT_INFO__

#include <vector>

struct bRigidBodyJointConstraint;

struct BL_ConvertObjectInfo
{
	/// Blender object used during conversion.
	Object *m_blenderObject;
	/// Object constraints defined by the user.
	std::vector<bRigidBodyJointConstraint *> m_constraints;
};

#endif  // __BL_CONVERT_OBJECT_INFO__

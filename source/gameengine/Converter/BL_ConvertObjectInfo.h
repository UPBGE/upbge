#ifndef __BL_CONVERT_OBJECT_INFO__
#define __BL_CONVERT_OBJECT_INFO__

#include "BL_Resource.h"

#include <vector>

struct bRigidBodyJointConstraint;
struct Object;

class BL_ConvertObjectInfo : public BL_Resource
{
public:
	/// Blender object used during conversion.
	Object *m_blenderObject;
	/// Object constraints defined by the user.
	std::vector<bRigidBodyJointConstraint *> m_constraints;

	BL_ConvertObjectInfo(Object *blendobj);
};

#endif  // __BL_CONVERT_OBJECT_INFO__

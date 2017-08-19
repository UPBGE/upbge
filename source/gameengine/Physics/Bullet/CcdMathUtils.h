#ifndef __CCD_MATH_UTILS__
#define __CCD_MATH_UTILS__

#include "mathfu.h"

#include "LinearMath/btVector3.h"
#include "LinearMath/btQuaternion.h"
#include "LinearMath/btMatrix3x3.h"

inline mt::vec3 ToMoto(const btVector3& vec)
{
	return mt::vec3(vec.x(), vec.y(), vec.z());
}

inline mt::vec4 ToMoto(const btVector4& vec)
{
	return mt::vec4(vec.x(), vec.y(), vec.z(), vec.w());
}

inline mt::mat3 ToMoto(const btMatrix3x3& mat)
{
	return mt::mat3(mat[0][0], mat[1][0], mat[2][0],
						mat[0][1], mat[1][1], mat[2][1],
						mat[0][2], mat[1][2], mat[2][2]);
}

inline mt::quat ToMoto(const btQuaternion& quat)
{
	return mt::quat(quat.w(), quat.x(), quat.y(), quat.z());
}

inline btVector3 ToBullet(const mt::vec3& vec)
{
	return btVector3(vec.x, vec.y, vec.z);
}

inline btVector4 ToBullet(const mt::vec4& vec)
{
	return btVector4(vec.x, vec.y, vec.z, vec.w);
}

inline btMatrix3x3 ToBullet(const mt::mat3& mat)
{
	return btMatrix3x3(mat(0, 0), mat(0, 1), mat(0, 2),
					   mat(1, 0), mat(1, 1), mat(1, 2),
					   mat(2, 0), mat(2, 1), mat(2, 2));
}

inline btQuaternion ToBullet(const mt::quat& quat)
{
	return btQuaternion(quat.x, quat.y, quat.z, quat.w);
}

#endif  // __CCD_MATH_UTILS__

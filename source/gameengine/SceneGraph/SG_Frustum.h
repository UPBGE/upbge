#ifndef __SG_FRUSTUM_H__
#define __SG_FRUSTUM_H__

#include "mathfu.h"

#include <array>

/// \brief Camera frustum data.
class SG_Frustum
{
private:
	/// Camera modelview multiplied by projection.
	mt::mat4 m_matrix;
	/// Frustum planes.
	std::array<mt::vec4, 6> m_planes;

public:
	enum TestType
	{
		INSIDE,
		INTERSECT,
		OUTSIDE
	};

	SG_Frustum() = default;
	SG_Frustum(const mt::mat4& matrix);
	~SG_Frustum() = default;

	const std::array<mt::vec4, 6>& GetPlanes() const;
	const mt::mat4& GetMatrix() const;

	TestType PointInsideFrustum(const mt::vec3& point) const;
	TestType SphereInsideFrustum(const mt::vec3& center, float radius) const;
	TestType BoxInsideFrustum(const std::array<mt::vec3, 8>& box) const;
	TestType AabbInsideFrustum(const mt::vec3& min, const mt::vec3& max, const mt::mat4& mat) const;
	TestType FrustumInsideFrustum(const SG_Frustum& frustum) const;
};

#endif  // __SG_FRUSTUM_H__

#include "SG_Frustum.h"

SG_Frustum::SG_Frustum(const mt::mat4& matrix)
	:m_matrix(matrix)
{
	// Near clip plane
	m_planes[0] = m_matrix.GetRow(3) + m_matrix.GetRow(2);
	// Far clip plane
	m_planes[1] = m_matrix.GetRow(3) - m_matrix.GetRow(2);
	// Left clip plane
	m_planes[2] = m_matrix.GetRow(3) + m_matrix.GetRow(0);
	// Right clip plane
	m_planes[3] = m_matrix.GetRow(3) - m_matrix.GetRow(0);
	// Top clip plane
	m_planes[4] = m_matrix.GetRow(3) - m_matrix.GetRow(1);
	// Bottom clip plane
	m_planes[5] = m_matrix.GetRow(3) + m_matrix.GetRow(1);

	// Normalize clip planes.
	for (mt::vec4& plane : m_planes) {
		const float factor = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
		if (!mt::FuzzyZero(factor)) {
			plane /= factor;
		}
	}
}

const std::array<mt::vec4, 6>& SG_Frustum::GetPlanes() const
{
	return m_planes;
}

const mt::mat4& SG_Frustum::GetMatrix() const
{
	return m_matrix;
}

static inline float planeSide(const mt::vec4& plane, const mt::vec3& point)
{
	return (plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w);
}

SG_Frustum::TestType SG_Frustum::PointInsideFrustum(const mt::vec3& point) const
{
	for (const mt::vec4& plane : m_planes) {
		if (planeSide(plane, point) < 0.0f) {
			return OUTSIDE;
		}
	}

	return INSIDE;
}

SG_Frustum::TestType SG_Frustum::SphereInsideFrustum(const mt::vec3& center, float radius) const
{
	for (const mt::vec4& plane : m_planes) {
		const float distance = planeSide(plane, center);
		if (distance < -radius) {
			return OUTSIDE;
		}
		else if (fabs(distance) <= radius) {
			return INTERSECT;
		}
	}

	return INSIDE;
}

SG_Frustum::TestType SG_Frustum::BoxInsideFrustum(const std::array<mt::vec3, 8>& box) const
{
	unsigned short insidePlane = 0;
	for (const mt::vec4& plane : m_planes) {
		unsigned short insidePoint = 0;
		for (const mt::vec3& point : box) {
			insidePoint += (planeSide(plane, point) < 0.0f) ? 0 : 1;
		}

		if (insidePoint == 0) {
			return OUTSIDE;
		}

		insidePlane += (insidePoint == 8) ? 1 : 0;
	}

	if (insidePlane == 6) {
		return INSIDE;
	}

	return INTERSECT;
}

static void getNearFarAabbPoint(const mt::vec4& plane, const mt::vec3& min, const mt::vec3& max, mt::vec3& near, mt::vec3& far)
{
	for (unsigned short axis = 0; axis < 3; ++axis) {
		if (plane[axis] < 0.0f) {
			near[axis] = max[axis];
			far[axis] = min[axis];
		}
		else {
			near[axis] = min[axis];
			far[axis] = max[axis];
		}
	}
}

static bool aabbIntersect(const mt::vec3& min1, const mt::vec3& max1, const mt::vec3& min2, const mt::vec3& max2)
{
	for (unsigned short axis = 0; axis < 3; ++axis) {
		if (max1[axis] < min2[axis] || min1[axis] > max2[axis]) {
			return false;
		}
	}

	return true;
}

SG_Frustum::TestType SG_Frustum::AabbInsideFrustum(const mt::vec3& min, const mt::vec3& max, const mt::mat4& mat) const
{
	TestType result = INSIDE;

	for (const mt::vec4& wplane : m_planes) {
		// Compute frustum plane in object space.
		const mt::vec4 oplane = wplane * mat;

		// Test near and far AABB vertices.
		mt::vec3 near;
		mt::vec3 far;

		// Generate nearest and further points from the positive plane side.
		getNearFarAabbPoint(oplane, min, max, near, far);

		// If the near point to the plane is out, all the other points are out.
		if (planeSide(oplane, far) < 0.0f) {
			return OUTSIDE;
		}
		// If the far plane is out, the AABB is intersecting this plane.
		else if (result != INTERSECT && planeSide(oplane, near) < 0.0f) {
			result = INTERSECT;
		}
	}

	/* Big object can intersect two "orthogonal" planes without be inside the frustum.
	 * In this case the object is outside the AABB of the frustum. */
	if (result == INTERSECT) {
		mt::vec3 fmin;
		mt::vec3 fmax;
		mt::FrustumAabb((m_matrix * mat).Inverse(), fmin, fmax);

		if (!aabbIntersect(min, max, fmin, fmax)) {
			return OUTSIDE;
		}
	}

	return result;
}

static int whichSide(const std::array<mt::vec3, 8>& box, const mt::vec4& plane)
{
	unsigned short positive = 0;
	unsigned short negative = 0;

	for (const mt::vec3& point : box) {
		const float t = planeSide(plane, point);
		if (mt::FuzzyZero(t)) {
			return 0;
		}

		negative += (t < 0.0f); // point outside
		positive += (t > 0.0f); // point inside

		if (positive > 0 && negative > 0) {
			return 0;
		}
	}

	return (positive > 0) ? 1 : -1;
}

static int whichSide(const std::array<mt::vec3, 8>& box, const mt::vec3& normal, const mt::vec3& vert)
{
	unsigned short positive = 0;
	unsigned short negative = 0;

	for (const mt::vec3& point : box) {
		const float t = mt::dot(normal, point - vert);
		if (mt::FuzzyZero(t)) {
			return 0;
		}

		negative += (t < 0.0f); // point outside
		positive += (t > 0.0f); // point inside

		if (positive > 0 && negative > 0) {
			return 0;
		}
	}

	return (positive > 0) ? 1 : -1;
}

SG_Frustum::TestType SG_Frustum::FrustumInsideFrustum(const SG_Frustum& frustum) const
{
	// Based on https://booksite.elsevier.com/9781558605930/revisionnotes/MethodOfSeperatingAxes.pdf

	/* First test if the vertices of the second frustum box are not fully oustide the
	 * planes of the first frustum.
	 */
	std::array<mt::vec3, 8> fbox2;
	mt::FrustumBox(frustum.m_matrix.Inverse(), fbox2);

	for (const mt::vec4& plane : m_planes) {
		if (whichSide(fbox2, plane) < 0) {
			return OUTSIDE;
		}
	}

	// Test with first frustum box and second frustum planes.
	std::array<mt::vec3, 8> fbox1;
	mt::FrustumBox(m_matrix.Inverse(), fbox1);

	for (const mt::vec4& plane : frustum.m_planes) {
		if (whichSide(fbox1, plane) < 0) {
			return OUTSIDE;
		}
	}

	/* Test edge separation axis, they are produced by the cross product of
	 * edge from the both frustums.
	 */
	std::array<mt::vec3, 12> fedges1;
	std::array<mt::vec3, 12> fedges2;

	mt::FrustumEdges(fbox1, fedges1);
	mt::FrustumEdges(fbox2, fedges2);

	for (unsigned short i = 0; i < 12; ++i) {
		const mt::vec3& edge1 = fedges1[i];
		// Origin of the separation axis.
		const mt::vec3& vert = fbox1[mt::FrustumEdgeVertex(i)];
		for (unsigned short j = 0; j < 12; ++j) {
			const mt::vec3& edge2 = fedges2[j];
			// Normal of the separation axis.
			const mt::vec3 normal = mt::cross(edge2, edge1);

			const int side1 = whichSide(fbox1, normal, vert);

			// Intersect ?
			if (side1 == 0) {
				continue;
			}

			const int side2 = whichSide(fbox2, normal, vert);

			// Intersect ?
			if (side2 == 0) {
				continue;
			}

			// Frustum on opposite side of the separation axis.
			if ((side1 * side2) < 0) {
				return OUTSIDE;
			}
		}
	}

	return INSIDE;
}

#include "SG_Frustum.h"

#include "MT_Frustum.h"

SG_Frustum::SG_Frustum(const MT_Matrix4x4& matrix)
	:m_matrix(matrix)
{
	// Left clip plane
	m_planes[0] = m_matrix[3] + m_matrix[0];
	// Right clip plane
	m_planes[1] = m_matrix[3] - m_matrix[0];
	// Top clip plane
	m_planes[2] = m_matrix[3] - m_matrix[1];
	// Bottom clip plane
	m_planes[3] = m_matrix[3] + m_matrix[1];
	// Near clip plane
	m_planes[4] = m_matrix[3] + m_matrix[2];
	// Far clip plane
	m_planes[5] = m_matrix[3] - m_matrix[2];

	// Normalize clip planes.
	for (MT_Vector4& plane : m_planes) {
		const float factor = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
		if (!MT_fuzzyZero(factor)) {
			plane /= factor;
		}
	}
}

const std::array<MT_Vector4, 6>& SG_Frustum::GetPlanes() const
{
	return m_planes;
}

SG_Frustum::TestType SG_Frustum::PointInsideFrustum(const MT_Vector3& point) const
{
	for (const MT_Vector4& plane : m_planes) {
		if (plane.dot(point) < 0.0f) {
			return OUTSIDE;
		}
	}

	return INSIDE;
}

SG_Frustum::TestType SG_Frustum::SphereInsideFrustum(const MT_Vector3& center, float radius) const
{
	for (const MT_Vector4& plane : m_planes) {
		const float distance = plane.dot(center);
		if (distance < -radius) {
			return OUTSIDE;
		}
		else if (fabs(distance) <= radius) {
			return INTERSECT;
		}
	}

	return INSIDE;
}

SG_Frustum::TestType SG_Frustum::BoxInsideFrustum(const std::array<MT_Vector3, 8>& box) const
{
	unsigned short insidePlane = 0;
	for (const MT_Vector4& plane : m_planes) {
		unsigned short insidePoint = 0;
		for (const MT_Vector3& point : box) {
			insidePoint += (plane.dot(point) < 0.0f) ? 0 : 1;
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

static void getNearFarAabbPoint(const MT_Vector4& plane, const MT_Vector3& min, const MT_Vector3& max, MT_Vector3& near, MT_Vector3& far)
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

static bool aabbIntersect(const MT_Vector3& min1, const MT_Vector3& max1, const MT_Vector3& min2, const MT_Vector3& max2)
{
	for (unsigned short axis = 0; axis < 3; ++axis) {
		if (max1[axis] < min2[axis] || min1[axis] > max2[axis]) {
			return false;
		}
	}

	return true;
}

SG_Frustum::TestType SG_Frustum::AabbInsideFrustum(const MT_Vector3& min, const MT_Vector3& max, const MT_Matrix4x4& mat) const
{
	TestType result = INSIDE;

	for (const MT_Vector4& wplane : m_planes) {
		// Compute frustum plane in object space.
		const MT_Vector4 oplane = wplane * mat;

		// Test near and far AABB vertices.
		MT_Vector3 near;
		MT_Vector3 far;

		// Generate nearest and further points from the positive plane side.
		getNearFarAabbPoint(oplane, min, max, near, far);

		// If the near point to the plane is out, all the other points are out.
		if (oplane.dot(far) < 0.0f) {
			return OUTSIDE;
		}
		// If the far plane is out, the AABB is intersecting this plane.
		else if (result != INTERSECT && oplane.dot(near) < 0.0f) {
			result = INTERSECT;
		}
	}

	/* Big object can intersect two "orthogonal" planes without be inside the frustum.
	 * In this case the object is outside the AABB of the frustum. */
	if (result == INTERSECT) {
		MT_Vector3 fmin;
		MT_Vector3 fmax;
		MT_FrustumAabb((m_matrix * mat).inverse(), fmin, fmax);

		if (!aabbIntersect(min, max, fmin, fmax)) {
			return OUTSIDE;
		}
	}

	return result;
}

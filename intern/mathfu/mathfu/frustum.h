#ifndef MATHFU_FRUSTUM_H_
#define MATHFU_FRUSTUM_H_

#include "mathfu/matrix.h"
#include "mathfu/vector.h"
#include <array>

namespace mathfu {

static const Vector<float, 3> normalizedBox[8] = {
	Vector<float, 3>(-1.0f, -1.0f, -1.0f),
	Vector<float, 3>(-1.0f, 1.0f, -1.0f),
	Vector<float, 3>(1.0f, 1.0f, -1.0f),
	Vector<float, 3>(1.0f, -1.0f, -1.0f),
	Vector<float, 3>(-1.0f, -1.0f, 1.0f),
	Vector<float, 3>(-1.0f, 1.0f, 1.0f),
	Vector<float, 3>(1.0f, 1.0f, 1.0f),
	Vector<float, 3>(1.0f, -1.0f, 1.0f)
};

static const unsigned short edgeIndices[12][2] = {
	{0, 1},
	{1, 2},
	{2, 3},
	{3, 0},
	{4, 5},
	{5, 6},
	{6, 7},
	{7, 0},
	{0, 4},
	{1, 5},
	{2, 6},
	{3, 7}
};

inline void FrustumBox(const Matrix<float, 4>& m, std::array<Vector<float, 3>, 8>& box)
{
	for (unsigned short i = 0; i < 8; ++i) {
		box[i] = m * normalizedBox[i];
	}
}

inline void FrustumAabb(const Matrix<float, 4>& m, Vector<float, 3>& min, Vector<float, 3>& max)
{
	for (unsigned short i = 0; i < 8; ++i) {
		const Vector<float, 3> co = m * normalizedBox[i];

		if (i == 0) {
			min = max = co;
		}
		else {
			min = Vector<float, 3>::Min(min, co);
			max = Vector<float, 3>::Max(max, co);
		}
	}
}

inline void FrustumEdges(std::array<Vector<float, 3>, 8>& box, std::array<Vector<float, 3>, 12>& edges)
{
	for (unsigned short i = 0; i < 12; ++i) {
		const Vector<float, 3>& p1 = box[edgeIndices[i][0]];
		const Vector<float, 3>& p2 = box[edgeIndices[i][1]];

		edges[i] = (p2 - p1).Normalized();
	}
}

inline unsigned short FrustumEdgeVertex(unsigned short edge)
{
	return edgeIndices[edge][0];
}

}

#endif  // MATHFU_FRUSTUM_H_

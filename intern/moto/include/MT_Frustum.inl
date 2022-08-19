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

/** \file moto/include/MT_Frustum.inl
 *  \ingroup moto
 */

#include "MT_Optimize.h"

static const MT_Vector3 normalizedBox[8] = {
	MT_Vector3(-1.0f, -1.0f, -1.0f),
	MT_Vector3(-1.0f, 1.0f, -1.0f),
	MT_Vector3(1.0f, 1.0f, -1.0f),
	MT_Vector3(1.0f, -1.0f, -1.0f),
	MT_Vector3(-1.0f, -1.0f, 1.0f),
	MT_Vector3(-1.0f, 1.0f, 1.0f),
	MT_Vector3(1.0f, 1.0f, 1.0f),
	MT_Vector3(1.0f, -1.0f, 1.0f)
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

GEN_INLINE void MT_FrustumBox(const MT_Matrix4x4& mat, std::array<MT_Vector3, 8>& box)
{
	for (unsigned short i = 0; i < 8; ++i) {
		const MT_Vector3& p3 = normalizedBox[i];
		const MT_Vector4 p4 = mat * MT_Vector4(p3.x(), p3.y(), p3.z(), 1.0f);

		box[i] = p4.to3d() / p4.w();
	}
}

GEN_INLINE void MT_FrustumAabb(const MT_Matrix4x4& mat, MT_Vector3& min, MT_Vector3& max)
{
	for (unsigned short i = 0; i < 8; ++i) {
		const MT_Vector3& p3 = normalizedBox[i];
		const MT_Vector4 p4 = mat * MT_Vector4(p3.x(), p3.y(), p3.z(), 1.0f);
		const MT_Vector3 co = p4.to3d() / p4.w();

		if (i == 0) {
			min = max = co;
		}
		else {
			for (unsigned short axis = 0; axis < 3; ++axis) {
				min[axis] = std::min(min[axis], co[axis]);
				max[axis] = std::max(max[axis], co[axis]);
			}
		}
	}
}

GEN_INLINE void MT_FrustumEdges(std::array<MT_Vector3, 8>& box, std::array<MT_Vector3, 12>& edges)
{
	for (unsigned short i = 0; i < 12; ++i) {
		const MT_Vector3& p1 = box[edgeIndices[i][0]];
		const MT_Vector3& p2 = box[edgeIndices[i][1]];

		edges[i] = (p2 - p1).normalized();
	}
}

GEN_INLINE unsigned short MT_FrustumEdgeVertex(unsigned short edge)
{
	return edgeIndices[edge][0];
}

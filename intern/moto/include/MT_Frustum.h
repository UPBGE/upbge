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

/** \file moto/include/MT_Frustum.h
 *  \ingroup moto
 */

#ifndef __MT_FRUSTUM_H__
#define __MT_FRUSTUM_H__

#include "MT_Config.h"
#include "MT_Matrix4x4.h"

#include <array>

void MT_FrustumBox(const MT_Matrix4x4& mat, std::array<MT_Vector3, 8>& box);
void MT_FrustumAabb(const MT_Matrix4x4& mat, MT_Vector3& min, MT_Vector3& max);
void MT_FrustumEdges(std::array<MT_Vector3, 8>& box, std::array<MT_Vector3, 12>& edges);
unsigned short MT_FrustumEdgeVertex(unsigned short edge);

#ifdef GEN_INLINED
#  include "MT_Frustum.inl"
#endif

#endif  // __MT_FRUSTUM_H__

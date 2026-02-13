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
 * Contributor(s): UPBGE Contributors
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file JoltMathUtils.h
 *  \ingroup physjolt
 *  \brief Conversion functions between Blender/UPBGE math types and Jolt Physics types.
 *
 *  Blender uses Z-up coordinate system; Jolt uses Y-up.
 *  All conversions between the two coordinate systems are centralized here.
 */

#pragma once

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Real.h>

#include "MT_Matrix3x3.h"
#include "MT_Matrix4x4.h"
#include "MT_Quaternion.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

namespace JoltMath {

/* -------------------------------------------------------------------- */
/** \name Blender (Z-up) → Jolt (Y-up) conversions
 * \{ */

/** Convert a Blender Z-up vector to Jolt Y-up vector.
 *  Mapping: (X, Y, Z)_blender → (X, Z, -Y)_jolt */
inline JPH::Vec3 ToJolt(const MT_Vector3 &v)
{
  return JPH::Vec3(float(v.x()), float(v.z()), float(-v.y()));
}

/** Convert a Blender Z-up position to Jolt Y-up RVec3 (for positions).
 *  Mapping: (X, Y, Z)_blender → (X, Z, -Y)_jolt */
inline JPH::RVec3 ToJoltR(const MT_Vector3 &v)
{
  return JPH::RVec3(JPH::Real(v.x()), JPH::Real(v.z()), JPH::Real(-v.y()));
}

/** Convert a Blender quaternion (W, X, Y, Z) to Jolt quaternion with coordinate swap.
 *  Mapping: (X, Y, Z, W)_blender → (X, Z, -Y, W)_jolt */
inline JPH::Quat ToJolt(const MT_Quaternion &q)
{
  return JPH::Quat(float(q.x()), float(q.z()), float(-q.y()), float(q.w()));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Jolt (Y-up) → Blender (Z-up) conversions
 * \{ */

/** Convert a Jolt Y-up Vec3 to Blender Z-up vector.
 *  Mapping: (X, Y, Z)_jolt → (X, -Z, Y)_blender */
inline MT_Vector3 ToMT(const JPH::Vec3 &v)
{
  return MT_Vector3(v.GetX(), -v.GetZ(), v.GetY());
}

/** Convert a Jolt Y-up RVec3 to Blender Z-up vector (for positions).
 *  Only defined when JPH_DOUBLE_PRECISION is set, otherwise RVec3 == Vec3. */
#ifdef JPH_DOUBLE_PRECISION
inline MT_Vector3 ToMT(const JPH::RVec3 &v)
{
  return MT_Vector3(float(v.GetX()), float(-v.GetZ()), float(v.GetY()));
}
#endif

/** Convert a Jolt quaternion to Blender quaternion with coordinate swap.
 *  Mapping: (X, Y, Z, W)_jolt → (X, -Z, Y, W)_blender */
inline MT_Quaternion ToMT(const JPH::Quat &q)
{
  return MT_Quaternion(q.GetX(), -q.GetZ(), q.GetY(), q.GetW());
}

/** Convert a Jolt Vec4 color to Blender MT_Vector4. No coordinate swap needed for colors. */
inline MT_Vector4 ToMTColor(const JPH::Vec4 &v)
{
  return MT_Vector4(v.GetX(), v.GetY(), v.GetZ(), v.GetW());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scalar conversions
 * \{ */

/** Convert 3 float components (Blender Z-up) to a Jolt Y-up Vec3. */
inline JPH::Vec3 ToJolt(float x, float y, float z)
{
  return JPH::Vec3(x, z, -y);
}

/** \} */

}  // namespace JoltMath

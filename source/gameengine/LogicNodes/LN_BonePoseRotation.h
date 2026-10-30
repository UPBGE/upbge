/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_BonePoseRotation.h
 *  \ingroup logicnodes
 *
 * Game-runtime-only rotation conversions for Blender bone orientations.
 *
 * Bone-facing Logic Nodes use Blender's native bone basis. The explicit
 * Bone Rotation to Object Rotation node uses the fixed conversion in this file when an ordinary
 * object's +Z axis should point along the bone instead.
 */

#pragma once

#include "LN_Rotation.h"

/**
 * Blender's native bone frame uses +Y along the bone. The optional object-style conversion maps
 * that frame to an ordinary UPBGE object with +Z along the bone:
 *
 *   object +X = Blender bone +X
 *   object +Y = Blender bone -Z
 *   object +Z = Blender bone +Y
 *
 * This is a fixed game-runtime conversion. It neither changes Blender's bone data nor removes a
 * bone's authored direction or roll.
 */
inline const LN_Rotation &LN_BlenderBoneBasisRotation()
{
  static const LN_Rotation rotation = LN_Rotation::FromMatrix(
      MT_Matrix3x3(1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, -1.0f,
                   0.0f, 1.0f, 0.0f));
  return rotation;
}

/**
 * Blender's float matrices and MT_Matrix3x3 use opposite row/column storage conventions.
 * This is the same storage conversion used for normal objects by BL_DataConversion. It preserves
 * Blender's native bone orientation exactly; it does not remap the bone basis to object axes.
 */
inline MT_Matrix3x3 LN_BlenderRotationMatrixToGame(const float matrix[3][3])
{
  MT_Matrix3x3 result;
  result.setValue3x3(&matrix[0][0]);
  return result;
}

inline void LN_GameRotationMatrixToBlender(const MT_Matrix3x3 &matrix,
                                           float r_matrix[3][3])
{
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      r_matrix[row][column] = float(matrix[column][row]);
    }
  }
}

/**
 * Extract the closest proper orientation from a Blender matrix.
 *
 * Evaluated pose matrices can contain inherited non-uniform scale and shear. Passing the complete
 * linear transform to LN_Rotation lets its polar decomposition remove both symmetrically. Do not
 * normalize the three columns first: doing so biases the result toward whichever axes happened to
 * carry the largest inherited scale.
 */
inline LN_Rotation LN_BlenderMatrixToRotation(const float matrix[3][3])
{
  return LN_Rotation::FromMatrix(LN_BlenderRotationMatrixToGame(matrix));
}

inline LN_Rotation LN_BlenderPoseMatrixToRotation(const float matrix[4][4])
{
  float linear[3][3];
  for (int column = 0; column < 3; column++) {
    for (int row = 0; row < 3; row++) {
      linear[column][row] = matrix[column][row];
    }
  }
  return LN_BlenderMatrixToRotation(linear);
}

inline LN_Rotation LN_BoneRotationToObjectRotation(const LN_Rotation &bone_orientation)
{
  const MT_Quaternion &basis = LN_BlenderBoneBasisRotation().Quaternion();
  return LN_Rotation(bone_orientation.Quaternion() * basis.conjugate());
}

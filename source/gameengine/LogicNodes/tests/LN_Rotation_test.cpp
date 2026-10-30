/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cmath>
#include <limits>
#include <type_traits>

#include "LN_BonePoseRotation.h"
#include "LN_Rotation.h"

namespace {

constexpr float kRotationTolerance = 1.0e-5f;

void ExpectSameOrientation(const LN_Rotation &actual,
                           const LN_Rotation &expected,
                           const float tolerance = kRotationTolerance)
{
  EXPECT_LE(actual.AngleTo(expected), tolerance);
}

}  // namespace

TEST(LN_Rotation, IsCompactAlignedAndIdentityByDefault)
{
  EXPECT_EQ(sizeof(LN_Rotation), sizeof(MT_Quaternion));
  EXPECT_EQ(alignof(LN_Rotation), 16u);
  EXPECT_TRUE(std::is_trivially_copyable_v<LN_Rotation>);
  EXPECT_TRUE(std::is_standard_layout_v<LN_Rotation>);
  EXPECT_TRUE(LN_Rotation{}.IsIdentity());
}

TEST(LN_Rotation, EulerXYZRoundTripPreservesOrientation)
{
  const LN_Rotation rotation = LN_Rotation::FromEulerXYZ(MT_Vector3(0.7f, -1.1f, 2.4f));
  ExpectSameOrientation(LN_Rotation::FromEulerXYZ(rotation.ToEulerXYZ()), rotation);
}

TEST(LN_Rotation, QuaternionNormalizationAndDoubleCoverAreHandled)
{
  const MT_Quaternion quaternion(MT_Vector3(0.2f, 0.7f, -0.4f).normalized(), 1.4f);
  const LN_Rotation positive(MT_Quaternion(quaternion.x() * 8.0f,
                                           quaternion.y() * 8.0f,
                                           quaternion.z() * 8.0f,
                                           quaternion.w() * 8.0f));
  const LN_Rotation negative(MT_Quaternion(quaternion.x() * -3.0f,
                                           quaternion.y() * -3.0f,
                                           quaternion.z() * -3.0f,
                                           quaternion.w() * -3.0f));
  ExpectSameOrientation(positive, negative);
  EXPECT_NEAR(positive.Quaternion().length2(), 1.0f, 1.0e-6f);
  EXPECT_NEAR(negative.Quaternion().length2(), 1.0f, 1.0e-6f);
}

TEST(LN_Rotation, AngleToResolvesSubMilliradianDifferences)
{
  const LN_Rotation identity;
  const LN_Rotation small_rotation = LN_Rotation::FromEulerXYZ(
      MT_Vector3(2.0e-4f, 0.0f, 0.0f));

  EXPECT_NEAR(identity.AngleTo(small_rotation), 2.0e-4f, 1.0e-7f);
  EXPECT_FALSE(identity.NearlyEquals(small_rotation, 1.0e-4f));
  EXPECT_TRUE(identity.NearlyEquals(small_rotation, 3.0e-4f));

  const MT_Quaternion &quaternion = small_rotation.Quaternion();
  const LN_Rotation negative(MT_Quaternion(-quaternion.x(),
                                           -quaternion.y(),
                                           -quaternion.z(),
                                           -quaternion.w()));
  EXPECT_NEAR(identity.AngleTo(negative), 2.0e-4f, 1.0e-7f);
}

TEST(LN_Rotation, InvalidAndSingularInputsFailToIdentity)
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_TRUE(LN_Rotation(MT_Quaternion(nan, 0.0f, 0.0f, 1.0f)).IsIdentity());
  EXPECT_TRUE(LN_Rotation(MT_Quaternion(0.0f, 0.0f, 0.0f, 0.0f)).IsIdentity());
  EXPECT_TRUE(LN_Rotation::FromEulerXYZ(MT_Vector3(nan, 0.0f, 0.0f)).IsIdentity());
  EXPECT_TRUE(LN_Rotation::FromMatrix(
                  MT_Matrix3x3(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f))
                  .IsIdentity());
}

TEST(LN_Rotation, MatrixExtractionRemovesScaleAndShearOnEitherSide)
{
  const LN_Rotation expected = LN_Rotation::FromEulerXYZ(MT_Vector3(0.35f, -0.8f, 1.2f));
  MT_Matrix3x3 column_contaminated = expected.ToMatrix().scaled(2.0f, 0.5f, 3.0f);
  column_contaminated.setColumn(
      1, column_contaminated.getColumn(1) + column_contaminated.getColumn(0) * 1.0e-4f);
  ExpectSameOrientation(
      LN_Rotation::FromMatrix(column_contaminated), expected, 2.0e-4f);

  MT_Matrix3x3 row_contaminated = expected.ToMatrix();
  row_contaminated[0] *= 2.0f;
  row_contaminated[1] *= 0.5f;
  row_contaminated[1] += row_contaminated[0] * 1.0e-4f;
  row_contaminated[2] *= 3.0f;
  ExpectSameOrientation(LN_Rotation::FromMatrix(row_contaminated), expected, 2.0e-4f);
}

TEST(LN_Rotation, ReflectedMatrixProducesAProperFiniteRotation)
{
  const MT_Matrix3x3 reflected(-2.0f, 0.0f, 0.0f,
                               0.0f, 3.0f, 0.0f,
                               0.0f, 0.0f, 4.0f);
  const LN_Rotation rotation = LN_Rotation::FromMatrix(reflected);
  const MT_Matrix3x3 proper = rotation.ToMatrix();
  EXPECT_TRUE(std::isfinite(float(proper.determinant())));
  EXPECT_NEAR(float(proper.determinant()), 1.0f, 1.0e-5f);
}

TEST(LN_Rotation, SlerpUsesShortestQuaternionArc)
{
  constexpr float degrees_to_radians = float(M_PI / 180.0);
  const LN_Rotation from = LN_Rotation::FromEulerXYZ(
      MT_Vector3(0.0f, 0.0f, 170.0f * degrees_to_radians));
  const LN_Rotation to = LN_Rotation::FromEulerXYZ(
      MT_Vector3(0.0f, 0.0f, -170.0f * degrees_to_radians));
  const LN_Rotation expected = LN_Rotation::FromEulerXYZ(
      MT_Vector3(0.0f, 0.0f, float(M_PI)));
  ExpectSameOrientation(from.Slerp(to, 0.5f), expected);
}

TEST(LN_Rotation, GimbalLockAndNonFiniteInterpolationRemainValid)
{
  constexpr float half_pi = float(M_PI * 0.5);
  const LN_Rotation gimbal = LN_Rotation::FromEulerXYZ(
      MT_Vector3(0.4f, half_pi, -1.2f));
  ExpectSameOrientation(LN_Rotation::FromEulerXYZ(gimbal.ToEulerXYZ()), gimbal, 2.0e-4f);

  const LN_Rotation target = LN_Rotation::FromEulerXYZ(MT_Vector3(-1.0f, 0.2f, 2.0f));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  ExpectSameOrientation(gimbal.Slerp(target, nan), gimbal);
  ExpectSameOrientation(gimbal.Slerp(LN_Rotation(MT_Quaternion(-gimbal.Quaternion().x(),
                                                               -gimbal.Quaternion().y(),
                                                               -gimbal.Quaternion().z(),
                                                               -gimbal.Quaternion().w())),
                                    0.5f),
                        gimbal);
}

TEST(LN_Rotation, CanonicalizationIsStableAtHalfTurns)
{
  const LN_Rotation positive(MT_Quaternion(1.0f, 0.0f, 0.0f, 0.0f));
  const LN_Rotation negative(MT_Quaternion(-1.0f, 0.0f, 0.0f, 0.0f));
  const LN_Rotation canonical_positive = positive.Canonicalized();
  const LN_Rotation canonical_negative = negative.Canonicalized();
  const MT_Quaternion &a = canonical_positive.Quaternion();
  const MT_Quaternion &b = canonical_negative.Quaternion();
  EXPECT_EQ(a.x(), b.x());
  EXPECT_EQ(a.y(), b.y());
  EXPECT_EQ(a.z(), b.z());
  EXPECT_EQ(a.w(), b.w());
}

TEST(LN_Rotation, NativeDefaultBlenderBoneBasisIsPreservedExactly)
{
  const float default_raw_blender_bone_basis[3][3] = {
      {1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f},
      {0.0f, -1.0f, 0.0f},
  };
  const MT_Matrix3x3 default_native_game_bone_basis =
      LN_BlenderRotationMatrixToGame(default_raw_blender_bone_basis);
  const LN_Rotation native_orientation = LN_Rotation::FromMatrix(
      default_native_game_bone_basis);
  ExpectSameOrientation(native_orientation, LN_BlenderBoneBasisRotation(), 1.0e-6f);
  EXPECT_FALSE(native_orientation.IsIdentity(1.0e-6f));

  float recovered_raw_blender_basis[3][3];
  LN_GameRotationMatrixToBlender(default_native_game_bone_basis,
                                 recovered_raw_blender_basis);
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      EXPECT_FLOAT_EQ(recovered_raw_blender_basis[row][column],
                      default_raw_blender_bone_basis[row][column]);
    }
  }
}

TEST(LN_Rotation, BoneRotationToObjectRotationMakesDefaultBoneIdentity)
{
  const LN_Rotation object_orientation = LN_BoneRotationToObjectRotation(
      LN_BlenderBoneBasisRotation());
  EXPECT_TRUE(object_orientation.IsIdentity(1.0e-6f));
}

TEST(LN_Rotation, BoneRotationToObjectRotationPreservesDocumentedAxisMapping)
{
  const LN_Rotation native_orientation = LN_Rotation::FromEulerXYZ(
      MT_Vector3(0.35f, -0.6f, 1.1f));
  const LN_Rotation object_orientation = LN_BoneRotationToObjectRotation(
      native_orientation);
  const MT_Matrix3x3 bone_matrix = native_orientation.ToMatrix();
  const MT_Matrix3x3 object_matrix = object_orientation.ToMatrix();
  const MT_Vector3 object_x = object_matrix.getColumn(0);
  const MT_Vector3 object_y = object_matrix.getColumn(1);
  const MT_Vector3 object_z = object_matrix.getColumn(2);
  EXPECT_LE((object_x - bone_matrix.getColumn(0)).length(), 1.0e-6f);
  EXPECT_LE((object_y + bone_matrix.getColumn(2)).length(), 1.0e-6f);
  EXPECT_LE((object_z - bone_matrix.getColumn(1)).length(), 1.0e-6f);
}

TEST(LN_Rotation, BoneRotationToObjectRotationComposesCorrectlyInWorldSpace)
{
  const LN_Rotation armature_world = LN_Rotation::FromEulerXYZ(
      MT_Vector3(-0.4f, 0.75f, 1.25f));
  const LN_Rotation native_bone_in_armature = LN_Rotation::FromEulerXYZ(
      MT_Vector3(0.3f, -1.1f, 0.55f));
  const LN_Rotation native_bone_in_world(
      armature_world.Quaternion() * native_bone_in_armature.Quaternion());
  const LN_Rotation converted_in_world = LN_BoneRotationToObjectRotation(
      native_bone_in_world);
  const LN_Rotation converted_in_armature = LN_BoneRotationToObjectRotation(
      native_bone_in_armature);
  const LN_Rotation expected_in_world(
      armature_world.Quaternion() * converted_in_armature.Quaternion());
  ExpectSameOrientation(converted_in_world, expected_in_world, 1.0e-6f);
}

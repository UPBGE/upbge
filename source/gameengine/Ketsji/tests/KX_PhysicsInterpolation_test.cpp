/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <limits>

#include "KX_GameObject.h"
#include "KX_KetsjiEngine.h"
#include "KX_NodeRelationships.h"
#include "SG_Node.h"

namespace {

using blender::OB_SMOOTH_MOTION_EXTRAPOLATE;
using blender::OB_SMOOTH_MOTION_INTERPOLATE;
using blender::OB_SMOOTH_MOTION_NONE;
using blender::OB_SMOOTH_MOTION_SCENE;

class InterpolationObject {
 public:
  InterpolationObject()
  {
    node = new SG_Node(&object, nullptr, callbacks);
    object.SetSGNode(node);
  }

  ~InterpolationObject()
  {
    object.SetSGNode(nullptr);
    delete node;
  }

  void SetWorldPose(const MT_Vector3 &position,
                    const MT_Matrix3x3 &orientation,
                    const MT_Vector3 &scale)
  {
    node->SetWorldPosition(position);
    node->SetWorldOrientation(orientation);
    node->SetWorldScale(scale);
  }

  SG_Callbacks callbacks;
  KX_GameObject object;
  SG_Node *node = nullptr;
};

void ExpectVectorNear(const MT_Vector3 &actual, const MT_Vector3 &expected)
{
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(actual[axis], expected[axis], 1.0e-5f);
  }
}

void ExpectMatrixNear(const MT_Matrix3x3 &actual, const MT_Matrix3x3 &expected)
{
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      EXPECT_NEAR(actual[row][column], expected[row][column], 1.0e-5f);
    }
  }
}

TEST(KX_PhysicsInterpolation, InterpolatesAndRestoresExactSignedPose)
{
  InterpolationObject fixture;
  const MT_Vector3 previousPosition(0.0f, 0.0f, 0.0f);
  const MT_Matrix3x3 previousOrientation = MT_Matrix3x3::Identity();
  const MT_Vector3 previousScale(-2.0f, 3.0f, -4.0f);
  fixture.SetWorldPose(previousPosition, previousOrientation, previousScale);
  fixture.object.StorePhysicsInterpolationState();

  const MT_Vector3 currentPosition(8.0f, -4.0f, 2.0f);
  const MT_Matrix3x3 currentOrientation(MT_Vector3(0.0f, 0.0f, 1.0f));
  const MT_Vector3 currentScale(-4.0f, 5.0f, -6.0f);
  fixture.SetWorldPose(currentPosition, currentOrientation, currentScale);

  fixture.object.ApplyPhysicsInterpolation(0.25);

  ExpectVectorNear(fixture.node->GetWorldPosition(), MT_Vector3(2.0f, -1.0f, 0.5f));
  ExpectVectorNear(fixture.node->GetWorldScaling(), MT_Vector3(-2.5f, 3.5f, -4.5f));
  const MT_Matrix3x3 expectedOrientation(
      previousOrientation.getRotation().slerp(currentOrientation.getRotation(), 0.25f));
  ExpectMatrixNear(fixture.node->GetWorldOrientation(), expectedOrientation);

  fixture.object.ClearPhysicsInterpolationState();
  ExpectVectorNear(fixture.node->GetWorldPosition(), currentPosition);
  ExpectMatrixNear(fixture.node->GetWorldOrientation(), currentOrientation);
  ExpectVectorNear(fixture.node->GetWorldScaling(), currentScale);
}

TEST(KX_PhysicsInterpolation, RebaseMakesRenderFrameTransformImmediate)
{
  InterpolationObject fixture;
  fixture.SetWorldPose(
      MT_Vector3(0.0f, 0.0f, 0.0f), MT_Matrix3x3::Identity(), MT_Vector3(1.0f, 1.0f, 1.0f));
  fixture.object.StorePhysicsInterpolationState();

  const MT_Vector3 renderFramePosition(3.0f, 4.0f, 5.0f);
  const MT_Matrix3x3 renderFrameOrientation(MT_Vector3(0.2f, -0.4f, 0.7f));
  const MT_Vector3 renderFrameScale(-1.0f, 2.0f, 0.5f);
  fixture.SetWorldPose(renderFramePosition, renderFrameOrientation, renderFrameScale);
  fixture.object.RebasePhysicsInterpolationState();

  fixture.object.ApplyPhysicsInterpolation(0.1);
  ExpectVectorNear(fixture.node->GetWorldPosition(), renderFramePosition);
  ExpectMatrixNear(fixture.node->GetWorldOrientation(), renderFrameOrientation);
  ExpectVectorNear(fixture.node->GetWorldScaling(), renderFrameScale);
}

TEST(KX_PhysicsInterpolation, InvalidAlphaFallsBackToCurrentPose)
{
  InterpolationObject fixture;
  fixture.SetWorldPose(
      MT_Vector3(-1.0f, -2.0f, -3.0f), MT_Matrix3x3::Identity(), MT_Vector3(1.0f, 1.0f, 1.0f));
  fixture.object.StorePhysicsInterpolationState();

  const MT_Vector3 currentPosition(6.0f, 7.0f, 8.0f);
  const MT_Matrix3x3 currentOrientation(MT_Vector3(0.3f, 0.4f, 0.5f));
  const MT_Vector3 currentScale(-2.0f, 3.0f, 4.0f);
  fixture.SetWorldPose(currentPosition, currentOrientation, currentScale);

  fixture.object.ApplyPhysicsInterpolation(std::numeric_limits<double>::quiet_NaN());
  ExpectVectorNear(fixture.node->GetWorldPosition(), currentPosition);
  ExpectMatrixNear(fixture.node->GetWorldOrientation(), currentOrientation);
  ExpectVectorNear(fixture.node->GetWorldScaling(), currentScale);
}

KX_GameObject::SmoothMotionFrame Frame(const double interpolation,
                                       const uint64_t number,
                                       const double seconds = 1.0 / 60.0)
{
  KX_GameObject::SmoothMotionFrame frame;
  frame.interpolation = interpolation;
  frame.number = number;
  frame.seconds = seconds;
  return frame;
}

MT_Vector3 At(const float x, const float y = 0.0f, const float z = 0.0f)
{
  return MT_Vector3(x, y, z);
}

const MT_Vector3 kUnitScale(1.0f, 1.0f, 1.0f);

/** Turned about Z. */
MT_Matrix3x3 Yaw(const float radians)
{
  return MT_Matrix3x3(MT_Vector3(0.0f, 0.0f, radians));
}

/* Extrapolate: ahead of the last step by the share of a step since it, along the step's move. */
TEST(KX_SmoothMotion, ExtrapolateDrawsAheadOfTheLastStep)
{
  InterpolationObject fixture;
  fixture.SetWorldPose(At(0.0f), Yaw(0.0f), kUnitScale);
  fixture.object.StoreSmoothMotionHistory(true);
  fixture.SetWorldPose(At(2.0f), Yaw(0.2f), kUnitScale);

  fixture.object.ApplySmoothMotion(Frame(0.5, 1),
                                   OB_SMOOTH_MOTION_EXTRAPOLATE,
                                   KX_GameObject::SmoothMotionFollow(),
                                   KX_GameObject::SmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(3.0f));
  ExpectMatrixNear(fixture.node->GetWorldOrientation(), Yaw(0.3f));

  fixture.object.ClearPhysicsInterpolationState();
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(2.0f));
}

/* An object that did not move in the last step is left alone: no work, nothing to restore. */
TEST(KX_SmoothMotion, AStillObjectIsNotWritten)
{
  InterpolationObject fixture;
  fixture.SetWorldPose(At(5.0f), Yaw(0.4f), kUnitScale);
  fixture.object.StoreSmoothMotionHistory(true);

  const KX_GameObject::SmoothMotionFollow follow = fixture.object.ApplySmoothMotion(
      Frame(0.3, 1),
      OB_SMOOTH_MOTION_INTERPOLATE,
      KX_GameObject::SmoothMotionFollow(),
      KX_GameObject::SmoothMotionReset(),
      nullptr);
  EXPECT_FALSE(follow.moved);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(5.0f));
  /* Nothing was written for drawing. */
  ExpectVectorNear(fixture.object.GetCachedInterpolatedTransform().getOrigin(), At(0.0f));
}

/* Only smoothed objects keep a step history, so nothing old is smoothed when one starts. */
TEST(KX_SmoothMotion, OnlySmoothedObjectsKeepAStepHistory)
{
  InterpolationObject fixture;
  fixture.object.StoreSmoothMotionHistory(true);
  EXPECT_TRUE(fixture.node->HasPreviousWorldTransform());
  fixture.object.StoreSmoothMotionHistory(false);
  EXPECT_FALSE(fixture.node->HasPreviousWorldTransform());
}

TEST(KX_SmoothMotion, SceneDefaultIsTheParentsChoice)
{
  InterpolationObject fixture;
  EXPECT_EQ(fixture.object.GetSmoothMotion(), OB_SMOOTH_MOTION_SCENE);
  EXPECT_EQ(fixture.object.ResolveSmoothMotion(OB_SMOOTH_MOTION_EXTRAPOLATE),
            OB_SMOOTH_MOTION_EXTRAPOLATE);
  fixture.object.SetSmoothMotion(OB_SMOOTH_MOTION_NONE);
  EXPECT_EQ(fixture.object.ResolveSmoothMotion(OB_SMOOTH_MOTION_INTERPOLATE),
            OB_SMOOTH_MOTION_NONE);
}

/* A child left out of smoothing (None) is drawn with its smoothed parent, turning around it, so
 * what is attached never shakes against what carries it. */
TEST(KX_SmoothMotion, ANoneChildIsDrawnWithItsSmoothedParent)
{
  InterpolationObject parent;
  InterpolationObject child;
  parent.node->AddChild(child.node);

  parent.SetWorldPose(At(0.0f), Yaw(0.0f), kUnitScale);
  parent.object.StoreSmoothMotionHistory(true);
  child.object.StoreSmoothMotionHistory(false);
  /* The step moved the parent 2 along X and turned it 0.4; the child sits 1 along its X. */
  parent.SetWorldPose(At(2.0f), Yaw(0.4f), kUnitScale);
  child.SetWorldPose(At(2.0f) + Yaw(0.4f) * At(1.0f), Yaw(0.4f), kUnitScale);

  const KX_GameObject::SmoothMotionFollow follow = parent.object.ApplySmoothMotion(
      Frame(0.5, 1),
      OB_SMOOTH_MOTION_INTERPOLATE,
      KX_GameObject::SmoothMotionFollow(),
      KX_GameObject::SmoothMotionReset(),
      nullptr);
  ASSERT_TRUE(follow.moved);
  child.object.ApplySmoothMotion(
      Frame(0.5, 1), OB_SMOOTH_MOTION_NONE, follow, KX_GameObject::SmoothMotionReset(), nullptr);

  ExpectVectorNear(parent.node->GetWorldPosition(), At(1.0f));
  ExpectVectorNear(child.node->GetWorldPosition(), At(1.0f) + Yaw(0.2f) * At(1.0f));
  ExpectMatrixNear(child.node->GetWorldOrientation(), Yaw(0.2f));

  child.object.ClearPhysicsInterpolationState();
  ExpectVectorNear(child.node->GetWorldPosition(), At(2.0f) + Yaw(0.4f) * At(1.0f));
  parent.node->RemoveChild(child.node);
}

/* A teleport (ResetSmoothing) is drawn at its end at once, and stays there until the next step:
 * never a slide across the map. */
TEST(KX_SmoothMotion, ATeleportIsDrawnAtOnceUntilTheNextStep)
{
  InterpolationObject fixture;
  fixture.SetWorldPose(At(0.0f), Yaw(0.0f), kUnitScale);
  fixture.object.StoreSmoothMotionHistory(true);
  fixture.SetWorldPose(At(1.0f), Yaw(0.0f), kUnitScale);
  fixture.object.ApplySmoothMotion(Frame(0.5, 1),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   KX_GameObject::SmoothMotionFollow(),
                                   KX_GameObject::SmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(0.5f));
  fixture.object.ClearPhysicsInterpolationState();

  /* A step teleports it. */
  fixture.object.StoreSmoothMotionHistory(true);
  fixture.SetWorldPose(At(10.0f), Yaw(0.0f), kUnitScale);
  fixture.object.ResetSmoothing(0.0f);
  ASSERT_TRUE(fixture.object.HasSmoothMotionReset());
  fixture.object.ApplySmoothMotion(Frame(0.5, 2),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   KX_GameObject::SmoothMotionFollow(),
                                   fixture.object.TakeSmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(10.0f));
  fixture.object.ClearPhysicsInterpolationState();

  /* The next frame, still before the next step. */
  fixture.object.ApplySmoothMotion(Frame(0.8, 3),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   KX_GameObject::SmoothMotionFollow(),
                                   KX_GameObject::SmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(10.0f));
}

/* A blended reset (a small correction) slides from where the last frame drew the object to its
 * new place, easing in and out, in the given game seconds. */
TEST(KX_SmoothMotion, ABlendedJumpSlidesFromWhereItWasDrawn)
{
  InterpolationObject fixture;
  fixture.SetWorldPose(At(0.0f), Yaw(0.0f), kUnitScale);
  fixture.object.StoreSmoothMotionHistory(true);
  fixture.SetWorldPose(At(1.0f), Yaw(0.0f), kUnitScale);
  fixture.object.ApplySmoothMotion(Frame(0.5, 1),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   KX_GameObject::SmoothMotionFollow(),
                                   KX_GameObject::SmoothMotionReset(),
                                   nullptr);
  fixture.object.ClearPhysicsInterpolationState();

  fixture.object.StoreSmoothMotionHistory(true);
  fixture.SetWorldPose(At(10.0f), Yaw(0.0f), kUnitScale);
  fixture.object.ResetSmoothing(0.2f);
  /* A blended reset waits for the drawn frame (a step does not take it). */
  fixture.object.StoreSmoothMotionHistory(true);
  ASSERT_TRUE(fixture.object.HasSmoothMotionReset());

  const KX_GameObject::SmoothMotionFollow none;
  fixture.object.ApplySmoothMotion(Frame(0.5, 2),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   none,
                                   fixture.object.TakeSmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(0.5f));
  fixture.object.ClearPhysicsInterpolationState();

  /* Half the time later: halfway (smoothstep(0.5) = 0.5). */
  fixture.object.ApplySmoothMotion(Frame(0.5, 3, 0.1),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   none,
                                   KX_GameObject::SmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(5.25f));
  fixture.object.ClearPhysicsInterpolationState();

  fixture.object.ApplySmoothMotion(Frame(0.5, 4, 0.1),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   none,
                                   KX_GameObject::SmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(10.0f));
}

/* Without a frame drawn just before, there is nothing to slide from: a blended reset snaps. */
TEST(KX_SmoothMotion, ABlendWithoutALastFrameSnaps)
{
  InterpolationObject fixture;
  fixture.SetWorldPose(At(0.0f), Yaw(0.0f), kUnitScale);
  fixture.object.StoreSmoothMotionHistory(true);
  fixture.SetWorldPose(At(10.0f), Yaw(0.0f), kUnitScale);
  fixture.object.ResetSmoothing(0.5f);
  fixture.object.ApplySmoothMotion(Frame(0.5, 7),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   KX_GameObject::SmoothMotionFollow(),
                                   fixture.object.TakeSmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(10.0f));
}

/* On Update logic between the steps (Mouse Look turning the body, a teleport): the change moves
 * the step history along, so it shows at once and the step's own move stays smooth. Before, the
 * changed object's history was its new pose: a frame without a step drew it a step ahead. */
TEST(KX_SmoothMotion, OnUpdateChangesMoveTheStepHistory)
{
  InterpolationObject fixture;
  fixture.node->SetParentRelation(new KX_NormalParentRelation());
  fixture.node->SetLocalPosition(At(0.0f));
  fixture.node->SetLocalOrientation(Yaw(0.0f));
  fixture.node->UpdateWorldData(0.0);
  fixture.object.StoreSmoothMotionHistory(true);
  /* The step moves it 1 along X. */
  fixture.node->SetLocalPosition(At(1.0f));
  fixture.node->UpdateWorldData(0.0);

  /* On Update: up 5 along Y, turned 0.3. */
  SG_Node::SetHistoryFollowsUpdates(true);
  fixture.node->SetLocalPosition(At(1.0f, 5.0f));
  fixture.node->SetLocalOrientation(Yaw(0.3f));
  fixture.node->UpdateWorldData(0.0);
  SG_Node::SetHistoryFollowsUpdates(false);

  ExpectVectorNear(fixture.node->GetPrevWorldPosition(), At(0.0f, 5.0f));
  ExpectMatrixNear(fixture.node->GetPrevWorldOrientation(), Yaw(0.3f));
  fixture.object.ApplySmoothMotion(Frame(0.5, 1),
                                   OB_SMOOTH_MOTION_INTERPOLATE,
                                   KX_GameObject::SmoothMotionFollow(),
                                   KX_GameObject::SmoothMotionReset(),
                                   nullptr);
  ExpectVectorNear(fixture.node->GetWorldPosition(), At(0.5f, 5.0f));
  ExpectMatrixNear(fixture.node->GetWorldOrientation(), Yaw(0.3f));
}

/* The step's own updates leave the history alone. */
TEST(KX_SmoothMotion, StepUpdatesKeepTheStepHistory)
{
  InterpolationObject fixture;
  fixture.node->SetParentRelation(new KX_NormalParentRelation());
  fixture.node->SetLocalPosition(At(0.0f));
  fixture.node->UpdateWorldData(0.0);
  fixture.object.StoreSmoothMotionHistory(true);
  fixture.node->SetLocalPosition(At(3.0f));
  fixture.node->UpdateWorldData(0.0);
  ExpectVectorNear(fixture.node->GetPrevWorldPosition(), At(0.0f));
}

TEST(KX_PhysicsInterpolation, SchedulerStepLimitsClampDefensively)
{
  EXPECT_EQ(KX_KetsjiEngine::ClampScheduledStepLimit(std::numeric_limits<int>::min()), 1);
  EXPECT_EQ(KX_KetsjiEngine::ClampScheduledStepLimit(0), 1);
  EXPECT_EQ(KX_KetsjiEngine::ClampScheduledStepLimit(1), 1);
  EXPECT_EQ(KX_KetsjiEngine::ClampScheduledStepLimit(500), 500);
  EXPECT_EQ(KX_KetsjiEngine::ClampScheduledStepLimit(
                KX_KetsjiEngine::MAX_SCHEDULED_STEPS),
            KX_KetsjiEngine::MAX_SCHEDULED_STEPS);
  EXPECT_EQ(KX_KetsjiEngine::ClampScheduledStepLimit(std::numeric_limits<int>::max()),
            KX_KetsjiEngine::MAX_SCHEDULED_STEPS);
}

}  // namespace

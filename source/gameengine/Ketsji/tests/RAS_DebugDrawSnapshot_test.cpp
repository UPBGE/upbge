/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "RAS_DebugDraw.h"

struct RAS_DebugDrawTestAccess {
  static size_t FrameLineCount(const RAS_DebugDraw &debug_draw)
  {
    return debug_draw.m_lines.size();
  }

  static size_t PublishedLineCount(const RAS_DebugDraw &debug_draw, const void *context)
  {
    const auto found = debug_draw.m_fixedStepPublished.find(context);
    return found != debug_draw.m_fixedStepPublished.end() ? found->second.lines.size() : 0;
  }

  static size_t PublishedCircleCount(const RAS_DebugDraw &debug_draw, const void *context)
  {
    const auto found = debug_draw.m_fixedStepPublished.find(context);
    return found != debug_draw.m_fixedStepPublished.end() ? found->second.circles.size() : 0;
  }

  static bool IsCollecting(const RAS_DebugDraw &debug_draw)
  {
    return debug_draw.m_collectingFixedStep;
  }

  static void ClearFrameBuffer(RAS_DebugDraw &debug_draw)
  {
    debug_draw.ClearFrameBuffer();
  }
};

namespace {

const MT_Vector4 kColor(1.0f, 0.5f, 0.25f, 1.0f);

void DrawTestLine(RAS_DebugDraw &debug_draw, const float x)
{
  debug_draw.DrawLine(MT_Vector3(x, 0.0f, 0.0f), MT_Vector3(x, 1.0f, 0.0f), kColor);
}

TEST(RAS_DebugDrawSnapshot, LatestFixedStepReplacesEarlierStepsBeforeRender)
{
  RAS_DebugDraw debug_draw;
  int scene = 0;

  debug_draw.BeginFixedStepSnapshot();
  debug_draw.SetFixedStepSnapshotContext(&scene);
  DrawTestLine(debug_draw, 1.0f);
  debug_draw.PublishFixedStepSnapshot();
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &scene), 1u);

  debug_draw.BeginFixedStepSnapshot();
  debug_draw.SetFixedStepSnapshotContext(&scene);
  DrawTestLine(debug_draw, 2.0f);
  DrawTestLine(debug_draw, 3.0f);
  debug_draw.PublishFixedStepSnapshot();

  /* Two intentional lines from the latest simulation state survive. The earlier tick does not
   * accumulate into the presentation snapshot. */
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &scene), 2u);
}

TEST(RAS_DebugDrawSnapshot, RenderFrameConsumptionDoesNotConsumeFixedSnapshot)
{
  RAS_DebugDraw debug_draw;
  int scene = 0;

  debug_draw.BeginFixedStepSnapshot();
  debug_draw.SetFixedStepSnapshotContext(&scene);
  DrawTestLine(debug_draw, 1.0f);
  debug_draw.PublishFixedStepSnapshot();

  DrawTestLine(debug_draw, 2.0f);
  EXPECT_EQ(RAS_DebugDrawTestAccess::FrameLineCount(debug_draw), 1u);
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &scene), 1u);

  /* Flush clears the frame buffer through the same helper. The fixed snapshot must remain
   * available to every render until another fixed step publishes a replacement. */
  RAS_DebugDrawTestAccess::ClearFrameBuffer(debug_draw);
  EXPECT_EQ(RAS_DebugDrawTestAccess::FrameLineCount(debug_draw), 0u);
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &scene), 1u);
}

TEST(RAS_DebugDrawSnapshot, EmptyFixedStepExpiresPreviousVisualization)
{
  RAS_DebugDraw debug_draw;
  int scene = 0;

  debug_draw.BeginFixedStepSnapshot();
  debug_draw.SetFixedStepSnapshotContext(&scene);
  DrawTestLine(debug_draw, 1.0f);
  debug_draw.PublishFixedStepSnapshot();
  ASSERT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &scene), 1u);

  debug_draw.BeginFixedStepSnapshot();
  debug_draw.SetFixedStepSnapshotContext(&scene);
  debug_draw.PublishFixedStepSnapshot();
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &scene), 0u);
}

TEST(RAS_DebugDrawSnapshot, SceneContextsAreIsolatedAndDiscardable)
{
  RAS_DebugDraw debug_draw;
  int first_scene = 0;
  int second_scene = 0;

  debug_draw.BeginFixedStepSnapshot();
  debug_draw.SetFixedStepSnapshotContext(&first_scene);
  DrawTestLine(debug_draw, 1.0f);
  debug_draw.SetFixedStepSnapshotContext(&second_scene);
  DrawTestLine(debug_draw, 2.0f);
  debug_draw.DrawCircle(
      MT_Vector3(0.0f, 0.0f, 0.0f), 1.0f, kColor, MT_Vector3(0.0f, 0.0f, 1.0f), 16);
  debug_draw.PublishFixedStepSnapshot();

  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &first_scene), 1u);
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &second_scene), 1u);
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedCircleCount(debug_draw, &first_scene), 0u);
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedCircleCount(debug_draw, &second_scene), 1u);

  debug_draw.DiscardFixedStepSnapshotContext(&first_scene);
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &first_scene), 0u);
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &second_scene), 1u);

  debug_draw.Reset();
  EXPECT_FALSE(RAS_DebugDrawTestAccess::IsCollecting(debug_draw));
  EXPECT_EQ(RAS_DebugDrawTestAccess::PublishedLineCount(debug_draw, &second_scene), 0u);
}

}  // namespace

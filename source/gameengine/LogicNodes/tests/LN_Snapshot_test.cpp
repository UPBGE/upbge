/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <memory>

#include "DEV_InputDevice.h"
#include "LN_Program.h"
#include "LN_Snapshot.h"

namespace {

bool HasChannel(const LN_SnapshotChannelMask mask, const LN_SnapshotChannelMask channel)
{
  return (mask & channel) != 0u;
}

std::shared_ptr<LN_Program> CreateTransformSnapshotProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::SnapshotWorldPosition;
  program->AddVectorExpression(expression);
  return program;
}

std::shared_ptr<LN_Program> CreateTimingSnapshotProgram()
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::SnapshotFrameDelta;
  program->AddFloatExpression(expression);
  return program;
}

}  // namespace

TEST(LN_Snapshot, DependencyChannelsMapToCaptureChannels)
{
  const LN_SnapshotChannelMask mask = LN_SnapshotChannelMaskFromDependencies(
      LN_DEP_SNAPSHOT_TRANSFORM | LN_DEP_SNAPSHOT_LIGHT | LN_DEP_SNAPSHOT_GAME_PROPERTY |
      LN_DEP_SNAPSHOT_OBJECT_GRAPH);

  EXPECT_TRUE(HasChannel(mask, LN_SNAPSHOT_CHANNEL_TRANSFORM));
  EXPECT_TRUE(HasChannel(mask, LN_SNAPSHOT_CHANNEL_LIGHT));
  EXPECT_TRUE(HasChannel(mask, LN_SNAPSHOT_CHANNEL_GAME_PROPERTY));
  EXPECT_TRUE(HasChannel(mask, LN_SNAPSHOT_CHANNEL_OBJECT_IDENTITY));
  EXPECT_FALSE(HasChannel(mask, LN_SNAPSHOT_CHANNEL_CHARACTER));
}

TEST(LN_Snapshot, ProgramWithoutSnapshotDependenciesSkipsBroadObjectState)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  LN_Snapshot snapshot;
  LN_TickReadContext context;
  context.tick_index = 42;

  snapshot.Capture(nullptr, program.get(), &context, 1.0f / 60.0f);
  const LN_SnapshotCaptureStats &stats = snapshot.GetCaptureStats();

  EXPECT_EQ(stats.declared_channels, uint32_t(LN_SNAPSHOT_CHANNEL_NONE));
  EXPECT_EQ(stats.captured_channels, uint32_t(LN_SNAPSHOT_CHANNEL_NONE));
  EXPECT_TRUE(HasChannel(stats.skipped_channels, LN_SNAPSHOT_CHANNEL_TRANSFORM));
  EXPECT_TRUE(HasChannel(stats.skipped_channels, LN_SNAPSHOT_CHANNEL_LIGHT));
  EXPECT_EQ(snapshot.GetTickIndex(), 42u);
}

TEST(LN_Snapshot, TransformOnlyProgramDoesNotCaptureLightCharacterOrProperties)
{
  std::shared_ptr<LN_Program> program = CreateTransformSnapshotProgram();
  LN_Snapshot snapshot;
  LN_TickReadContext context;
  context.tick_index = 7;

  snapshot.Capture(nullptr, program.get(), &context, 1.0f / 60.0f);
  const LN_SnapshotCaptureStats &stats = snapshot.GetCaptureStats();

  EXPECT_TRUE(HasChannel(stats.declared_channels, LN_SNAPSHOT_CHANNEL_TRANSFORM));
  EXPECT_TRUE(HasChannel(stats.captured_channels, LN_SNAPSHOT_CHANNEL_TRANSFORM));
  EXPECT_FALSE(HasChannel(stats.captured_channels, LN_SNAPSHOT_CHANNEL_LIGHT));
  EXPECT_FALSE(HasChannel(stats.captured_channels, LN_SNAPSHOT_CHANNEL_CHARACTER));
  EXPECT_FALSE(HasChannel(stats.captured_channels, LN_SNAPSHOT_CHANNEL_GAME_PROPERTY));
}

TEST(LN_Snapshot, SharedTickContextProvidesTimingWithoutPerTreeInputCopy)
{
  std::shared_ptr<LN_Program> program = CreateTimingSnapshotProgram();
  LN_TickReadContext context;
  context.tick_index = 100;
  context.has_timing = true;
  context.frame_delta = 0.125f;
  context.fps = 8.0f;
  context.delta_factor = 7.5f;

  LN_Snapshot snapshot;
  snapshot.Capture(nullptr, program.get(), &context, 1.0f / 60.0f);

  EXPECT_TRUE(snapshot.HasCapturedChannel(LN_SNAPSHOT_CHANNEL_TIMING));
  EXPECT_EQ(snapshot.GetFrameDelta(), 0.125f);
  EXPECT_EQ(snapshot.GetFPS(), 8.0f);
  EXPECT_EQ(snapshot.GetDeltaFactor(), 7.5f);
  EXPECT_FALSE(snapshot.GetCaptureStats().used_shared_input);
}

TEST(LN_Snapshot, MouseDeltaPreservesSameFrameMovementBeforeRecentering)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;

  input_device.ConvertMoveEvent(100, 100);
  input_snapshot.Capture(&input_device, nullptr);
  input_device.ClearInputs();

  input_device.ConvertMoveEvent(180, 70);
  input_device.ConvertMoveEvent(100, 100);
  input_snapshot.Capture(&input_device, nullptr);

  const LN_MouseSnapshot &mouse = input_snapshot.GetMouse();
  EXPECT_TRUE(mouse.has_position);
  EXPECT_EQ(mouse.x, 100);
  EXPECT_EQ(mouse.y, 100);
  EXPECT_EQ(mouse.delta_x, 80);
  EXPECT_EQ(mouse.delta_y, -30);
}

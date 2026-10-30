/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <limits>
#include <memory>

#include "DEV_InputDevice.h"
#include "LN_Program.h"
#include "LN_Snapshot.h"
#include "SCA_InputEvent.h"

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

TEST(LN_Snapshot, FirstMouseSampleAndDeviceReconnectionEstablishAStableBaseline)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;

  input_device.ConvertMoveEvent(640, 360);
  input_snapshot.Capture(&input_device, nullptr);
  EXPECT_EQ(input_snapshot.GetMouse().delta_x, 0);
  EXPECT_EQ(input_snapshot.GetMouse().delta_y, 0);

  input_device.ClearInputs();
  input_device.ConvertMoveEvent(650, 355);
  input_snapshot.Capture(&input_device, nullptr);
  EXPECT_EQ(input_snapshot.GetMouse().delta_x, 10);
  EXPECT_EQ(input_snapshot.GetMouse().delta_y, -5);

  input_snapshot.Capture(nullptr, nullptr);
  input_device.ClearInputs();
  input_device.ConvertMoveEvent(100, 100);
  input_snapshot.Capture(&input_device, nullptr);
  EXPECT_EQ(input_snapshot.GetMouse().delta_x, 0);
  EXPECT_EQ(input_snapshot.GetMouse().delta_y, 0);
}

TEST(LN_Snapshot, FirstMouseSamplePreservesMotionAfterItsInitialBaselineEvent)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;

  input_device.ConvertMoveEvent(640, 360);
  input_device.ConvertMoveEvent(650, 355);
  input_snapshot.Capture(&input_device, nullptr);

  EXPECT_EQ(input_snapshot.GetMouse().x, 650);
  EXPECT_EQ(input_snapshot.GetMouse().y, 355);
  EXPECT_EQ(input_snapshot.GetMouse().delta_x, 10);
  EXPECT_EQ(input_snapshot.GetMouse().delta_y, -5);
}

TEST(LN_Snapshot, MouseWheelPreservesMagnitudeDirectionAndEveryEvent)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot frame;
  LN_InputSnapshot pending_fixed;

  input_device.ConvertWheelEvent(2);
  input_device.ConvertWheelEvent(3);
  input_device.ConvertWheelEvent(-4);
  frame.Capture(&input_device, nullptr);
  EXPECT_EQ(frame.GetMouse().wheel_delta, 1);
  pending_fixed.CopyFrom(frame);

  input_device.ClearInputs();
  input_device.ReleaseMoveEvent();
  input_device.ConvertWheelEvent(-2);
  frame.Capture(&input_device, nullptr);
  EXPECT_EQ(frame.GetMouse().wheel_delta, -2);
  pending_fixed.AccumulateFrame(frame);
  EXPECT_EQ(pending_fixed.GetMouse().wheel_delta, -1);

  pending_fixed.ClearTransientState();
  EXPECT_EQ(pending_fixed.GetMouse().wheel_delta, 0);

  DEV_InputDevice saturating_device;
  saturating_device.ConvertWheelEvent(std::numeric_limits<int32_t>::max());
  saturating_device.ConvertWheelEvent(std::numeric_limits<int32_t>::max());
  frame.Capture(&saturating_device, nullptr);
  EXPECT_EQ(frame.GetMouse().wheel_delta, std::numeric_limits<int32_t>::max());
}

TEST(LN_Snapshot, FixedInputAccumulatesFrameEdgesAndConsumesTransientsOnce)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot frame;
  LN_InputSnapshot pending_fixed;

  input_device.ConvertEvent(SCA_IInputDevice::SPACEKEY, 1, uint32_t(' '));
  frame.Capture(&input_device, nullptr);
  pending_fixed.CopyFrom(frame);
  input_device.ClearInputs();

  input_device.ConvertEvent(SCA_IInputDevice::SPACEKEY, 0, 0);
  frame.Capture(&input_device, nullptr);
  pending_fixed.AccumulateFrame(frame);

  EXPECT_TRUE(pending_fixed.GetStatus(SCA_IInputDevice::SPACEKEY,
                                      SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(pending_fixed.GetStatus(SCA_IInputDevice::SPACEKEY,
                                      SCA_InputEvent::JUSTRELEASED));
  EXPECT_TRUE(pending_fixed.GetStatus(SCA_IInputDevice::SPACEKEY, SCA_InputEvent::NONE));
  EXPECT_FALSE(pending_fixed.GetStatus(SCA_IInputDevice::SPACEKEY, SCA_InputEvent::ACTIVE));
  EXPECT_EQ(pending_fixed.GetUnicode(SCA_IInputDevice::SPACEKEY), uint32_t(' '));

  pending_fixed.ClearTransientState();
  EXPECT_FALSE(pending_fixed.GetStatus(SCA_IInputDevice::SPACEKEY,
                                       SCA_InputEvent::JUSTACTIVATED));
  EXPECT_FALSE(pending_fixed.GetStatus(SCA_IInputDevice::SPACEKEY,
                                       SCA_InputEvent::JUSTRELEASED));
  EXPECT_TRUE(pending_fixed.GetStatus(SCA_IInputDevice::SPACEKEY, SCA_InputEvent::NONE));
  EXPECT_EQ(pending_fixed.GetUnicode(SCA_IInputDevice::SPACEKEY), 0u);
}

TEST(LN_Snapshot, FrameAndFixedConsumersRetainIndependentTransientQueues)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot sample;
  LN_InputSnapshot pending_update;
  LN_InputSnapshot pending_fixed;

  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 1, uint32_t('a'));
  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 0, 0);
  sample.Capture(&input_device, nullptr);
  pending_update.CopyFrom(sample);
  pending_fixed.CopyFrom(sample);

  pending_fixed.ClearTransientState();
  EXPECT_FALSE(
      pending_fixed.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_FALSE(
      pending_fixed.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTRELEASED));
  EXPECT_TRUE(
      pending_update.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(
      pending_update.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTRELEASED));

  pending_update.ClearTransientState();
  EXPECT_FALSE(
      pending_update.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_FALSE(
      pending_update.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTRELEASED));
}

TEST(LN_Snapshot, QuickKeyTapRetainsTheCharacterAssociatedWithKeyDown)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;

  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 1, uint32_t('a'));
  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 0, 0);
  input_snapshot.Capture(&input_device, nullptr);

  EXPECT_TRUE(
      input_snapshot.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(input_snapshot.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTRELEASED));
  EXPECT_EQ(input_snapshot.GetUnicode(SCA_IInputDevice::AKEY), uint32_t('a'));
  ASSERT_EQ(input_snapshot.GetTextInput().size(), 1u);
  EXPECT_EQ(input_snapshot.GetTextInput().front(), wchar_t('a'));
}

TEST(LN_Snapshot, RepeatedCaptureOfRetainedRawInputDoesNotReplayTransients)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;

  input_device.ConvertMoveEvent(100, 100);
  input_device.ConvertMoveEvent(115, 92);
  input_device.ConvertWheelEvent(3);
  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 1, uint32_t('a'));
  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 0, 0);
  input_snapshot.Capture(&input_device, nullptr);

  EXPECT_TRUE(
      input_snapshot.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(input_snapshot.GetStatus(SCA_IInputDevice::AKEY,
                                      SCA_InputEvent::JUSTRELEASED));
  EXPECT_EQ(input_snapshot.GetMouse().delta_x, 15);
  EXPECT_EQ(input_snapshot.GetMouse().delta_y, -8);
  EXPECT_EQ(input_snapshot.GetMouse().wheel_delta, 3);
  EXPECT_EQ(input_snapshot.GetTextInput(), std::wstring(1, wchar_t('a')));

  /* The fixed scheduler may sample again before legacy consumers are allowed to clear the raw
   * device queue. The second sample contains current state but no repeated one-shot data. */
  input_snapshot.Capture(&input_device, nullptr);
  EXPECT_FALSE(
      input_snapshot.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_FALSE(input_snapshot.GetStatus(SCA_IInputDevice::AKEY,
                                       SCA_InputEvent::JUSTRELEASED));
  EXPECT_EQ(input_snapshot.GetMouse().delta_x, 0);
  EXPECT_EQ(input_snapshot.GetMouse().delta_y, 0);
  EXPECT_EQ(input_snapshot.GetMouse().wheel_delta, 0);
  EXPECT_TRUE(input_snapshot.GetTextInput().empty());
}

TEST(LN_Snapshot, IncrementalCaptureReadsOnlyNewlyAppendedRawEvents)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot input_snapshot;

  input_device.ConvertMoveEvent(100, 100);
  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 1, uint32_t('a'));
  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 0, 0);
  input_snapshot.Capture(&input_device, nullptr);

  input_device.ConvertMoveEvent(125, 90);
  input_device.ConvertWheelEvent(-2);
  input_device.ConvertEvent(SCA_IInputDevice::BKEY, 1, uint32_t('b'));
  input_device.ConvertEvent(SCA_IInputDevice::BKEY, 0, 0);
  input_snapshot.Capture(&input_device, nullptr);

  EXPECT_FALSE(
      input_snapshot.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_FALSE(input_snapshot.GetStatus(SCA_IInputDevice::AKEY,
                                       SCA_InputEvent::JUSTRELEASED));
  EXPECT_TRUE(
      input_snapshot.GetStatus(SCA_IInputDevice::BKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(input_snapshot.GetStatus(SCA_IInputDevice::BKEY,
                                      SCA_InputEvent::JUSTRELEASED));
  EXPECT_EQ(input_snapshot.GetMouse().delta_x, 25);
  EXPECT_EQ(input_snapshot.GetMouse().delta_y, -10);
  EXPECT_EQ(input_snapshot.GetMouse().wheel_delta, -2);
  EXPECT_EQ(input_snapshot.GetTextInput(), std::wstring(1, wchar_t('b')));
}

TEST(LN_Snapshot, DiscardingSuspendedFixedTransientsRetainsHeldAndAbsoluteState)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot pending_fixed;

  input_device.ConvertEvent(SCA_IInputDevice::AKEY, 1, uint32_t('a'));
  input_device.ConvertMoveEvent(200, 100);
  input_device.ConvertWheelEvent(3);
  pending_fixed.Capture(&input_device, nullptr);
  EXPECT_TRUE(
      pending_fixed.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(pending_fixed.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::ACTIVE));
  EXPECT_FALSE(pending_fixed.GetTextInput().empty());
  EXPECT_EQ(pending_fixed.GetMouse().wheel_delta, 3);

  pending_fixed.ClearTransientState();
  EXPECT_FALSE(
      pending_fixed.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(pending_fixed.GetStatus(SCA_IInputDevice::AKEY, SCA_InputEvent::ACTIVE));
  EXPECT_EQ(pending_fixed.GetUnicode(SCA_IInputDevice::AKEY), 0u);
  EXPECT_TRUE(pending_fixed.GetTextInput().empty());
  EXPECT_EQ(pending_fixed.GetMouse().delta_x, 0);
  EXPECT_EQ(pending_fixed.GetMouse().delta_y, 0);
  EXPECT_EQ(pending_fixed.GetMouse().wheel_delta, 0);
  EXPECT_EQ(pending_fixed.GetMouse().x, 200);
  EXPECT_EQ(pending_fixed.GetMouse().y, 100);
}

TEST(LN_Snapshot, FixedInputAccumulatesMouseMotionAcrossRenderFrames)
{
  DEV_InputDevice input_device;
  LN_InputSnapshot frame;
  LN_InputSnapshot pending_fixed;

  input_device.ConvertMoveEvent(100, 100);
  frame.Capture(&input_device, nullptr);
  input_device.ClearInputs();

  input_device.ConvertMoveEvent(110, 95);
  frame.Capture(&input_device, nullptr);
  pending_fixed.CopyFrom(frame);
  input_device.ClearInputs();

  input_device.ConvertMoveEvent(125, 90);
  frame.Capture(&input_device, nullptr);
  pending_fixed.AccumulateFrame(frame);

  EXPECT_EQ(pending_fixed.GetMouse().delta_x, 25);
  EXPECT_EQ(pending_fixed.GetMouse().delta_y, -10);
  pending_fixed.ClearTransientState();
  EXPECT_EQ(pending_fixed.GetMouse().delta_x, 0);
  EXPECT_EQ(pending_fixed.GetMouse().delta_y, 0);
  EXPECT_EQ(pending_fixed.GetMouse().x, 125);
  EXPECT_EQ(pending_fixed.GetMouse().y, 90);
}

TEST(LN_Snapshot, GamepadControlStateSeparatesPressedHeldAndReleased)
{
  LN_GamepadSnapshot gamepad;
  gamepad.connected = true;
  gamepad.buttons[2] = 1;
  gamepad.pressed[2] = 1;
  gamepad.released[2] = 1;
  gamepad.axes_active[4] = 1;
  gamepad.axes_pressed[4] = 1;
  gamepad.axes_released[4] = 1;

  for (const bool is_trigger : {false, true}) {
    const int32_t control_index = is_trigger ? 4 : 2;
    EXPECT_TRUE(LN_GetGamepadControlState(
        gamepad, control_index, is_trigger, SCA_InputEvent::JUSTACTIVATED));
    EXPECT_TRUE(
        LN_GetGamepadControlState(gamepad, control_index, is_trigger, SCA_InputEvent::ACTIVE));
    EXPECT_TRUE(LN_GetGamepadControlState(
        gamepad, control_index, is_trigger, SCA_InputEvent::JUSTRELEASED));
  }

  EXPECT_FALSE(
      LN_GetGamepadControlState(gamepad, -1, false, SCA_InputEvent::ACTIVE));
  EXPECT_FALSE(LN_GetGamepadControlState(
      gamepad, LN_MAX_GAMEPAD_BUTTONS, false, SCA_InputEvent::ACTIVE));
  EXPECT_FALSE(LN_GetGamepadControlState(
      gamepad, LN_MAX_GAMEPAD_AXES, true, SCA_InputEvent::ACTIVE));

  gamepad.connected = false;
  EXPECT_TRUE(LN_GetGamepadControlState(
      gamepad, 2, false, SCA_InputEvent::JUSTRELEASED));
  EXPECT_FALSE(
      LN_GetGamepadControlState(gamepad, 2, false, SCA_InputEvent::ACTIVE));
}

TEST(LN_Snapshot, GamepadTriggerNormalizationUsesCanonicalPositiveRange)
{
  EXPECT_EQ(LN_NormalizeGamepadTriggerValue(-32768), 0.0f);
  EXPECT_EQ(LN_NormalizeGamepadTriggerValue(0), 0.0f);
  EXPECT_NEAR(LN_NormalizeGamepadTriggerValue(16384), 0.5f, 0.0001f);
  EXPECT_EQ(LN_NormalizeGamepadTriggerValue(32767), 1.0f);
  EXPECT_EQ(LN_NormalizeGamepadTriggerValue(65535), 1.0f);

  LN_GamepadSnapshot gamepad;
  gamepad.connected = true;
  gamepad.axes[4] = -32768;
  float strength = -1.0f;
  ASSERT_TRUE(LN_TryGetGamepadTriggerStrength(gamepad, 4, strength));
  EXPECT_EQ(strength, 0.0f);
  EXPECT_FALSE(LN_GamepadHasActiveInput(gamepad));

  gamepad.axes[4] = 16384;
  ASSERT_TRUE(LN_TryGetGamepadTriggerStrength(gamepad, 4, strength));
  EXPECT_NEAR(strength, 0.5f, 0.0001f);
  EXPECT_TRUE(LN_GamepadHasActiveInput(gamepad));

  EXPECT_FALSE(LN_TryGetGamepadTriggerStrength(gamepad, -1, strength));
  EXPECT_FALSE(LN_TryGetGamepadTriggerStrength(gamepad, LN_MAX_GAMEPAD_AXES, strength));
  gamepad.connected = false;
  EXPECT_FALSE(LN_TryGetGamepadTriggerStrength(gamepad, 4, strength));
}

namespace {

void SampleMouseButton(DEV_InputDevice &device, LN_InputSnapshot &sample, const int value)
{
  device.ClearInputs();
  if (value >= 0) {
    device.ConvertEvent(SCA_IInputDevice::LEFTMOUSE, value, 0);
  }
  sample.Capture(&device, nullptr);
}

bool MouseStatus(const LN_InputSnapshot &snapshot, const int status)
{
  return snapshot.GetStatus(SCA_IInputDevice::LEFTMOUSE, status);
}

}  // namespace

TEST(LN_InputConsumption, AConsumedStrokeHidesPressHoldAndReleaseUntilItEnds)
{
  DEV_InputDevice device;
  LN_InputSnapshot sample;
  LN_InputConsumption consumption;
  LN_InputSnapshot game_frame;

  /* Frame 1: the UI takes the press. */
  consumption.BeginFrame();
  SampleMouseButton(device, sample, 1);
  consumption.NoteSample(sample);
  consumption.ConsumeStroke(SCA_IInputDevice::LEFTMOUSE, true);
  game_frame.CopyFrom(sample);
  consumption.ApplyToFrame(game_frame);
  EXPECT_FALSE(MouseStatus(game_frame, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_FALSE(MouseStatus(game_frame, SCA_InputEvent::ACTIVE));

  /* Frame 2: still held, still hidden; the unfiltered sample keeps it for the UI. */
  consumption.BeginFrame();
  SampleMouseButton(device, sample, -1);
  ASSERT_TRUE(MouseStatus(sample, SCA_InputEvent::ACTIVE));
  consumption.NoteSample(sample);
  game_frame.CopyFrom(sample);
  consumption.ApplyToFrame(game_frame);
  EXPECT_FALSE(MouseStatus(game_frame, SCA_InputEvent::ACTIVE));

  /* Released between frames, in a pass without a frame: the input waiting for Logic + Physics
   * hides the release, and so does the next frame. */
  SampleMouseButton(device, sample, 0);
  ASSERT_TRUE(MouseStatus(sample, SCA_InputEvent::JUSTRELEASED));
  consumption.NoteSample(sample);
  LN_InputSnapshot fixed;
  fixed.CopyFrom(sample);
  consumption.ApplyStrokesTo(fixed);
  EXPECT_FALSE(MouseStatus(fixed, SCA_InputEvent::JUSTRELEASED));
  consumption.BeginFrame();
  EXPECT_TRUE(consumption.IsStrokeConsumed(SCA_IInputDevice::LEFTMOUSE));
  game_frame.CopyFrom(sample);
  consumption.ApplyToFrame(game_frame);
  EXPECT_FALSE(MouseStatus(game_frame, SCA_InputEvent::JUSTRELEASED));

  /* The stroke ended: the next click belongs to game logic. */
  consumption.BeginFrame();
  EXPECT_FALSE(consumption.IsStrokeConsumed(SCA_IInputDevice::LEFTMOUSE));
  SampleMouseButton(device, sample, 1);
  consumption.NoteSample(sample);
  game_frame.CopyFrom(sample);
  consumption.ApplyToFrame(game_frame);
  EXPECT_TRUE(MouseStatus(game_frame, SCA_InputEvent::JUSTACTIVATED));
}

TEST(LN_InputConsumption, ANewPressRightAfterAConsumedReleaseIsNotHidden)
{
  DEV_InputDevice device;
  LN_InputSnapshot sample;
  LN_InputConsumption consumption;
  SampleMouseButton(device, sample, 1);
  consumption.ConsumeStroke(SCA_IInputDevice::LEFTMOUSE, true);
  SampleMouseButton(device, sample, 0);
  consumption.NoteSample(sample);
  /* Pressed again before any frame saw the release. */
  SampleMouseButton(device, sample, 1);
  consumption.NoteSample(sample);
  EXPECT_FALSE(consumption.IsStrokeConsumed(SCA_IInputDevice::LEFTMOUSE));
}

TEST(LN_InputConsumption, AConsumedPressIsHiddenOnlyInItsFrameAndTheKeyStaysHeld)
{
  DEV_InputDevice device;
  LN_InputSnapshot sample;
  LN_InputConsumption consumption;
  LN_InputSnapshot game_frame;
  device.ConvertEvent(SCA_IInputDevice::RETKEY, 1, 0);
  sample.Capture(&device, nullptr);
  consumption.ConsumePress(SCA_IInputDevice::RETKEY);
  consumption.ConsumeGamepadPress(0, 0);
  EXPECT_TRUE(consumption.IsGamepadPressConsumed(0, 0));
  game_frame.CopyFrom(sample);
  consumption.ApplyToFrame(game_frame);
  EXPECT_FALSE(game_frame.GetStatus(SCA_IInputDevice::RETKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(game_frame.GetStatus(SCA_IInputDevice::RETKEY, SCA_InputEvent::ACTIVE));

  consumption.BeginFrame();
  EXPECT_FALSE(consumption.IsPressConsumed(SCA_IInputDevice::RETKEY));
  EXPECT_FALSE(consumption.IsGamepadPressConsumed(0, 0));
}

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <limits>
#include <memory>

#include "DEV_InputDevice.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "EXP_Vector3Value.h"
#include "KX_GameObject.h"
#include "LN_CommandBuffer.h"
#include "LN_Program.h"
#include "LN_RuntimeTree.h"
#include "LN_Snapshot.h"
#include "LN_TypeConversion.h"
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
  expression.kind = LN_VectorExpressionKind::AxisVector; /* the own object: a transform read */
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

LN_GamePropertyRef MakeGamePropertyRef(const std::string &name, const LN_ValueType type)
{
  LN_GamePropertyRef property_ref;
  property_ref.name = name;
  property_ref.value_type = type;
  property_ref.default_value.bool_value = true;
  property_ref.default_value.int_value = 3;
  property_ref.default_value.float_value = 7.5f;
  property_ref.default_value.string_value = "default";
  property_ref.default_value.vector_value = MT_Vector3(1.0f, 2.0f, 3.0f);
  return property_ref;
}

/** A program reading `refs` (a game property read declares the channel). */
std::shared_ptr<LN_Program> CreateGamePropertyProgram(const std::vector<LN_GamePropertyRef> &refs)
{
  std::shared_ptr<LN_Program> program = std::make_shared<LN_Program>();
  for (const LN_GamePropertyRef &property_ref : refs) {
    program->AddGamePropertyRef(property_ref);
  }
  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::SnapshotGameProperty;
  expression.property_ref_index = 0;
  program->AddFloatExpression(expression);
  return program;
}

/** A game property's snapshot value as captures built it before they kept their values and the
 * properties they found (2026-10-30): a fresh copy of the default, filled from the property
 * looked up by name. A property of another type than the ref's converts as every other read of a
 * value (2026-11-08): text read as a number is its number (the default when it is none), empty
 * text is False, anything but text read as text is its value's text. */
LN_Value ReferenceGamePropertyValue(KX_GameObject &object, const LN_GamePropertyRef &property_ref)
{
  LN_Value value = property_ref.default_value;
  value.type = property_ref.value_type;
  if (property_ref.name.empty()) {
    return value;
  }
  EXP_Value *property = object.GetProperty(property_ref.name);
  value.exists = property != nullptr;
  if (property == nullptr) {
    return value;
  }
  const int type = property->GetValueType();
  const bool number = type == VALUE_BOOL_TYPE || type == VALUE_INT_TYPE ||
                      type == VALUE_FLOAT_TYPE;
  float text_number = 0.0f;
  const bool text_is_number = type == VALUE_STRING_TYPE &&
                              LN_TryParseTypecastFloat(property->GetText(), text_number);
  switch (property_ref.value_type) {
    case LN_ValueType::Bool:
      if (number) {
        value.bool_value = object.GetPropertyNumber(property_ref.name, 0.0f) != 0.0f;
      }
      else if (type == VALUE_STRING_TYPE) {
        value.bool_value = !property->GetText().empty();
      }
      else {
        value.bool_value = LN_GamePropertyAsBool(property);
      }
      break;
    case LN_ValueType::Int:
      if (number) {
        value.int_value = LN_ConvertFloatToInt(
            object.GetPropertyNumber(property_ref.name, 0.0f), LN_FloatToIntMode::Truncate);
      }
      else if (text_is_number) {
        value.int_value = LN_ConvertFloatToInt(text_number, LN_FloatToIntMode::Truncate);
      }
      break;
    case LN_ValueType::Float:
      if (number) {
        value.float_value = object.GetPropertyNumber(property_ref.name, 0.0f);
      }
      else if (text_is_number) {
        value.float_value = text_number;
      }
      break;
    case LN_ValueType::String:
      value.string_value = type == VALUE_STRING_TYPE ? property->GetText() :
                                                       LN_GamePropertyAsText(property);
      break;
    default:
      break;
  }
  return value;
}

void ExpectSameGamePropertyValue(const LN_Value *actual,
                                 const LN_Value &expected,
                                 const std::string &where)
{
  ASSERT_NE(actual, nullptr) << where;
  EXPECT_EQ(actual->type, expected.type) << where;
  EXPECT_EQ(actual->exists, expected.exists) << where;
  EXPECT_EQ(actual->bool_value, expected.bool_value) << where;
  EXPECT_EQ(actual->int_value, expected.int_value) << where;
  EXPECT_EQ(actual->float_value, expected.float_value) << where;
  EXPECT_EQ(actual->string_value, expected.string_value) << where;
  EXPECT_EQ(actual->vector_value, expected.vector_value) << where;
}

/** The snapshot's value of a ref, built from its cells (null past the last capture's refs), and the
 * typed view of it checked against the reference too. */
const LN_Value *BuiltGamePropertyValue(const LN_Snapshot &snapshot,
                                       const uint32_t index,
                                       LN_Value &storage,
                                       const LN_Value *reference = nullptr,
                                       const std::string &where = "")
{
  if (!snapshot.BuildGamePropertyValue(index, storage)) {
    LN_Snapshot::GamePropertyView view;
    EXPECT_FALSE(snapshot.ReadGameProperty(index, view)) << where;
    return nullptr;
  }
  LN_Snapshot::GamePropertyView view;
  EXPECT_TRUE(snapshot.ReadGameProperty(index, view)) << where;
  if (reference != nullptr) {
    EXPECT_EQ(view.exists, reference->exists) << where;
    EXPECT_EQ(view.bool_value, reference->bool_value) << where;
    EXPECT_EQ(view.int_value, reference->int_value) << where;
    EXPECT_EQ(view.float_value, reference->float_value) << where;
    EXPECT_EQ(*view.string_value, reference->string_value) << where;
  }
  return &storage;
}

struct TypedSnapshotModeGuard {
  ~TypedSnapshotModeGuard()
  {
    LN_Snapshot::SetTypedSnapshotModeForTests(-1);
  }
};

void SetOwnedProperty(KX_GameObject &object, const std::string &name, EXP_Value *value)
{
  object.SetProperty(name, value);
  value->Release();
}

}  // namespace

TEST(LN_Snapshot, GamePropertyStampChangesOnlyWhenTheListChanges)
{
  KX_GameObject object;
  KX_GameObject other;
  EXPECT_NE(object.GetPropertyMapStamp(), other.GetPropertyMapStamp());

  uint64_t stamp = object.GetPropertyMapStamp();
  SetOwnedProperty(object, "health", new EXP_FloatValue(10.0f));
  EXPECT_NE(object.GetPropertyMapStamp(), stamp);

  /* Logic Nodes write numbers and text in place: the property stays the one found. */
  stamp = object.GetPropertyMapStamp();
  EXP_Value *health = object.GetProperty("health");
  LN_Value value;
  value.type = LN_ValueType::Float;
  value.exists = true;
  value.float_value = 4.0f;
  std::string reason;
  ASSERT_TRUE(LN_CommandBuffer::ApplyGamePropertyValue(&object, "health", value, nullptr, reason));
  EXPECT_EQ(object.GetProperty("health"), health);
  EXPECT_EQ(object.GetPropertyMapStamp(), stamp);

  /* Replaced, removed, all removed: a new stamp, never an earlier one. */
  SetOwnedProperty(object, "health", new EXP_IntValue(5));
  const uint64_t replaced = object.GetPropertyMapStamp();
  EXPECT_NE(replaced, stamp);
  EXPECT_TRUE(object.RemoveProperty("health"));
  const uint64_t removed = object.GetPropertyMapStamp();
  EXPECT_NE(removed, replaced);
  EXPECT_NE(removed, stamp);
  object.ClearProperties();
  EXPECT_NE(object.GetPropertyMapStamp(), removed);
}

/* The kept game properties against a fresh capture made by the old code (kept here as the
 * reference), after any change, in each storage mode (plan A10): the old kept values, the typed
 * cells, and both compared at every capture. */
void RunKeptGamePropertiesAgainstTheReference(const int mode)
{
  SCOPED_TRACE("typed snapshot mode " + std::to_string(mode));
  TypedSnapshotModeGuard restore;
  LN_Snapshot::SetTypedSnapshotModeForTests(mode);
  const uint32_t differences_before = LN_Snapshot::TypedSnapshotVerifyDifferencesForTests();
  const std::vector<std::string> names = {"a", "b", "c"};
  const std::vector<LN_ValueType> types = {LN_ValueType::Bool,
                                           LN_ValueType::Int,
                                           LN_ValueType::Float,
                                           LN_ValueType::String,
                                           LN_ValueType::Vector};
  std::vector<LN_GamePropertyRef> refs;
  for (const std::string &name : names) {
    for (const LN_ValueType type : types) {
      refs.push_back(MakeGamePropertyRef(name, type));
    }
  }
  /* A ref without a name keeps its default, `exists` included. */
  LN_GamePropertyRef unnamed = MakeGamePropertyRef("", LN_ValueType::Float);
  unnamed.default_value.exists = true;
  refs.push_back(unnamed);
  /* The other programs read the same refs in reverse order (as many: only the program's
   * identity tells them apart), and some of them. */
  const std::vector<LN_GamePropertyRef> reversed(refs.rbegin(), refs.rend());
  const std::shared_ptr<LN_Program> programs[3] = {
      CreateGamePropertyProgram(refs),
      CreateGamePropertyProgram(reversed),
      CreateGamePropertyProgram({refs[13], refs[2], refs[5], refs[11]})};

  KX_GameObject objects[2];
  LN_Snapshot snapshot;
  LN_TickReadContext context;
  uint32_t state = 12345u;
  const auto next = [&state](const uint32_t range) {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % range;
  };

  for (uint32_t step = 0; step < 4000; step++) {
    KX_GameObject &changed = objects[next(4) == 0 ? 1 : 0];
    const std::string &name = names[next(uint32_t(names.size()))];
    const float number = float(int(next(7)) - 3) * 0.75f;
    switch (next(10)) {
      case 0:
        SetOwnedProperty(changed, name, new EXP_FloatValue(number));
        break;
      case 1:
        SetOwnedProperty(changed, name, new EXP_IntValue(cInt(number)));
        break;
      case 2:
        SetOwnedProperty(changed, name, new EXP_BoolValue(next(2) == 0));
        break;
      case 3:
        SetOwnedProperty(changed, name, new EXP_StringValue(next(2) == 0 ? "12.5" : "text", name));
        break;
      case 4:
        SetOwnedProperty(changed, name, new EXP_Vector3Value(number, 1.0f, 2.0f));
        break;
      case 5:
        changed.RemoveProperty(name);
        break;
      case 6:
        if (next(8) == 0) {
          changed.ClearProperties();
        }
        break;
      default: {
        /* A Logic Nodes write: numbers and text change in place. */
        LN_Value value;
        value.exists = true;
        switch (next(4)) {
          case 0:
            value.type = LN_ValueType::Float;
            value.float_value = number;
            break;
          case 1:
            value.type = LN_ValueType::Int;
            value.int_value = int32_t(number);
            break;
          case 2:
            value.type = LN_ValueType::Bool;
            value.bool_value = number > 0.0f;
            break;
          default:
            value.type = LN_ValueType::String;
            value.string_value = next(2) == 0 ? "-4" : "words";
            break;
        }
        std::string reason;
        LN_CommandBuffer::ApplyGamePropertyValue(&changed, name, value, nullptr, reason);
        break;
      }
    }

    /* Mostly the same object and program, as a tree captures; now and then the others. */
    KX_GameObject &captured = objects[next(6) == 0 ? 1 : 0];
    const LN_Program &program = *programs[next(6) == 0 ? 1 + next(2) : 0];
    context.tick_index = step;
    snapshot.Capture(&captured, &program, &context, 1.0f / 60.0f);
    const std::vector<LN_GamePropertyRef> &program_refs = program.GetGamePropertyRefs();
    for (uint32_t index = 0; index < program_refs.size(); index++) {
      const LN_Value reference = ReferenceGamePropertyValue(captured, program_refs[index]);
      LN_Value kept;
      const std::string where = "step " + std::to_string(step) + " ref " + std::to_string(index);
      ExpectSameGamePropertyValue(
          BuiltGamePropertyValue(snapshot, index, kept, &reference, where), reference, where);
      if (::testing::Test::HasFailure()) {
        return;
      }
    }
    LN_Value past_the_end;
    EXPECT_EQ(BuiltGamePropertyValue(snapshot, uint32_t(program_refs.size()), past_the_end),
              nullptr);

    /* A UI widget's change read again after the capture. */
    if (next(10) == 0 && !program_refs[0].name.empty()) {
      SetOwnedProperty(captured, program_refs[0].name, new EXP_FloatValue(number + 1.0f));
      snapshot.RefreshGameProperties(&captured, &program);
      const LN_Value reference = ReferenceGamePropertyValue(captured, program_refs[0]);
      LN_Value kept;
      const std::string where = "refresh at step " + std::to_string(step);
      ExpectSameGamePropertyValue(
          BuiltGamePropertyValue(snapshot, 0, kept, &reference, where), reference, where);
    }
  }
  EXPECT_EQ(LN_Snapshot::TypedSnapshotVerifyDifferencesForTests(), differences_before);
}

TEST(LN_Snapshot, KeptGamePropertiesMatchAFreshCaptureAfterAnyChange)
{
  RunKeptGamePropertiesAgainstTheReference(1);
}

TEST(LN_Snapshot, KeptGamePropertiesAsOldValuesMatchAFreshCaptureAfterAnyChange)
{
  RunKeptGamePropertiesAgainstTheReference(0);
}

TEST(LN_Snapshot, KeptGamePropertiesInBothStoragesMatchAFreshCaptureAfterAnyChange)
{
  RunKeptGamePropertiesAgainstTheReference(2);
}

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

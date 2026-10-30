/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstdint>
#include <string>

#include "LN_DenseIds.h"
#include "LN_EventBus.h"

class KX_GameObject;

namespace {

KX_GameObject *FakeGameObjectPointer(const uint32_t value)
{
  return reinterpret_cast<KX_GameObject *>(uintptr_t(value));
}

LN_Value IntValue(const int32_t value)
{
  LN_Value result;
  result.type = LN_ValueType::Int;
  result.exists = true;
  result.int_value = value;
  return result;
}

LN_Value StringValue(const std::string &value)
{
  LN_Value result;
  result.type = LN_ValueType::String;
  result.exists = true;
  result.string_value = value;
  return result;
}

}  // namespace

TEST(LN_EventBus, BeginTickMovesPendingEventsAndPreservesDeterministicOrder)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("ordered");

  LN_EventBus bus;
  LN_EventEntry first;
  first.subject_id = subject_id;
  first.subject = "ordered";
  first.content = IntValue(1);
  bus.Publish(first);

  LN_EventEntry second;
  second.subject_id = subject_id;
  second.subject = "ordered";
  second.content = IntValue(2);
  bus.Publish(second);

  ASSERT_EQ(bus.PendingEvents().size(), 2u);
  bus.BeginFrame();

  EXPECT_TRUE(bus.PendingEvents().empty());
  ASSERT_EQ(bus.TickEvents().size(), 2u);
  EXPECT_EQ(bus.TickEventCount(), 2u);
  EXPECT_EQ(bus.TickEvents()[0].content.int_value, 1);
  EXPECT_EQ(bus.TickEvents()[1].content.int_value, 2);
  EXPECT_EQ(bus.TickEvents()[0].payload_lane, LN_EventPayloadLane::Int);

  const LN_EventEntry *match = bus.FindFirst(subject_id, "ordered", LN_ObjectHandle());
  ASSERT_NE(match, nullptr);
  EXPECT_EQ(match->content.int_value, 1);
  EXPECT_EQ(bus.IndexedLookupCount(), 1u);
  EXPECT_EQ(bus.FallbackScanCount(), 0u);
}

TEST(LN_EventBus, BroadcastEventsMatchEveryTargetAndTargetedEventsUseHandles)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("targeted");
  KX_GameObject *target_a = FakeGameObjectPointer(0x10);
  KX_GameObject *target_b = FakeGameObjectPointer(0x20);
  const LN_ObjectHandle target_a_handle = registry.MakeObjectHandle(target_a, "TargetA");
  const LN_ObjectHandle target_b_handle = registry.MakeObjectHandle(target_b, "TargetB");

  LN_EventBus bus;
  LN_EventEntry targeted;
  targeted.subject_id = subject_id;
  targeted.subject = "targeted";
  targeted.content = IntValue(7);
  targeted.target_handle = target_a_handle;
  bus.Publish(targeted);

  LN_EventEntry broadcast;
  broadcast.subject_id = subject_id;
  broadcast.subject = "targeted";
  broadcast.content = IntValue(8);
  bus.Publish(broadcast);
  bus.BeginFrame();

  const LN_EventEntry *match_a = bus.FindFirst(subject_id, "targeted", target_a_handle);
  ASSERT_NE(match_a, nullptr);
  EXPECT_EQ(match_a->content.int_value, 7);

  const LN_EventEntry *match_b = bus.FindFirst(subject_id, "targeted", target_b_handle);
  ASSERT_NE(match_b, nullptr);
  EXPECT_EQ(match_b->content.int_value, 8);

  const LN_EventEntry *dynamic_target_match = bus.FindFirst(
      LN_EventSubjectId(), "targeted", target_a_handle);
  ASSERT_NE(dynamic_target_match, nullptr);
  EXPECT_EQ(dynamic_target_match->content.int_value, 7);
  EXPECT_GE(bus.IndexedLookupCount(), 3u);
}

TEST(LN_EventBus, TargetedEventsDoNotMatchOtherTargetsWithoutBroadcast)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("targeted.only");
  KX_GameObject *target_a = FakeGameObjectPointer(0x10);
  KX_GameObject *target_b = FakeGameObjectPointer(0x20);
  const LN_ObjectHandle target_a_handle = registry.MakeObjectHandle(target_a, "TargetA");
  const LN_ObjectHandle target_b_handle = registry.MakeObjectHandle(target_b, "TargetB");

  LN_EventBus bus;
  LN_EventEntry event;
  event.subject_id = subject_id;
  event.subject = "targeted.only";
  event.content = IntValue(7);
  event.target_handle = target_a_handle;
  bus.Publish(event);
  bus.BeginFrame();

  EXPECT_EQ(bus.FindFirst(subject_id, "targeted.only", target_b_handle), nullptr);
}

TEST(LN_EventBus, DynamicSubjectFallbackMatchesStringSubject)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId static_subject_id = registry.InternEventSubject("dynamic.subject");
  LN_EventBus bus;

  LN_EventEntry static_targeted_to_other_object;
  static_targeted_to_other_object.subject_id = static_subject_id;
  static_targeted_to_other_object.subject = "dynamic.subject";
  static_targeted_to_other_object.target_handle = registry.MakeObjectHandle(
      FakeGameObjectPointer(0x50), "Other");
  static_targeted_to_other_object.content = IntValue(1);
  bus.Publish(static_targeted_to_other_object);

  LN_EventEntry event;
  event.subject = "dynamic.subject";
  event.content = StringValue("payload");
  bus.Publish(event);
  bus.BeginFrame();

  const LN_EventEntry *match = bus.FindFirst(
      static_subject_id, "dynamic.subject", LN_ObjectHandle());
  ASSERT_NE(match, nullptr);
  EXPECT_EQ(match->content.string_value, "payload");
  EXPECT_EQ(match->payload_lane, LN_EventPayloadLane::String);
  EXPECT_EQ(bus.IndexedLookupCount(), 1u);
  EXPECT_EQ(bus.FallbackScanCount(), 1u);
}

TEST(LN_EventBus, EventEntryCarriesContentMessengerAndTargetPayload)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("content");
  KX_GameObject *messenger = FakeGameObjectPointer(0x30);
  KX_GameObject *target = FakeGameObjectPointer(0x40);

  LN_EventBus bus;
  LN_EventEntry event;
  event.subject_id = subject_id;
  event.subject = "content";
  event.content = StringValue("hello");
  event.messenger_handle = registry.MakeObjectHandle(messenger, "Messenger");
  event.target_handle = registry.MakeObjectHandle(target, "Target");
  bus.Publish(event);
  bus.BeginFrame();

  const LN_EventEntry *match = bus.FindFirst(subject_id, "content", event.target_handle);
  ASSERT_NE(match, nullptr);
  EXPECT_EQ(registry.ResolveHandle(match->messenger_handle), messenger);
  EXPECT_EQ(registry.ResolveHandle(match->target_handle), target);
  EXPECT_EQ(match->content.string_value, "hello");
  EXPECT_EQ(match->payload_lane, LN_EventPayloadLane::String);
  EXPECT_TRUE(match->messenger_handle.IsValid());
  EXPECT_TRUE(match->target_handle.IsValid());
}

TEST(LN_EventBus, RemovedMessengerResolvesToNothing)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("hit");
  KX_GameObject *messenger = FakeGameObjectPointer(0x70);

  LN_EventBus bus;
  LN_EventEntry event;
  event.subject_id = subject_id;
  event.subject = "hit";
  event.messenger_handle = registry.MakeObjectHandle(messenger, "Bullet");
  bus.Publish(event);
  bus.BeginFrame();

  /* The sender ends itself before the event is read on the next tick. */
  registry.InvalidateHandle(LN_DenseHandleKind::Object, messenger);
  const LN_EventEntry *match = bus.FindFirst(subject_id, "hit", LN_ObjectHandle());
  ASSERT_NE(match, nullptr);
  EXPECT_EQ(registry.ResolveHandle(match->messenger_handle), nullptr);
}

TEST(LN_EventBus, TargetedEventDoesNotMatchObjectReusingRemovedTargetAddress)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("damage");
  KX_GameObject *address = FakeGameObjectPointer(0x80);

  LN_EventBus bus;
  LN_EventEntry event;
  event.subject_id = subject_id;
  event.subject = "damage";
  event.target_handle = registry.MakeObjectHandle(address, "Target");
  bus.Publish(event);
  bus.BeginFrame();

  registry.InvalidateHandle(LN_DenseHandleKind::Object, address);
  const LN_ObjectHandle reused_handle = registry.MakeObjectHandle(address, "Spawned");
  EXPECT_EQ(bus.FindFirst(subject_id, "damage", reused_handle), nullptr);
  EXPECT_EQ(bus.FindFirst(LN_EventSubjectId(), "damage", reused_handle), nullptr);
}

TEST(LN_EventBus, StaticSubjectIdLookupDoesNotRequireDebugString)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("id.only");

  LN_EventBus bus;
  LN_EventEntry event;
  event.subject_id = subject_id;
  event.content = IntValue(99);
  bus.Publish(event);
  bus.BeginFrame();

  const LN_EventEntry *match = bus.FindFirst(subject_id, "", LN_ObjectHandle());
  ASSERT_NE(match, nullptr);
  EXPECT_EQ(match->content.int_value, 99);
  EXPECT_EQ(bus.IndexedLookupCount(), 1u);
  EXPECT_EQ(bus.FallbackScanCount(), 0u);
}

TEST(LN_EventBus, ClearAllDropsTargetAndBroadcastIndexes)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("clear");
  KX_GameObject *target = FakeGameObjectPointer(0x60);
  const LN_ObjectHandle target_handle = registry.MakeObjectHandle(target, "Target");

  LN_EventBus bus;
  LN_EventEntry targeted;
  targeted.subject_id = subject_id;
  targeted.subject = "clear";
  targeted.target_handle = target_handle;
  targeted.content = IntValue(1);
  bus.Publish(targeted);

  LN_EventEntry broadcast;
  broadcast.subject_id = subject_id;
  broadcast.subject = "clear";
  broadcast.content = IntValue(2);
  bus.Publish(broadcast);
  bus.BeginFrame();
  ASSERT_EQ(bus.TickEventCount(), 2u);

  bus.ClearAll();
  EXPECT_EQ(bus.TickEventCount(), 0u);
  EXPECT_EQ(bus.FindFirst(subject_id, "clear", target_handle), nullptr);
  EXPECT_EQ(bus.IndexedLookupCount(), 0u);
  EXPECT_EQ(bus.FallbackScanCount(), 1u);
}

TEST(LN_EventBus, FixedDeliverySurvivesFramesWithoutSimulationSteps)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("fixed.queue");
  LN_EventBus bus;

  LN_EventEntry first;
  first.subject_id = subject_id;
  first.subject = "fixed.queue";
  first.content = IntValue(1);
  bus.Publish(first);
  bus.BeginFrame();
  EXPECT_EQ(bus.PendingFixedEventCount(), 1u);

  LN_EventEntry second;
  second.subject_id = subject_id;
  second.subject = "fixed.queue";
  second.content = IntValue(2);
  bus.Publish(second);
  bus.BeginFrame();
  EXPECT_EQ(bus.PendingFixedEventCount(), 2u);

  bus.BeginFixedTick();
  ASSERT_EQ(bus.TickEventCount(), 2u);
  EXPECT_EQ(bus.TickEvents()[0].content.int_value, 1);
  EXPECT_EQ(bus.TickEvents()[1].content.int_value, 2);
}

TEST(LN_EventBus, FixedEventsPulseForOnlyOneCatchUpStep)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("fixed.once");
  LN_EventBus bus;

  LN_EventEntry event;
  event.subject_id = subject_id;
  event.subject = "fixed.once";
  event.content = IntValue(7);
  bus.Publish(event);
  bus.BeginFrame();

  bus.BeginFixedTick();
  ASSERT_NE(bus.FindFirst(subject_id, "fixed.once", LN_ObjectHandle()), nullptr);
  bus.BeginFixedTick();
  EXPECT_EQ(bus.FindFirst(subject_id, "fixed.once", LN_ObjectHandle()), nullptr);
}

TEST(LN_EventBus, EventPublishedAfterFrameBeginReachesImmediateFixedStep)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("frame.to.fixed");
  LN_EventBus bus;

  bus.BeginFrame();

  LN_EventEntry event;
  event.subject_id = subject_id;
  event.subject = "frame.to.fixed";
  event.content = IntValue(11);
  bus.Publish(event);

  bus.BeginFixedTick();
  const LN_EventEntry *match =
      bus.FindFirst(subject_id, "frame.to.fixed", LN_ObjectHandle());
  ASSERT_NE(match, nullptr);
  EXPECT_EQ(match->content.int_value, 11);
}

TEST(LN_EventBus, EventPublishedDuringCatchUpReachesOnlyNextFixedStep)
{
  LN_DenseIdRegistry registry;
  const LN_EventSubjectId subject_id = registry.InternEventSubject("fixed.to.fixed");
  LN_EventBus bus;

  bus.BeginFixedTick();

  LN_EventEntry event;
  event.subject_id = subject_id;
  event.subject = "fixed.to.fixed";
  event.content = IntValue(13);
  bus.Publish(event);

  bus.BeginFixedTick();
  const LN_EventEntry *match =
      bus.FindFirst(subject_id, "fixed.to.fixed", LN_ObjectHandle());
  ASSERT_NE(match, nullptr);
  EXPECT_EQ(match->content.int_value, 13);

  bus.BeginFixedTick();
  EXPECT_EQ(bus.FindFirst(subject_id, "fixed.to.fixed", LN_ObjectHandle()), nullptr);
}

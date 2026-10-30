/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstdint>
#include <map>
#include <random>
#include <utility>
#include <vector>

#include "LN_DenseIds.h"

namespace {

void *FakePointer(const uint32_t value)
{
  /* Aligned like heap objects. */
  return reinterpret_cast<void *>(uintptr_t(0x100000) + uintptr_t(value) * 16);
}

LN_DenseHandle MakeHandleOfKind(LN_DenseIdRegistry &registry,
                                const LN_DenseHandleKind kind,
                                void *pointer)
{
  switch (kind) {
    case LN_DenseHandleKind::Scene:
      return registry.MakeSceneHandle(pointer, "Scene");
    case LN_DenseHandleKind::Datablock:
      return registry.MakeDatablockHandle(pointer, "Datablock");
    default:
      return registry.MakeObjectHandle(pointer, "Object");
  }
}

}  // namespace

TEST(LN_DenseIds, NullPointerNeverGetsAHandle)
{
  LN_DenseIdRegistry registry;
  EXPECT_FALSE(registry.MakeObjectHandle(nullptr, "None").IsValid());
  EXPECT_FALSE(registry.FindObjectHandle(nullptr).IsValid());
  registry.InvalidateHandle(LN_DenseHandleKind::Object, nullptr);
  EXPECT_EQ(registry.ResolveHandle(LN_ObjectHandle()), nullptr);
}

TEST(LN_DenseIds, SamePointerOfDifferentKindsGetsSeparateHandles)
{
  for (const uint32_t extra_objects : {0u, 100u}) {
    LN_DenseIdRegistry registry;
    for (uint32_t i = 0; i < extra_objects; i++) {
      registry.MakeObjectHandle(FakePointer(1000 + i), "Filler");
    }
    void *pointer = FakePointer(1);
    const LN_ObjectHandle object = registry.MakeObjectHandle(pointer, "Object");
    const LN_SceneHandle scene = registry.MakeSceneHandle(pointer, "Scene");
    EXPECT_NE(object.index, scene.index);
    EXPECT_EQ(registry.ResolveHandle(object), pointer);
    EXPECT_EQ(registry.ResolveHandle(scene), pointer);

    registry.InvalidateHandle(LN_DenseHandleKind::Object, pointer);
    EXPECT_EQ(registry.ResolveHandle(object), nullptr);
    EXPECT_FALSE(registry.FindObjectHandle(pointer).IsValid());
    EXPECT_EQ(registry.ResolveHandle(scene), pointer);
  }
}

TEST(LN_DenseIds, ReaddedPointerGetsNewGenerationAndOldHandleStaysStale)
{
  for (const uint32_t extra_objects : {0u, 100u}) {
    LN_DenseIdRegistry registry;
    for (uint32_t i = 0; i < extra_objects; i++) {
      registry.MakeObjectHandle(FakePointer(1000 + i), "Filler");
    }
    void *pointer = FakePointer(1);
    const LN_ObjectHandle first = registry.MakeObjectHandle(pointer, "First");
    registry.InvalidateHandle(LN_DenseHandleKind::Object, pointer);
    /* Invalidating twice or an unknown pointer is harmless. */
    registry.InvalidateHandle(LN_DenseHandleKind::Object, pointer);
    registry.InvalidateHandle(LN_DenseHandleKind::Object, FakePointer(2));

    const LN_ObjectHandle second = registry.MakeObjectHandle(pointer, "Second");
    EXPECT_NE(first.generation, second.generation);
    EXPECT_EQ(registry.ResolveHandle(first), nullptr);
    EXPECT_EQ(registry.DebugName(first), "");
    EXPECT_EQ(registry.ResolveHandle(second), pointer);
    EXPECT_EQ(registry.DebugName(second), "Second");
    const LN_ObjectHandle found = registry.FindObjectHandle(pointer);
    EXPECT_EQ(found.index, second.index);
    EXPECT_EQ(found.generation, second.generation);
  }
}

TEST(LN_DenseIds, ClearSceneLocalStateInvalidatesEveryHandle)
{
  for (const uint32_t count : {3u, 200u}) {
    LN_DenseIdRegistry registry;
    std::vector<LN_ObjectHandle> handles;
    for (uint32_t i = 0; i < count; i++) {
      handles.push_back(registry.MakeObjectHandle(FakePointer(i), "Object"));
    }
    registry.ClearSceneLocalState();
    for (uint32_t i = 0; i < count; i++) {
      EXPECT_EQ(registry.ResolveHandle(handles[i]), nullptr);
      EXPECT_FALSE(registry.FindObjectHandle(FakePointer(i)).IsValid());
    }
    /* Entries are reused, lowest first, and resolve again. */
    const LN_ObjectHandle again = registry.MakeObjectHandle(FakePointer(7), "Again");
    EXPECT_EQ(again.index, 0u);
    EXPECT_EQ(registry.ResolveHandle(again), FakePointer(7));
    EXPECT_EQ(registry.ResolveHandle(handles[0]), nullptr);
  }
}

TEST(LN_DenseIds, RandomOperationsMatchReferenceModel)
{
  /* Grows past the scanned size into the hash index, churns, clears and shrinks again, checking
   * every live and stale handle against a plain map. */
  const LN_DenseHandleKind kinds[] = {
      LN_DenseHandleKind::Object, LN_DenseHandleKind::Scene, LN_DenseHandleKind::Datablock};
  std::mt19937 rng(20260930);
  LN_DenseIdRegistry registry;
  std::map<std::pair<void *, LN_DenseHandleKind>, LN_DenseHandle> live;
  std::vector<LN_DenseHandle> stale;

  auto check_all = [&]() {
    for (const auto &[key, handle] : live) {
      ASSERT_EQ(registry.ResolveHandle(handle), key.first);
      const LN_DenseHandle again = MakeHandleOfKind(registry, key.second, key.first);
      ASSERT_EQ(again.index, handle.index);
      ASSERT_EQ(again.generation, handle.generation);
      if (key.second == LN_DenseHandleKind::Object) {
        const LN_ObjectHandle found = registry.FindObjectHandle(key.first);
        ASSERT_EQ(found.index, handle.index);
        ASSERT_EQ(found.generation, handle.generation);
      }
    }
    for (const LN_DenseHandle &handle : stale) {
      ASSERT_EQ(registry.ResolveHandle(handle), nullptr);
    }
  };

  for (int step = 0; step < 20000; step++) {
    /* Phases: grow to ~300 live handles, churn, then shrink to a handful. */
    const size_t target = step < 6000 ? 300 : (step < 14000 ? 150 : 3);
    const uint32_t roll = rng() % 100;
    if (step % 5000 == 4999) {
      registry.ClearSceneLocalState();
      for (const auto &[key, handle] : live) {
        stale.push_back(handle);
      }
      live.clear();
    }
    else if (live.size() < target ? roll < 70 : roll < 30) {
      void *pointer = FakePointer(rng() % 600);
      const LN_DenseHandleKind kind = kinds[rng() % 3];
      const LN_DenseHandle handle = MakeHandleOfKind(registry, kind, pointer);
      ASSERT_TRUE(handle.IsValid());
      const auto [iter, inserted] = live.try_emplace({pointer, kind}, handle);
      if (!inserted) {
        ASSERT_EQ(iter->second.index, handle.index);
        ASSERT_EQ(iter->second.generation, handle.generation);
      }
    }
    else if (!live.empty()) {
      auto iter = live.begin();
      std::advance(iter, rng() % live.size());
      registry.InvalidateHandle(iter->first.second, iter->first.first);
      stale.push_back(iter->second);
      live.erase(iter);
    }
    if (stale.size() > 2000) {
      stale.erase(stale.begin(), stale.begin() + 1000);
    }
    if (step % 97 == 0) {
      check_all();
    }
  }
  check_all();
}

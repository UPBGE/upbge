/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "KX_OverlayResource.h"

namespace {

blender::Collection *FakeCollection(const uintptr_t id)
{
  return reinterpret_cast<blender::Collection *>(id);
}

TEST(KX_OverlayResource, IndependentAndLeaseRequestsShareOneEntry)
{
  KX_OverlayResourceRegistry registry;
  blender::Collection *hud = FakeCollection(11);
  blender::Collection *menu = FakeCollection(12);

  EXPECT_EQ(registry.ValidateIndependentEnable(hud), KX_OverlayCollectionResult::Success);
  registry.Create(hud);
  registry.SetIndependentEnabled(hud, true);
  EXPECT_TRUE(registry.ShouldExist(hud));
  EXPECT_TRUE(registry.ShouldRender(hud));
  EXPECT_EQ(registry.ValidateIndependentEnable(hud), KX_OverlayCollectionResult::NoChange);

  registry.Create(menu);
  registry.AddLease(menu, 1, true);
  EXPECT_TRUE(registry.ShouldExist(menu));
  EXPECT_TRUE(registry.HasLease(menu, 1));
  EXPECT_FALSE(registry.IndependentEnabled(menu));

  registry.SetIndependentEnabled(menu, true);
  registry.SetLeaseVisible(menu, 1, false);
  EXPECT_TRUE(registry.ShouldRender(menu));

  registry.SetIndependentEnabled(menu, false);
  EXPECT_FALSE(registry.ShouldRender(menu));
  EXPECT_TRUE(registry.ShouldExist(menu));

  registry.RemoveLease(menu, 1);
  EXPECT_FALSE(registry.ShouldExist(menu));
  EXPECT_EQ(registry.ValidateIndependentDisable(hud), KX_OverlayCollectionResult::Success);
  registry.SetIndependentEnabled(hud, false);
  EXPECT_FALSE(registry.ShouldExist(hud));
}

TEST(KX_OverlayResource, LastLeaseWithoutIndependentRequestIsDestroyed)
{
  KX_OverlayResourceRegistry registry;
  blender::Collection *hud = FakeCollection(51);
  blender::Collection *menu = FakeCollection(52);
  registry.Create(hud);
  registry.SetIndependentEnabled(hud, true);
  registry.AddLease(hud, 3, true);
  EXPECT_TRUE(registry.WouldExistWithoutLease(hud, 3));

  registry.Create(menu);
  registry.AddLease(menu, 1, true);
  registry.AddLease(menu, 2, true);
  EXPECT_TRUE(registry.WouldExistWithoutLease(menu, 1));
  registry.RemoveLease(menu, 2);
  EXPECT_FALSE(registry.WouldExistWithoutLease(menu, 1));
}

TEST(KX_OverlayResource, CreatorVisibilityAndMembershipStayIndependentOfLeases)
{
  KX_OverlayResourceRegistry registry;
  blender::Collection *collection = FakeCollection(21);
  registry.Create(collection);
  registry.SetIndependentEnabled(collection, true);
  const uint64_t generation = registry.MembershipGeneration(collection);

  registry.SetCreatorVisible(collection, false);
  EXPECT_TRUE(registry.ShouldExist(collection));
  EXPECT_FALSE(registry.ShouldRender(collection));
  EXPECT_EQ(registry.MembershipGeneration(collection), generation);

  registry.NoteMembershipChanged(collection);
  EXPECT_EQ(registry.MembershipGeneration(collection), generation + 1);

  registry.AddLease(collection, 7, true);
  registry.SetIndependentEnabled(collection, false);
  registry.SetCreatorVisible(collection, true);
  EXPECT_TRUE(registry.ShouldRender(collection));
}

TEST(KX_OverlayResource, ClearingTheLastEntryReleasesTheEstablishedCamera)
{
  KX_OverlayResourceRegistry registry;
  blender::Collection *collection = FakeCollection(31);
  KX_Camera *camera = reinterpret_cast<KX_Camera *>(uintptr_t(41));
  registry.SetEstablishedCamera(camera);
  registry.Create(collection);
  EXPECT_EQ(registry.EstablishedCamera(), camera);
  registry.Erase(collection);
  EXPECT_EQ(registry.EstablishedCamera(), nullptr);
  EXPECT_TRUE(registry.Empty());
}

}  // namespace

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "KX_CameraRenderMemory.h"

namespace {

using KX_CameraRenderMemory::CanShareRenderer;
using KX_CameraRenderMemory::PassSettings;
using KX_CameraRenderMemory::ShadowPoolForSplitView;

constexpr int64_t Window4K = int64_t(3841) * 2161;

TEST(KX_CameraRenderMemory, ShadowPoolFullViewKeepsScenePool)
{
  EXPECT_EQ(ShadowPoolForSplitView(256, Window4K, Window4K), 256);
  EXPECT_EQ(ShadowPoolForSplitView(256, Window4K * 2, Window4K), 256);
  EXPECT_EQ(ShadowPoolForSplitView(512, 0, Window4K), 512);
  EXPECT_EQ(ShadowPoolForSplitView(512, Window4K, 0), 512);
}

TEST(KX_CameraRenderMemory, ShadowPoolSplitBySquareRootOfArea)
{
  /* Two side by side 4K split views (1918 and 1919 pixels wide): 256 * 0.707 = 181, to 192. A
   * half view used 211 of the 330 pages of a full view in the 4K split screen demo (64%). */
  EXPECT_EQ(ShadowPoolForSplitView(256, int64_t(1918) * 2161, Window4K), 192);
  EXPECT_EQ(ShadowPoolForSplitView(256, int64_t(1919) * 2161, Window4K), 192);
  /* Four quarter views: half the pool each. */
  EXPECT_EQ(ShadowPoolForSplitView(1024, Window4K / 4, Window4K), 512);
  /* Whole multiples of 16 MB. */
  EXPECT_EQ(ShadowPoolForSplitView(512, Window4K / 2, Window4K) % 16, 0);
}

TEST(KX_CameraRenderMemory, ShadowPoolStaysInTheScenePool)
{
  EXPECT_EQ(ShadowPoolForSplitView(256, int64_t(557) * 626, Window4K), 64);
  EXPECT_EQ(ShadowPoolForSplitView(16, int64_t(557) * 626, Window4K), 16);
  EXPECT_EQ(ShadowPoolForSplitView(16, Window4K / 2, Window4K), 16);
}

PassSettings HistoryFreeEEVEE()
{
  PassSettings settings;
  settings.width = 557;
  settings.height = 626;
  return settings;
}

TEST(KX_CameraRenderMemory, SharingNeedsAHistoryFreePass)
{
  EXPECT_TRUE(CanShareRenderer(HistoryFreeEEVEE()));

  for (int i = 0; i < 5; i++) {
    PassSettings settings = HistoryFreeEEVEE();
    settings.temporal_reprojection = (i == 0);
    settings.ray_tracing = (i == 1);
    settings.shadows = (i == 2);
    settings.motion_blur = (i == 3);
    settings.depth_of_field = (i == 4);
    EXPECT_FALSE(CanShareRenderer(settings)) << "history source " << i;
  }
}

TEST(KX_CameraRenderMemory, WorkbenchShares)
{
  PassSettings settings = HistoryFreeEEVEE();
  settings.workbench = true;
  /* Workbench ignores the EEVEE switches: the camera's EEVEE effects may still be on. */
  settings.temporal_reprojection = true;
  settings.shadows = true;
  EXPECT_TRUE(CanShareRenderer(settings));
}

TEST(KX_CameraRenderMemory, RenderToTextureNeverShares)
{
  PassSettings settings = HistoryFreeEEVEE();
  settings.image_render = true;
  EXPECT_FALSE(CanShareRenderer(settings));
  settings.workbench = true;
  EXPECT_FALSE(CanShareRenderer(settings));
}

TEST(KX_CameraRenderMemory, HiddenObjectsShareOnlyWithWorkbench)
{
  /* Set Camera Visibility or a Collection setting: EEVEE keeps its own renderer, Workbench (which
   * builds its draw from the objects at every pass) may share. */
  PassSettings settings = HistoryFreeEEVEE();
  settings.hides_objects = true;
  EXPECT_FALSE(CanShareRenderer(settings));
  settings.workbench = true;
  EXPECT_TRUE(CanShareRenderer(settings));
  settings.image_render = true;
  EXPECT_FALSE(CanShareRenderer(settings));
}

TEST(KX_CameraRenderMemory, EqualSettingsShareOneRenderer)
{
  PassSettings a = HistoryFreeEEVEE();
  PassSettings b = HistoryFreeEEVEE();
  EXPECT_EQ(a, b);
  b.width = 558;
  EXPECT_FALSE(a == b);
  b = a;
  b.overlay = true;
  EXPECT_FALSE(a == b);
  b = a;
  b.eevee_flag = 1;
  EXPECT_FALSE(a == b);
  b = a;
  b.workbench = true;
  EXPECT_FALSE(a == b);
}

}  // namespace

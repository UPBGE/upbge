/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <vector>

#include "KX_PlayerViews.h"
#include "RAS_FramingManager.h"

namespace {

using Area = KX_PlayerViews::Area;
using Layout = KX_PlayerViews::Layout;
using FieldOfView = KX_PlayerViews::FieldOfView;

/** The views never dereference cameras in these tests. */
KX_Camera *FakeCamera(const uintptr_t id)
{
  return reinterpret_cast<KX_Camera *>(id);
}

void ExpectArea(const Area &area, float left, float top, float width, float height)
{
  EXPECT_FLOAT_EQ(area.left, left);
  EXPECT_FLOAT_EQ(area.top, top);
  EXPECT_FLOAT_EQ(area.width, width);
  EXPECT_FLOAT_EQ(area.height, height);
}

TEST(KX_PlayerViews, LayoutsForTwoToFourPlayers)
{
  Area areas[KX_PlayerViews::MaxPlayers];
  Layout layout;

  ASSERT_EQ(KX_PlayerViews::ComputeAreas(1, 1.78f, layout, areas), 1);
  ExpectArea(areas[0], 0.0f, 0.0f, 1.0f, 1.0f);

  /* Auto: side by side on 16:10 and wider, top and bottom on narrower windows. */
  ASSERT_EQ(KX_PlayerViews::ComputeAreas(2, 1.78f, layout, areas), 2);
  ExpectArea(areas[0], 0.0f, 0.0f, 0.5f, 1.0f);
  ExpectArea(areas[1], 0.5f, 0.0f, 0.5f, 1.0f);
  KX_PlayerViews::ComputeAreas(2, 1.6f, layout, areas);
  ExpectArea(areas[1], 0.5f, 0.0f, 0.5f, 1.0f);
  KX_PlayerViews::ComputeAreas(2, 1.33f, layout, areas);
  ExpectArea(areas[0], 0.0f, 0.0f, 1.0f, 0.5f);
  ExpectArea(areas[1], 0.0f, 0.5f, 1.0f, 0.5f);

  layout.two_players = KX_PlayerViews::TwoPlayers::TopAndBottom;
  KX_PlayerViews::ComputeAreas(2, 1.78f, layout, areas);
  ExpectArea(areas[1], 0.0f, 0.5f, 1.0f, 0.5f);
  layout.two_players = KX_PlayerViews::TwoPlayers::SideBySide;
  KX_PlayerViews::ComputeAreas(2, 1.0f, layout, areas);
  ExpectArea(areas[1], 0.5f, 0.0f, 0.5f, 1.0f);

  ASSERT_EQ(KX_PlayerViews::ComputeAreas(3, 1.78f, layout, areas), 3);
  ExpectArea(areas[0], 0.0f, 0.0f, 1.0f, 0.5f);
  ExpectArea(areas[1], 0.0f, 0.5f, 0.5f, 0.5f);
  ExpectArea(areas[2], 0.5f, 0.5f, 0.5f, 0.5f);
  layout.three_players = KX_PlayerViews::ThreePlayers::FavorBottom;
  KX_PlayerViews::ComputeAreas(3, 1.78f, layout, areas);
  ExpectArea(areas[0], 0.0f, 0.0f, 0.5f, 0.5f);
  ExpectArea(areas[1], 0.5f, 0.0f, 0.5f, 0.5f);
  ExpectArea(areas[2], 0.0f, 0.5f, 1.0f, 0.5f);

  ASSERT_EQ(KX_PlayerViews::ComputeAreas(4, 1.78f, layout, areas), 4);
  ExpectArea(areas[0], 0.0f, 0.0f, 0.5f, 0.5f);
  ExpectArea(areas[1], 0.5f, 0.0f, 0.5f, 0.5f);
  ExpectArea(areas[2], 0.0f, 0.5f, 0.5f, 0.5f);
  ExpectArea(areas[3], 0.5f, 0.5f, 0.5f, 0.5f);
}

TEST(KX_PlayerViews, MoreThanFourPlayersShareAGrid)
{
  Area areas[KX_PlayerViews::MaxPlayers];
  const Layout layout;
  /* 5 and 6 players: 3 columns, 2 rows. 7 and 8: 3 by 3. */
  ASSERT_EQ(KX_PlayerViews::ComputeAreas(5, 1.78f, layout, areas), 5);
  ExpectArea(areas[3], 0.0f, 0.5f, 1.0f / 3.0f, 0.5f);
  ExpectArea(areas[4], 1.0f / 3.0f, 0.5f, 1.0f / 3.0f, 0.5f);
  ASSERT_EQ(KX_PlayerViews::ComputeAreas(8, 1.78f, layout, areas), 8);
  ExpectArea(areas[7], 1.0f / 3.0f, 2.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
  /* Never more views than players. */
  EXPECT_EQ(KX_PlayerViews::ComputeAreas(12, 1.78f, layout, areas), 8);
}

/** How many views draw each pixel of the window (inclusive edges, like camera viewports). */
std::vector<int> Coverage(const std::vector<RAS_Rect> &rects, const RAS_Rect &window)
{
  const int width = window.GetWidth() + 1;
  std::vector<int> counts(size_t(width * (window.GetHeight() + 1)), 0);
  for (const RAS_Rect &rect : rects) {
    for (int y = rect.GetBottom(); y <= rect.GetTop(); ++y) {
      for (int x = rect.GetLeft(); x <= rect.GetRight(); ++x) {
        counts[size_t(y * width + x)]++;
      }
    }
  }
  return counts;
}

TEST(KX_PlayerViews, ViewsCoverOddWindowsWithoutGapsOrOverlaps)
{
  Layout layout;
  for (const RAS_Rect window : {RAS_Rect(333, 211), RAS_Rect(160, 90), RAS_Rect(101, 101)}) {
    for (int count = 2; count <= KX_PlayerViews::MaxPlayers; ++count) {
      if (count == 5 || count >= 7) {
        continue; /* Their grids leave cells empty. */
      }
      for (const auto two : {KX_PlayerViews::TwoPlayers::SideBySide,
                             KX_PlayerViews::TwoPlayers::TopAndBottom})
      {
        layout.two_players = two;
        Area areas[KX_PlayerViews::MaxPlayers];
        const int views = KX_PlayerViews::ComputeAreas(count, 1.5f, layout, areas);
        std::vector<RAS_Rect> rects;
        for (int i = 0; i < views; ++i) {
          rects.push_back(KX_PlayerViews::AreaToPixels(areas[i], window, 0));
        }
        const std::vector<int> counts = Coverage(rects, window);
        for (const int covered : counts) {
          ASSERT_EQ(covered, 1) << count << " views in " << window;
        }
        /* The fractions given back add up to the whole window. */
        float total = 0.0f;
        for (const RAS_Rect &rect : rects) {
          const Area area = KX_PlayerViews::PixelsToArea(rect, window);
          total += area.width * area.height;
        }
        EXPECT_NEAR(total, 1.0f, 1.0e-5f);
      }
    }
  }
}

TEST(KX_PlayerViews, DividerLeavesExactPixelsBetweenViewsOnly)
{
  const RAS_Rect window(1600, 900);
  const Layout layout;
  Area areas[KX_PlayerViews::MaxPlayers];
  KX_PlayerViews::ComputeAreas(4, 1.78f, layout, areas);
  for (const int divider : {1, 2, 5}) {
    const RAS_Rect top_left = KX_PlayerViews::AreaToPixels(areas[0], window, divider);
    const RAS_Rect top_right = KX_PlayerViews::AreaToPixels(areas[1], window, divider);
    const RAS_Rect bottom_left = KX_PlayerViews::AreaToPixels(areas[2], window, divider);
    /* Window borders stay where they are. */
    EXPECT_EQ(top_left.GetLeft(), 0);
    EXPECT_EQ(top_left.GetTop(), 900);
    EXPECT_EQ(top_right.GetRight(), 1600);
    EXPECT_EQ(bottom_left.GetBottom(), 0);
    /* Empty pixels between neighbors: exactly the divider. */
    EXPECT_EQ(top_right.GetLeft() - top_left.GetRight() - 1, divider);
    EXPECT_EQ(top_left.GetBottom() - bottom_left.GetTop() - 1, divider);
  }
  /* A divider wider than a view still leaves it a pixel: its projection never divides by 0. */
  const RAS_Rect tiny(40, 30);
  const RAS_Rect squeezed = KX_PlayerViews::AreaToPixels(areas[1], tiny, 100);
  EXPECT_GE(squeezed.GetWidth(), 1);
  EXPECT_GE(squeezed.GetHeight(), 1);
}

TEST(KX_PlayerViews, CropKeepsTheSideThatWasNotSplit)
{
  const RAS_Rect window(1600, 900);
  RAS_FrameFrustum full;
  full.x1 = -1.6f;
  full.x2 = 1.6f;
  full.y1 = -0.9f;
  full.y2 = 0.9f;

  /* Side by side: same height, the middle half of the width. */
  RAS_FrameFrustum frustum = full;
  KX_PlayerViews::FitFrustumToView(FieldOfView::Crop, window, RAS_Rect(0, 0, 800, 900), frustum);
  EXPECT_FLOAT_EQ(frustum.x1, -0.8f);
  EXPECT_FLOAT_EQ(frustum.x2, 0.8f);
  EXPECT_FLOAT_EQ(frustum.y1, -0.9f);
  EXPECT_FLOAT_EQ(frustum.y2, 0.9f);

  /* Top and bottom: same width, the middle band. */
  frustum = full;
  KX_PlayerViews::FitFrustumToView(FieldOfView::Crop, window, RAS_Rect(0, 450, 1600, 900), frustum);
  EXPECT_FLOAT_EQ(frustum.x1, -1.6f);
  EXPECT_FLOAT_EQ(frustum.y1, -0.45f);
  EXPECT_FLOAT_EQ(frustum.y2, 0.45f);

  /* A grid cell has the window's shape: the whole picture. */
  frustum = full;
  KX_PlayerViews::FitFrustumToView(FieldOfView::Crop, window, RAS_Rect(0, 0, 800, 450), frustum);
  EXPECT_FLOAT_EQ(frustum.x2, 1.6f);
  EXPECT_FLOAT_EQ(frustum.y2, 0.9f);

  /* Keep Vertical on a top and bottom view: same height, twice as wide. */
  frustum = full;
  KX_PlayerViews::FitFrustumToView(
      FieldOfView::KeepVertical, window, RAS_Rect(0, 450, 1600, 900), frustum);
  EXPECT_FLOAT_EQ(frustum.x2, 3.2f);
  EXPECT_FLOAT_EQ(frustum.y2, 0.9f);

  /* Keep Horizontal on a side by side view: same width, twice as tall. */
  frustum = full;
  KX_PlayerViews::FitFrustumToView(
      FieldOfView::KeepHorizontal, window, RAS_Rect(0, 0, 800, 900), frustum);
  EXPECT_FLOAT_EQ(frustum.x2, 1.6f);
  EXPECT_FLOAT_EQ(frustum.y2, 1.8f);

  /* Fit leaves the frustum alone. */
  frustum = full;
  KX_PlayerViews::FitFrustumToView(FieldOfView::Fit, window, RAS_Rect(0, 0, 800, 900), frustum);
  EXPECT_FLOAT_EQ(frustum.x2, 1.6f);

  /* A lens shift keeps its place: the view is cut around the shifted center. */
  frustum = full;
  frustum.x1 += 0.4f;
  frustum.x2 += 0.4f;
  KX_PlayerViews::FitFrustumToView(FieldOfView::Crop, window, RAS_Rect(0, 0, 800, 900), frustum);
  EXPECT_FLOAT_EQ(frustum.x1, -0.4f);
  EXPECT_FLOAT_EQ(frustum.x2, 1.2f);
}

TEST(KX_PlayerViews, PlayersWithACameraAreShownInPlayerOrder)
{
  KX_PlayerViews views;
  KX_Camera *active = FakeCamera(10);
  const RAS_Rect window(1600, 900);
  Area area;

  EXPECT_EQ(views.GetViewCount(active), 1);
  ASSERT_TRUE(views.GetPlayerArea(0, active, window, area));
  ExpectArea(area, 0.0f, 0.0f, 1.0f, 1.0f);
  EXPECT_FALSE(views.GetPlayerArea(2, active, window, area));

  /* Player 2 joins: players 0 and 2 split the window, player 1 has no view. */
  views.SetPlayerCamera(2, FakeCamera(12));
  EXPECT_EQ(views.GetViewCount(active), 2);
  ASSERT_TRUE(views.GetPlayerArea(2, active, window, area));
  ExpectArea(area, 0.5f, 0.0f, 0.5f, 1.0f);
  EXPECT_FALSE(views.GetPlayerArea(1, active, window, area));
  EXPECT_EQ(views.GetPlayerCamera(2, active), FakeCamera(12));
  EXPECT_EQ(views.GetPlayerCamera(0, active), active);

  /* One camera draws one view: the first player using it keeps it. */
  views.SetPlayerCamera(3, FakeCamera(12));
  EXPECT_EQ(views.GetViewCount(active), 2);
  EXPECT_FALSE(views.GetPlayerArea(3, active, window, area));

  /* Split screen off: one full view of player 0. */
  Layout layout;
  layout.enabled = false;
  views.SetLayout(layout);
  EXPECT_EQ(views.GetViewCount(active), 1);
  EXPECT_FALSE(views.GetPlayerArea(2, active, window, area));
  layout.enabled = true;
  views.SetLayout(layout);

  /* A removed camera drops its player's view. */
  views.RemoveObject(reinterpret_cast<KX_GameObject *>(FakeCamera(12)));
  EXPECT_EQ(views.GetViewCount(active), 1);
  EXPECT_EQ(views.GetPlayerCamera(2, active), nullptr);

  /* Without an active camera, the other players still share the window. */
  views.SetPlayerCamera(1, FakeCamera(11));
  views.SetPlayerCamera(4, FakeCamera(14));
  EXPECT_EQ(views.GetViewCount(nullptr), 2);
  ASSERT_TRUE(views.GetPlayerArea(4, nullptr, window, area));
  ExpectArea(area, 0.5f, 0.0f, 0.5f, 1.0f);

  /* One view drawn in a custom viewport: the player's view is that area. */
  views.SetPlayerCamera(1, nullptr);
  views.SetPlayerCamera(4, nullptr);
  Area corner;
  corner.left = 0.5f;
  corner.top = 0.25f;
  corner.width = 0.5f;
  corner.height = 0.5f;
  views.SetCameraViewport(active, true, corner, true);
  ASSERT_TRUE(views.GetPlayerArea(0, active, window, area));
  ExpectArea(area, 0.5f, 0.25f, 0.5f, 0.5f);
  views.SetCameraViewport(active, false, corner, true);
  ASSERT_TRUE(views.GetPlayerArea(0, active, window, area));
  ExpectArea(area, 0.0f, 0.0f, 1.0f, 1.0f);
  /* A viewport without a size is off (no full pass for nothing). */
  corner.width = 0.0f;
  views.SetCameraViewport(active, true, corner, true);
  ASSERT_TRUE(views.GetPlayerArea(0, active, window, area));
  ExpectArea(area, 0.0f, 0.0f, 1.0f, 1.0f);

  /* Player numbers outside 1 to 7 are ignored. */
  views.SetPlayerCamera(0, FakeCamera(20));
  views.SetPlayerCamera(8, FakeCamera(21));
  EXPECT_EQ(views.GetPlayerCamera(0, active), active);
  EXPECT_EQ(views.GetPlayerCamera(8, active), nullptr);
}

TEST(KX_PlayerViews, SplitViewsAreThePlayersCamerasWhileTheWindowIsSplit)
{
  KX_PlayerViews views;
  KX_Camera *active = FakeCamera(10);
  KX_Camera *minimap = FakeCamera(30);
  views.SetCameraViewport(minimap, true, Area{0.7f, 0.0f, 0.3f, 0.3f}, true);

  /* One player: nothing is split. */
  EXPECT_FALSE(views.IsSplitView(active, active));
  views.SetPlayerCamera(1, FakeCamera(11));
  EXPECT_TRUE(views.IsSplitView(active, active));
  EXPECT_TRUE(views.IsSplitView(FakeCamera(11), active));
  /* A camera viewport (a minimap) is no player's view. */
  EXPECT_FALSE(views.IsSplitView(minimap, active));
  EXPECT_FALSE(views.IsSplitView(FakeCamera(12), active));

  Layout layout;
  layout.enabled = false;
  views.SetLayout(layout);
  EXPECT_FALSE(views.IsSplitView(active, active));
  EXPECT_FALSE(views.IsSplitView(FakeCamera(11), active));
}

TEST(KX_PlayerViews, TheMouseIsPlayerZerosOnceTheWindowIsSplit)
{
  KX_PlayerViews views;
  KX_Camera *active = FakeCamera(10);
  const RAS_Rect window(1600, 900);
  RAS_Rect view;
  /* One view: the mouse has the whole window. */
  EXPECT_FALSE(views.GetMouseView(active, window, view));

  /* Side by side with a 4 pixel divider: player 0's left half, up to the divider. */
  KX_PlayerViews::Layout layout;
  layout.two_players = KX_PlayerViews::TwoPlayers::SideBySide;
  layout.divider = 4;
  views.SetLayout(layout);
  views.SetPlayerCamera(1, FakeCamera(11));
  ASSERT_TRUE(views.GetMouseView(active, window, view));
  EXPECT_EQ(view.GetLeft(), 0);
  EXPECT_EQ(view.GetRight(), 797);
  EXPECT_EQ(view.GetBottom(), 0);
  EXPECT_EQ(view.GetTop(), 900);
  EXPECT_TRUE(views.KeepsMouse());

  /* Player 0 without a camera (only gamepad players): the mouse picks any view. */
  EXPECT_FALSE(views.GetMouseView(nullptr, window, view));
  /* Split screen turned off: one view again. */
  layout.enabled = false;
  views.SetLayout(layout);
  EXPECT_FALSE(views.GetMouseView(active, window, view));
  EXPECT_FALSE(views.GetMouseView(active, RAS_Rect(), view));
}

TEST(KX_PlayerViews, CircleViewportIsTheOvalFillingTheDrawnViewport)
{
  /* Inclusive edges 100..200 and 50..150: drawn 101 pixels wide and tall, the circle's center at
   * (150.5, 100.5) with a radius of 50.5. Pixels count by their center. */
  const RAS_Rect square(100, 50, 200, 150);
  EXPECT_TRUE(KX_PlayerViews::InsideViewportCircle(square, 150.0f, 100.0f));
  /* The middle of each edge is inside, the corners and the pixels past an edge are not. */
  EXPECT_TRUE(KX_PlayerViews::InsideViewportCircle(square, 100.0f, 100.0f));
  EXPECT_TRUE(KX_PlayerViews::InsideViewportCircle(square, 200.0f, 100.0f));
  EXPECT_TRUE(KX_PlayerViews::InsideViewportCircle(square, 150.0f, 150.0f));
  EXPECT_FALSE(KX_PlayerViews::InsideViewportCircle(square, 100.0f, 50.0f));
  EXPECT_FALSE(KX_PlayerViews::InsideViewportCircle(square, 200.0f, 150.0f));
  EXPECT_FALSE(KX_PlayerViews::InsideViewportCircle(square, 99.0f, 100.0f));
  /* On the diagonal: 35 pixels from the center each way is inside, 36 is out. */
  EXPECT_TRUE(KX_PlayerViews::InsideViewportCircle(square, 185.0f, 135.0f));
  EXPECT_FALSE(KX_PlayerViews::InsideViewportCircle(square, 186.0f, 136.0f));

  /* A viewport four times wider than tall: an oval touching its four edges. */
  const RAS_Rect wide(0, 0, 399, 99);
  EXPECT_TRUE(KX_PlayerViews::InsideViewportCircle(wide, 0.0f, 50.0f));
  EXPECT_TRUE(KX_PlayerViews::InsideViewportCircle(wide, 200.0f, 0.0f));
  EXPECT_FALSE(KX_PlayerViews::InsideViewportCircle(wide, 0.0f, 0.0f));
  EXPECT_TRUE(KX_PlayerViews::InsideViewportCircle(wide, 300.0f, 90.0f));
  EXPECT_FALSE(KX_PlayerViews::InsideViewportCircle(wide, 300.0f, 95.0f));

  /* An empty viewport has no circle. */
  EXPECT_FALSE(KX_PlayerViews::InsideViewportCircle(RAS_Rect(10, 10, 9, 9), 10.0f, 10.0f));
}

}  // namespace

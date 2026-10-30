/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_PlayerViews.h
 *  \ingroup ketsji
 *
 * Split screen. Player 0 sees the game through the scene's active camera, players 1 to 7 through
 * the camera Set Camera gave them. The players that have a camera share the window in a layout
 * (side by side, top and bottom, a grid). Layouts are window fractions, turned into camera
 * viewports right before each frame is drawn, so they follow window resizes. Custom viewports
 * (minimaps, mirrors) are placed the same way.
 *
 * Only the cameras placed here are changed, and each gets back the viewport it had before (none,
 * or one set from Python) when it is no longer placed. Nothing here draws; each camera with a
 * viewport is drawn by the usual camera passes.
 */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "RAS_Rect.h"

class KX_Camera;
class KX_GameObject;
class KX_Scene;
struct RAS_FrameFrustum;

class KX_PlayerViews {
 public:
  static constexpr int MaxPlayers = 8;

  enum class TwoPlayers : uint8_t { Auto = 0, SideBySide = 1, TopAndBottom = 2 };
  enum class ThreePlayers : uint8_t { FavorTop = 0, FavorBottom = 1 };
  /** What a split view shows of the picture its camera has over the whole window. */
  enum class FieldOfView : uint8_t {
    /** Keep the field of view across the side that was not split: the middle of the picture. */
    Crop = 0,
    /** The camera's own Sensor Fit for the view's shape, like a viewport set from Python. */
    Fit = 1,
    KeepVertical = 2,
    KeepHorizontal = 3,
  };

  struct Layout {
    /** Off: one full view of player 0. */
    bool enabled = true;
    TwoPlayers two_players = TwoPlayers::Auto;
    ThreePlayers three_players = ThreePlayers::FavorTop;
    FieldOfView field_of_view = FieldOfView::Crop;
    /** Gap between views in pixels; it shows the scene's framing bar color. */
    int divider = 0;
    /** The mouse cursor is moved back into player 0's view (KX_KetsjiEngine). */
    bool keep_mouse = true;
  };

  /** A part of the window in fractions, origin at the top left. */
  struct Area {
    float left = 0.0f;
    float top = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
  };

  /** Player 1 to 7's camera; null removes that player's view. */
  void SetPlayerCamera(int player, KX_Camera *camera);
  /** Player 0's camera is the active camera. */
  KX_Camera *GetPlayerCamera(int player, KX_Camera *active_camera) const;
  void SetLayout(const Layout &layout);
  /** `area` is a part of the window, or of `player`'s view when player >= 0 (a minimap per player:
   * not drawn while that player has no view). */
  void SetCameraViewport(
      KX_Camera *camera, bool enabled, const Area &area, bool on_top, int player = -1);
  /** Forgets an object being removed from the scene (compares the pointer only). */
  void RemoveObject(const KX_GameObject *object);

  /** The player's view in this window, in fractions; false when it is not shown. */
  bool GetPlayerArea(int player,
                     KX_Camera *active_camera,
                     const RAS_Rect &window,
                     Area &r_area) const;
  /** How many player views are shown. */
  int GetViewCount(KX_Camera *active_camera) const;
  /** The camera draws a player's view and the window is split (two views or more). */
  bool IsSplitView(const KX_Camera *camera, KX_Camera *active_camera) const;
  /** While the window is split and player 0 has a view: that view in this window's pixels
   * (inclusive edges, as AreaToPixels). The mouse is player 0's: it reaches only this view. */
  bool GetMouseView(KX_Camera *active_camera, const RAS_Rect &window, RAS_Rect &r_view) const;
  /** Set Split Screen's Keep Mouse in View input. */
  bool KeepsMouse() const
  {
    return m_layout.keep_mouse;
  }
  /** The shown players and their cameras, in player order; returns how many. */
  int CollectViews(KX_Camera *active_camera,
                   std::array<int, MaxPlayers> &r_players,
                   std::array<KX_Camera *, MaxPlayers> &r_cameras) const;

  /** Gives each camera its viewport for the coming frame; cheap when nothing changed. Without
   * draw_views (Use Viewport Render, which draws one camera) every camera gets its own back. */
  void Apply(KX_Scene &scene, const RAS_Rect &window, bool draw_views);
  /** The window the views were last placed in (empty before the first frame). */
  const RAS_Rect &GetWindow() const
  {
    return m_window;
  }
  /** Fit unless the camera is drawn as a split player view this frame. */
  FieldOfView GetFieldOfView(const KX_Camera *camera) const;

  /* The maths, unit tested. */

  /** The areas of `count` views in player order. Returns how many were written (at most 8). */
  static int ComputeAreas(int count, float window_aspect, const Layout &layout, Area *r_areas);
  /** The camera viewport (inclusive pixel edges) of an area; inner edges leave `divider` pixels
   * between neighbors. Neighbors share their rounded edge, so there is never a gap or overlap. */
  static RAS_Rect AreaToPixels(const Area &area, const RAS_Rect &window, int divider);
  /** The area a camera viewport covers, in fractions (inverse of AreaToPixels without divider). */
  static Area PixelsToArea(const RAS_Rect &rect, const RAS_Rect &window);
  /** Pixel (x, y) (window pixels from the bottom left) is inside the circle, or oval, filling a
   * camera viewport with the Circle shape. The viewport has inclusive edges: it is drawn one
   * pixel wider and taller than its width and height (KX_Scene::RenderAfterCameraSetup). */
  static bool InsideViewportCircle(const RAS_Rect &viewport, float x, float y);
  /** Turns the frustum a camera has over the whole window into the one of its view. */
  static void FitFrustumToView(FieldOfView field_of_view,
                               const RAS_Rect &window,
                               const RAS_Rect &view,
                               RAS_FrameFrustum &frustum);

 private:
  struct CustomViewport {
    KX_Camera *camera;
    Area area;
    bool on_top;
    /** -1: the area is a part of the window; else of this player's view. */
    int player = -1;
  };
  struct PlacedCamera {
    KX_Camera *camera;
    FieldOfView field_of_view;
    /** The viewport the camera had before it was placed, given back with it. */
    bool had_viewport;
    RAS_Rect previous;
  };


  /** Players 1 to 7 (index 0 unused: player 0 is the active camera). */
  std::array<KX_Camera *, MaxPlayers> m_playerCameras = {};
  Layout m_layout;
  std::vector<CustomViewport> m_customViewports;
  /** The cameras whose viewport Apply turned on, and their field of view rule. */
  std::vector<PlacedCamera> m_placed;
  /** Apply's next m_placed, kept to avoid an allocation each frame. */
  std::vector<PlacedCamera> m_nextPlaced;
  /** Use Viewport Render: said once that the views are not drawn. */
  bool m_viewportRenderWarned = false;
  /** False while Use Viewport Render draws the active camera alone: player 0's view is then the
   * only one shown, over the whole window. */
  bool m_drawViews = true;
  /** The window of the last Apply: Crop depends on its shape. */
  RAS_Rect m_window;
};

/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file gameengine/Ketsji/KX_PlayerViews.cpp
 *  \ingroup ketsji
 */

#include "KX_PlayerViews.h"

#include <algorithm>
#include <cmath>

#include "CM_Message.h"
#include "EXP_ListValue.h"
#include "KX_Camera.h"
#include "KX_Scene.h"
#include "RAS_FramingManager.h"

void KX_PlayerViews::SetPlayerCamera(const int player, KX_Camera *camera)
{
  if (player >= 1 && player < MaxPlayers) {
    m_playerCameras[player] = camera;
  }
}

KX_Camera *KX_PlayerViews::GetPlayerCamera(const int player, KX_Camera *active_camera) const
{
  if (player == 0) {
    return active_camera;
  }
  return (player >= 1 && player < MaxPlayers) ? m_playerCameras[player] : nullptr;
}

void KX_PlayerViews::SetLayout(const Layout &layout)
{
  m_layout = layout;
  m_layout.divider = std::clamp(layout.divider, 0, 100);
}

void KX_PlayerViews::SetCameraViewport(KX_Camera *camera,
                                       const bool enabled,
                                       const Area &area,
                                       const bool on_top,
                                       const int player)
{
  if (camera == nullptr) {
    return;
  }
  auto entry = std::find_if(m_customViewports.begin(),
                            m_customViewports.end(),
                            [camera](const CustomViewport &custom) { return custom.camera == camera; });
  Area clamped;
  clamped.left = std::clamp(area.left, 0.0f, 1.0f);
  clamped.top = std::clamp(area.top, 0.0f, 1.0f);
  clamped.width = std::clamp(area.width, 0.0f, 1.0f - clamped.left);
  clamped.height = std::clamp(area.height, 0.0f, 1.0f - clamped.top);
  /* A viewport without a size would cost a full pass for nothing: it is off. */
  if (!enabled || clamped.width <= 0.0f || clamped.height <= 0.0f) {
    if (entry != m_customViewports.end()) {
      m_customViewports.erase(entry);
    }
    return;
  }
  /* A number that is no player's (8 or more) has no view: Apply never draws it. */
  const int area_player = player >= 0 ? player : -1;
  if (entry != m_customViewports.end()) {
    entry->area = clamped;
    entry->on_top = on_top;
    entry->player = area_player;
  }
  else {
    m_customViewports.push_back({camera, clamped, on_top, area_player});
  }
}

void KX_PlayerViews::RemoveObject(const KX_GameObject *object)
{
  for (KX_Camera *&camera : m_playerCameras) {
    if (static_cast<const KX_GameObject *>(camera) == object) {
      camera = nullptr;
    }
  }
  std::erase_if(m_customViewports, [object](const CustomViewport &custom) {
    return static_cast<const KX_GameObject *>(custom.camera) == object;
  });
  std::erase_if(m_placed, [object](const PlacedCamera &placed) {
    return static_cast<const KX_GameObject *>(placed.camera) == object;
  });
}

int KX_PlayerViews::CollectViews(KX_Camera *active_camera,
                                 std::array<int, MaxPlayers> &r_players,
                                 std::array<KX_Camera *, MaxPlayers> &r_cameras) const
{
  int count = 0;
  const int players = m_layout.enabled && m_drawViews ? MaxPlayers : 1;
  for (int player = 0; player < players; ++player) {
    KX_Camera *camera = GetPlayerCamera(player, active_camera);
    /* A camera can draw only one view: the first player using it gets it. */
    if (camera == nullptr ||
        std::find(r_cameras.begin(), r_cameras.begin() + count, camera) != r_cameras.begin() + count)
    {
      continue;
    }
    r_players[count] = player;
    r_cameras[count] = camera;
    ++count;
  }
  return count;
}

static float window_aspect(const RAS_Rect &window)
{
  return float(window.GetWidth()) / float(std::max(window.GetHeight(), 1));
}

bool KX_PlayerViews::GetPlayerArea(const int player,
                                   KX_Camera *active_camera,
                                   const RAS_Rect &window,
                                   Area &r_area) const
{
  std::array<int, MaxPlayers> players;
  std::array<KX_Camera *, MaxPlayers> cameras;
  const int count = CollectViews(active_camera, players, cameras);
  const int index = int(std::find(players.begin(), players.begin() + count, player) -
                        players.begin());
  if (index >= count) {
    return false;
  }
  if (count == 1) {
    /* One view: the whole window, or the camera's own viewport (Set Camera Viewport). */
    const auto custom = std::find_if(
        m_customViewports.begin(), m_customViewports.end(), [&](const CustomViewport &entry) {
          return entry.camera == cameras[index];
        });
    r_area = m_drawViews && custom != m_customViewports.end() ?
                 PixelsToArea(AreaToPixels(custom->area, window, 0), window) :
                 Area();
    return true;
  }
  std::array<Area, MaxPlayers> areas;
  ComputeAreas(count, window_aspect(window), m_layout, areas.data());
  r_area = PixelsToArea(AreaToPixels(areas[index], window, m_layout.divider), window);
  return true;
}

bool KX_PlayerViews::GetMouseView(KX_Camera *active_camera,
                                  const RAS_Rect &window,
                                  RAS_Rect &r_view) const
{
  std::array<int, MaxPlayers> players;
  std::array<KX_Camera *, MaxPlayers> cameras;
  const int count = CollectViews(active_camera, players, cameras);
  if (count < 2 || players[0] != 0 || window.GetWidth() <= 0 || window.GetHeight() <= 0) {
    return false;
  }
  std::array<Area, MaxPlayers> areas;
  ComputeAreas(count, window_aspect(window), m_layout, areas.data());
  r_view = AreaToPixels(areas[0], window, m_layout.divider);
  return true;
}

int KX_PlayerViews::GetViewCount(KX_Camera *active_camera) const
{
  std::array<int, MaxPlayers> players;
  std::array<KX_Camera *, MaxPlayers> cameras;
  return CollectViews(active_camera, players, cameras);
}

bool KX_PlayerViews::IsSplitView(const KX_Camera *camera, KX_Camera *active_camera) const
{
  std::array<int, MaxPlayers> players;
  std::array<KX_Camera *, MaxPlayers> cameras;
  const int count = CollectViews(active_camera, players, cameras);
  return count >= 2 && std::find(cameras.begin(), cameras.begin() + count, camera) !=
                           cameras.begin() + count;
}

/** Gives a camera back the viewport it had before split screen placed it (none, or one set
 * from Python). */
static void restore_viewport(KX_Camera *camera, const bool had_viewport, const RAS_Rect &previous)
{
  if (had_viewport) {
    camera->SetViewport(
        previous.GetLeft(), previous.GetBottom(), previous.GetRight(), previous.GetTop());
  }
  /* Also recomputes the projection. */
  camera->EnableViewport(had_viewport);
}

void KX_PlayerViews::Apply(KX_Scene &scene, const RAS_Rect &window, const bool draw_views)
{
  std::array<int, MaxPlayers> players;
  std::array<KX_Camera *, MaxPlayers> cameras;
  KX_Camera *active_camera = scene.GetActiveCamera();
  /* The views the game asks for, then whether they are drawn (read by GetPlayerArea & co). */
  m_drawViews = true;
  const int count = CollectViews(active_camera, players, cameras);
  m_drawViews = draw_views;
  const bool window_changed = window.GetLeft() != m_window.GetLeft() ||
                              window.GetBottom() != m_window.GetBottom() ||
                              window.GetRight() != m_window.GetRight() ||
                              window.GetTop() != m_window.GetTop();
  m_window = window;
  /* Only the active camera draws without a viewport: a lone player whose camera isn't the active
   * one (player 0 has none) needs the whole window as its viewport. */
  const bool lone_inactive_view = count == 1 && cameras[0] != active_camera;
  /* A one-player game without custom viewports stops here. */
  if (count < 2 && !lone_inactive_view && m_customViewports.empty() && m_placed.empty()) {
    return;
  }

  if (!draw_views) {
    /* Use Viewport Render draws the first camera only: keep the game's own full view. */
    if (!m_viewportRenderWarned && (count >= 2 || !m_customViewports.empty())) {
      m_viewportRenderWarned = true;
      CM_Warning("Use Viewport Render shows only the active camera: split screen views and "
                 "camera viewports are not drawn. Turn it off (Render tab > Game Resolution) to "
                 "see them.");
    }
    for (const PlacedCamera &before : m_placed) {
      restore_viewport(before.camera, before.had_viewport, before.previous);
    }
    m_placed.clear();
    return;
  }

  KX_Camera *overlay_camera = scene.GetOverlayCamera();
  std::vector<PlacedCamera> &placed = m_nextPlaced;
  placed.clear();
  const auto place = [&](KX_Camera *camera, const RAS_Rect &rect, const FieldOfView field_of_view) {
    /* The menu camera draws over the whole window. */
    if (camera == overlay_camera ||
        std::any_of(placed.begin(), placed.end(), [camera](const PlacedCamera &other) {
          return other.camera == camera;
        }))
    {
      return;
    }
    PlacedCamera entry{camera, field_of_view, camera->GetViewport(), RAS_Rect()};
    const auto before = std::find_if(m_placed.begin(),
                                     m_placed.end(),
                                     [camera](const PlacedCamera &other) {
                                       return other.camera == camera;
                                     });
    if (before != m_placed.end()) {
      entry.had_viewport = before->had_viewport;
      entry.previous = before->previous;
    }
    else if (entry.had_viewport) {
      entry.previous = RAS_Rect(camera->GetViewportLeft(),
                                camera->GetViewportBottom(),
                                camera->GetViewportRight(),
                                camera->GetViewportTop());
    }
    if (!camera->GetViewport() || camera->GetViewportLeft() != rect.GetLeft() ||
        camera->GetViewportBottom() != rect.GetBottom() ||
        camera->GetViewportRight() != rect.GetRight() || camera->GetViewportTop() != rect.GetTop())
    {
      camera->SetViewport(rect.GetLeft(), rect.GetBottom(), rect.GetRight(), rect.GetTop());
      /* Also recomputes the projection. */
      camera->EnableViewport(true);
    }
    else if (window_changed || before == m_placed.end() || before->field_of_view != field_of_view)
    {
      camera->InvalidateProjectionMatrix();
    }
    placed.push_back(entry);
  };

  std::array<Area, MaxPlayers> areas;
  areas.fill(Area());
  if (count >= 2) {
    ComputeAreas(count, window_aspect(window), m_layout, areas.data());
    for (int i = 0; i < count; ++i) {
      place(cameras[i], AreaToPixels(areas[i], window, m_layout.divider), m_layout.field_of_view);
    }
  }
  else if (lone_inactive_view) {
    place(cameras[0], AreaToPixels(Area(), window, 0), FieldOfView::Fit);
  }
  for (const CustomViewport &custom : m_customViewports) {
    Area area = custom.area;
    if (custom.player >= 0) {
      /* A part of that player's view (the whole window while it is the only view); not drawn
       * while the player has none. */
      const int index = int(std::find(players.begin(), players.begin() + count, custom.player) -
                            players.begin());
      if (index >= count) {
        continue;
      }
      const RAS_Rect view = AreaToPixels(areas[index], window, count >= 2 ? m_layout.divider : 0);
      const Area view_area = PixelsToArea(view, window);
      area.left = view_area.left + custom.area.left * view_area.width;
      area.top = view_area.top + custom.area.top * view_area.height;
      area.width = custom.area.width * view_area.width;
      area.height = custom.area.height * view_area.height;
    }
    place(custom.camera, AreaToPixels(area, window, 0), FieldOfView::Fit);
  }

  /* Give back the cameras no longer placed: they draw full screen again when active. */
  for (const PlacedCamera &before : m_placed) {
    if (std::none_of(placed.begin(), placed.end(), [&before](const PlacedCamera &now) {
          return now.camera == before.camera;
        }))
    {
      restore_viewport(before.camera, before.had_viewport, before.previous);
    }
  }
  std::swap(m_placed, m_nextPlaced);

  /* On Top viewports draw after the other cameras (the menu camera still draws last). A camera
   * added later goes to the end of the list: put them back at the end when one went behind. */
  EXP_ListValue<KX_Camera> *camera_list = scene.GetCameraList();
  int tail = camera_list->GetCount();
  bool in_order = true;
  for (auto custom = m_customViewports.rbegin(); custom != m_customViewports.rend(); ++custom) {
    if (!custom->on_top || custom->camera == overlay_camera ||
        !camera_list->SearchValue(custom->camera))
    {
      continue;
    }
    do {
      --tail;
    } while (tail >= 0 && camera_list->GetValue(tail) == overlay_camera);
    if (tail < 0 || camera_list->GetValue(tail) != custom->camera) {
      in_order = false;
      break;
    }
  }
  if (!in_order) {
    for (const CustomViewport &custom : m_customViewports) {
      if (custom.on_top && custom.camera != overlay_camera && camera_list->SearchValue(custom.camera))
      {
        scene.SetCameraOnTop(custom.camera);
      }
    }
  }
}

KX_PlayerViews::FieldOfView KX_PlayerViews::GetFieldOfView(const KX_Camera *camera) const
{
  for (const PlacedCamera &placed : m_placed) {
    if (placed.camera == camera) {
      return placed.field_of_view;
    }
  }
  return FieldOfView::Fit;
}

int KX_PlayerViews::ComputeAreas(const int count,
                                 const float window_aspect,
                                 const Layout &layout,
                                 Area *r_areas)
{
  const auto set = [r_areas](const int i, float left, float top, float width, float height) {
    r_areas[i] = {left, top, width, height};
  };
  switch (count) {
    case 0:
      return 0;
    case 1:
      set(0, 0.0f, 0.0f, 1.0f, 1.0f);
      return 1;
    case 2: {
      /* Auto: side by side on windows 16:10 and wider, as Source engine games do. */
      const bool side_by_side = layout.two_players == TwoPlayers::SideBySide ||
                                (layout.two_players == TwoPlayers::Auto &&
                                 window_aspect >= 1.59f);
      if (side_by_side) {
        set(0, 0.0f, 0.0f, 0.5f, 1.0f);
        set(1, 0.5f, 0.0f, 0.5f, 1.0f);
      }
      else {
        set(0, 0.0f, 0.0f, 1.0f, 0.5f);
        set(1, 0.0f, 0.5f, 1.0f, 0.5f);
      }
      return 2;
    }
    case 3:
      if (layout.three_players == ThreePlayers::FavorTop) {
        set(0, 0.0f, 0.0f, 1.0f, 0.5f);
        set(1, 0.0f, 0.5f, 0.5f, 0.5f);
        set(2, 0.5f, 0.5f, 0.5f, 0.5f);
      }
      else {
        set(0, 0.0f, 0.0f, 0.5f, 0.5f);
        set(1, 0.5f, 0.0f, 0.5f, 0.5f);
        set(2, 0.0f, 0.5f, 1.0f, 0.5f);
      }
      return 3;
    default:
      break;
  }
  /* Four and more: a grid of ceil(sqrt(count)) columns filled row by row (Unity's rule). Four
   * cells have the window's shape; 5 to 8 players get 3 columns, and 7 leaves cells empty. */
  const int views = std::min(count, MaxPlayers);
  const int columns = int(std::ceil(std::sqrt(float(views))));
  const int rows = (views + columns - 1) / columns;
  for (int i = 0; i < views; ++i) {
    set(i,
        float(i % columns) / float(columns),
        float(i / columns) / float(rows),
        1.0f / float(columns),
        1.0f / float(rows));
  }
  return views;
}

/** One axis of AreaToPixels: the first and last pixel of the fractions [from, to]. */
static void area_edges_to_pixels(const float from,
                                 const float to,
                                 const int origin,
                                 const int size,
                                 const int divider,
                                 int &r_first,
                                 int &r_last)
{
  const int first = int(std::lround(std::clamp(from, 0.0f, 1.0f) * float(size)));
  const int last = int(std::lround(std::clamp(to, 0.0f, 1.0f) * float(size)));
  /* Pixel edges are inclusive, so a view ends one pixel before its neighbor starts. */
  r_first = origin + (first <= 0 ? 0 : first + divider - divider / 2);
  r_last = origin + (last >= size ? size : last - 1 - divider / 2);
  /* At least one pixel: a view without a size would divide by zero in its projection. */
  r_last = std::max(r_last, r_first + 1);
}

RAS_Rect KX_PlayerViews::AreaToPixels(const Area &area, const RAS_Rect &window, const int divider)
{
  const int gap = std::max(divider, 0);
  int left, right, bottom, top;
  area_edges_to_pixels(
      area.left, area.left + area.width, window.GetLeft(), window.GetWidth(), gap, left, right);
  /* Areas start at the top of the window, viewports at the bottom. */
  area_edges_to_pixels(1.0f - area.top - area.height,
                       1.0f - area.top,
                       window.GetBottom(),
                       window.GetHeight(),
                       gap,
                       bottom,
                       top);
  return RAS_Rect(left, bottom, right, top);
}

KX_PlayerViews::Area KX_PlayerViews::PixelsToArea(const RAS_Rect &rect, const RAS_Rect &window)
{
  const float width = float(std::max(window.GetWidth(), 1));
  const float height = float(std::max(window.GetHeight(), 1));
  const int right = rect.GetRight() >= window.GetRight() ? window.GetRight() : rect.GetRight() + 1;
  const int top = rect.GetTop() >= window.GetTop() ? window.GetTop() : rect.GetTop() + 1;
  Area area;
  area.left = float(rect.GetLeft() - window.GetLeft()) / width;
  area.top = float(window.GetTop() - top) / height;
  area.width = float(right - rect.GetLeft()) / width;
  area.height = float(top - rect.GetBottom()) / height;
  return area;
}

bool KX_PlayerViews::InsideViewportCircle(const RAS_Rect &viewport, const float x, const float y)
{
  /* The drawn viewport spans [left, right + 1) by [bottom, top + 1), and the circle shader keeps
   * the pixels whose center is inside the oval filling it. */
  const float radius_x = float(viewport.GetWidth() + 1) * 0.5f;
  const float radius_y = float(viewport.GetHeight() + 1) * 0.5f;
  if (radius_x <= 0.0f || radius_y <= 0.0f) {
    return false;
  }
  const float dx = (x + 0.5f - (float(viewport.GetLeft()) + radius_x)) / radius_x;
  const float dy = (y + 0.5f - (float(viewport.GetBottom()) + radius_y)) / radius_y;
  return dx * dx + dy * dy <= 1.0f;
}

void KX_PlayerViews::FitFrustumToView(const FieldOfView field_of_view,
                                      const RAS_Rect &window,
                                      const RAS_Rect &view,
                                      RAS_FrameFrustum &frustum)
{
  if (field_of_view == FieldOfView::Fit || window.GetWidth() <= 0 || window.GetHeight() <= 0 ||
      view.GetWidth() <= 0 || view.GetHeight() <= 0)
  {
    return;
  }
  const float share_x = float(view.GetWidth()) / float(window.GetWidth());
  const float share_y = float(view.GetHeight()) / float(window.GetHeight());
  /* Crop keeps the axis split the least: horizontal for top and bottom, vertical for side by
   * side, either for a grid (both give the whole picture). */
  const bool keep_horizontal = field_of_view == FieldOfView::KeepHorizontal ||
                               (field_of_view == FieldOfView::Crop && share_x >= share_y);
  const float scale_x = keep_horizontal ? 1.0f : share_x / share_y;
  const float scale_y = keep_horizontal ? share_y / share_x : 1.0f;
  /* Around the picture's center, so a lens shift stays where it was. */
  const float center_x = 0.5f * (frustum.x1 + frustum.x2);
  const float center_y = 0.5f * (frustum.y1 + frustum.y2);
  frustum.x1 = center_x + (frustum.x1 - center_x) * scale_x;
  frustum.x2 = center_x + (frustum.x2 - center_x) * scale_x;
  frustum.y1 = center_y + (frustum.y1 - center_y) * scale_y;
  frustum.y2 = center_y + (frustum.y2 - center_y) * scale_y;
}

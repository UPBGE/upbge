/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_UIService.cpp
 *  \ingroup logicnodes
 */

#include "LN_UIService.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "BKE_object.hh"
#include "BKE_property.hh"
#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "DNA_object_types.h"
#include "DNA_property_types.h"
#include "DNA_scene_types.h"

#include "CM_Message.h"
#include "EXP_StringValue.h"
#include "KX_Camera.h"
#include "KX_ClientObjectInfo.h"
#include "KX_FontObject.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "KX_RayCast.h"
#include "KX_Scene.h"
#include "LN_CommandBuffer.h"
#include "LN_InputMap.h"
#include "LN_Inventory.h"
#include "LN_Manager.h"
#include "LN_RuntimeTree.h"
#include "LN_Snapshot.h"
#include "MT_Matrix4x4.h"
#include "PHY_IPhysicsController.h"
#include "RAS_FramingManager.h"
#include "RAS_ICanvas.h"
#include "RAS_Rasterizer.h"
#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"

namespace {

/** A panel or backdrop: an element with no Action that never takes focus. It still blocks clicks,
 * but it is no control: it takes no highlight and makes no UI sound. */
bool IsBackgroundElement(const LN_UIService::ElementSettings &settings)
{
  return settings.action.empty() && settings.focus == LN_UIElementFocus::None;
}

constexpr float kScaleEpsilon = 1.0e-8f;
constexpr float kViewBoundaryEpsilon = 1.0e-5f;
/** Seconds after player 0's keys start working in a menu before the mouse counts for them (on
 * Wayland the system reports where its pointer is ~0.16 s after the cursor is shown or hidden). */
constexpr double kMouseForKeysSettle = 0.25;

bool IsFinite(const MT_Vector3 &value)
{
  return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

bool IsFinite(const MT_Matrix3x3 &value)
{
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      if (!std::isfinite(float(value[row][column]))) {
        return false;
      }
    }
  }
  return true;
}

template<typename Value> void HashCombine(size_t &seed, const Value &value)
{
  seed ^= std::hash<Value>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6u) + (seed >> 2u);
}

size_t ValueFingerprint(const LN_Value &value)
{
  return LN_ValueFingerprint(value);
}

bool PointInsideView(const LN_UIService::ViewRegistration &view, const MT_Vector3 &point)
{
  if (std::abs(view.world_scale.x()) <= kScaleEpsilon ||
      std::abs(view.world_scale.y()) <= kScaleEpsilon ||
      std::abs(view.world_scale.z()) <= kScaleEpsilon)
  {
    return false;
  }
  const MT_Vector3 unrotated = view.world_orientation.transposed() * (point - view.world_position);
  const MT_Vector3 local(unrotated.x() / view.world_scale.x(),
                         unrotated.y() / view.world_scale.y(),
                         unrotated.z() / view.world_scale.z());
  /* A view clips across its viewport's face, not through it: the thinnest side is its depth.
   * A flat panel has no depth at all, and the slots or content in front of it are still inside. */
  int depth = 2;
  for (const int axis : {1, 0}) {
    if (std::abs((view.bounds_max[axis] - view.bounds_min[axis]) * view.world_scale[axis]) <
        std::abs((view.bounds_max[depth] - view.bounds_min[depth]) * view.world_scale[depth]))
    {
      depth = axis;
    }
  }
  const float padding = std::max(0.0f, view.padding) + kViewBoundaryEpsilon;
  for (int axis = 0; axis < 3; ++axis) {
    if (axis != depth && (local[axis] < view.bounds_min[axis] - padding ||
                          local[axis] > view.bounds_max[axis] + padding))
    {
      return false;
    }
  }
  return true;
}

MT_Vector3 PointInViewSpace(const LN_UIService::ViewRegistration &view, const MT_Vector3 &point)
{
  const MT_Vector3 unrotated = view.world_orientation.transposed() * (point - view.world_position);
  return MT_Vector3(unrotated.x() / view.world_scale.x(),
                    unrotated.y() / view.world_scale.y(),
                    unrotated.z() / view.world_scale.z());
}

MT_Vector3 WorldDeltaToObjectLocal(KX_GameObject *object, const MT_Vector3 &world_delta)
{
  KX_GameObject *parent = object != nullptr ? object->GetParent() : nullptr;
  if (parent == nullptr) {
    return world_delta;
  }
  const MT_Vector3 scale = parent->NodeGetWorldScaling();
  if (std::abs(scale.x()) <= kScaleEpsilon || std::abs(scale.y()) <= kScaleEpsilon ||
      std::abs(scale.z()) <= kScaleEpsilon)
  {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  const MT_Vector3 unrotated = parent->NodeGetWorldOrientation().transposed() * world_delta;
  return MT_Vector3(
      unrotated.x() / scale.x(), unrotated.y() / scale.y(), unrotated.z() / scale.z());
}

bool WorldToObjectSpace(KX_GameObject *object, const MT_Vector3 &point, MT_Vector3 &r_local)
{
  const MT_Vector3 scale = object->NodeGetWorldScaling();
  if (std::abs(scale.x()) <= kScaleEpsilon || std::abs(scale.y()) <= kScaleEpsilon ||
      std::abs(scale.z()) <= kScaleEpsilon)
  {
    return false;
  }
  const MT_Vector3 unrotated = object->NodeGetWorldOrientation().transposed() *
                               (point - object->NodeGetWorldPosition());
  r_local = MT_Vector3(
      unrotated.x() / scale.x(), unrotated.y() / scale.y(), unrotated.z() / scale.z());
  return IsFinite(r_local);
}

/** The handle's position and scale in its bar's space, from world space: a child's own local
 * transform also holds Blender's parent inverse. The handle is turned like the bar. */
bool HandleInBarSpace(KX_GameObject *bar,
                      KX_GameObject *handle,
                      MT_Vector3 &r_position,
                      MT_Vector3 &r_scale)
{
  if (!WorldToObjectSpace(bar, handle->NodeGetWorldPosition(), r_position)) {
    return false;
  }
  const MT_Vector3 bar_scale = bar->NodeGetWorldScaling();
  const MT_Vector3 scale = handle->NodeGetWorldScaling();
  r_scale = MT_Vector3(
      scale.x() / bar_scale.x(), scale.y() / bar_scale.y(), scale.z() / bar_scale.z());
  return IsFinite(r_scale);
}

/** A scrollbar runs along its bar's longest side (`axis`) and lies flat across its thinnest
 * (`normal`). A vertical bar starts at its top, a horizontal one at its left. Lengths are in the
 * bar's own space, which is also the space of its handle's position. */
struct ScrollbarLayout {
  int axis = 1;
  int normal = 2;
  float length = 0.0f;
  float handle_length = 0.0f;
  bool valid = false;
};

ScrollbarLayout MakeScrollbarLayout(const LN_UIService::ViewRegistration &view,
                                    const float fraction)
{
  ScrollbarLayout layout;
  if (view.scrollbar == nullptr) {
    return layout;
  }
  const MT_Vector3 scale = view.scrollbar->NodeGetWorldScaling();
  float size[3];
  for (int axis = 0; axis < 3; ++axis) {
    size[axis] = std::abs((view.scrollbar_max[axis] - view.scrollbar_min[axis]) * scale[axis]);
  }
  for (const int axis : {1, 0}) {
    if (size[axis] < size[layout.normal]) {
      layout.normal = axis;
    }
  }
  layout.axis = layout.normal == 1 ? 2 : 1;
  for (int axis = 0; axis < 3; ++axis) {
    if (axis != layout.normal && size[axis] > size[layout.axis]) {
      layout.axis = axis;
    }
  }
  layout.length = view.scrollbar_max[layout.axis] - view.scrollbar_min[layout.axis];
  if (!(layout.length > kScaleEpsilon)) {
    return layout;
  }
  /* The handle shows the part in view, but is never shorter than it is wide (compared in world
   * units: the bar's own space can be stretched). */
  float width = 0.0f;
  if (view.scrollbar_handle != nullptr && size[layout.axis] > kScaleEpsilon) {
    const int across = 3 - layout.axis - layout.normal;
    width = (view.handle_max[across] - view.handle_min[across]) *
            std::abs(view.scrollbar_handle->NodeGetWorldScaling()[across]) * layout.length /
            size[layout.axis];
  }
  layout.handle_length = std::clamp(layout.length * std::clamp(fraction, 0.0f, 1.0f),
                                    std::clamp(width, 0.0f, layout.length),
                                    layout.length);
  layout.valid = std::isfinite(layout.handle_length);
  return layout;
}

/** Distance of a bar-space point from the bar's start. */
float ScrollbarAlong(const LN_UIService::ViewRegistration &view,
                     const ScrollbarLayout &layout,
                     const MT_Vector3 &local)
{
  return layout.axis == 0 ? local.x() - view.scrollbar_min.x() :
                            view.scrollbar_max[layout.axis] - local[layout.axis];
}

/** The bar as a pointer target: its box grown to hold the handle, which may be wider. */
LN_UIService::Registration ScrollbarRegistration(const LN_UIService::ViewRegistration &view)
{
  LN_UIService::Registration registration;
  registration.object = view.scrollbar;
  registration.camera = view.camera;
  registration.ray_from = view.ray_from;
  registration.ray_to = view.ray_to;
  registration.bounds_min = view.scrollbar_min;
  registration.bounds_max = view.scrollbar_max;
  MT_Vector3 position;
  MT_Vector3 scale;
  if (view.scrollbar_handle != nullptr &&
      HandleInBarSpace(view.scrollbar, view.scrollbar_handle, position, scale))
  {
    for (int axis = 0; axis < 3; ++axis) {
      const float low = position[axis] + view.handle_min[axis] * scale[axis];
      const float high = position[axis] + view.handle_max[axis] * scale[axis];
      registration.bounds_min[axis] = std::min({registration.bounds_min[axis], low, high});
      registration.bounds_max[axis] = std::max({registration.bounds_max[axis], low, high});
    }
  }
  registration.world_position = view.scrollbar->NodeGetWorldPosition();
  registration.world_orientation = view.scrollbar->NodeGetWorldOrientation();
  registration.world_scale = view.scrollbar->NodeGetWorldScaling();
  /* The pointer is outside the view: the bar is hit by nothing. */
  if ((view.ray_to - view.ray_from).length2() <= kScaleEpsilon) {
    registration.has_precomputed_hit = true;
    registration.precomputed_hit = false;
  }
  return registration;
}

/** Main-world raycast for UI Elements: visible, non-overlay, non-sensor objects only. */
class UIWorldRayCallback : public KX_RayCast {
 public:
  explicit UIWorldRayCallback(KX_Scene &scene) : KX_RayCast(nullptr, false, false), m_scene(scene)
  {
    m_hitFound = false;
  }

  bool RayHit(KX_ClientObjectInfo *client) override
  {
    KX_GameObject *object = client != nullptr ? client->m_gameobject : nullptr;
    if (object == nullptr || client->isSensor() || !object->GetVisible() ||
        m_scene.IsOverlayRuntimeObject(object) || Ignored(object))
    {
      return false;
    }
    m_object = object;
    m_hitFound = true;
    return true;
  }

  bool needBroadphaseRayCast(PHY_IPhysicsController *controller) override
  {
    KX_ClientObjectInfo *info = controller != nullptr ?
                                    static_cast<KX_ClientObjectInfo *>(
                                        controller->GetNewClientInfo()) :
                                    nullptr;
    return info != nullptr && info->m_gameobject != nullptr && !info->isSensor() &&
           !m_scene.IsOverlayRuntimeObject(info->m_gameobject) && !Ignored(info->m_gameobject);
  }

  /** The players' pointer cursors (with their children): they float in front of what they point
   * at, and must not hide it. What the ray's camera does not draw (Set Camera Visibility) is not
   * in the way either: a player's body hidden from a camera behind them. */
  bool Ignored(KX_GameObject *object) const
  {
    if (m_camera != nullptr && m_scene.IsHiddenForCamera(object, m_camera)) {
      return true;
    }
    for (KX_GameObject *cursor : m_ignored) {
      for (KX_GameObject *item = object; item != nullptr; item = item->GetParent()) {
        if (item == cursor) {
          return true;
        }
      }
    }
    return false;
  }

  KX_Scene &m_scene;
  /** The camera the ray goes out from (the view the player points in). */
  const KX_Camera *m_camera = nullptr;
  KX_GameObject *m_object = nullptr;
  std::vector<KX_GameObject *> m_ignored;
};

/** Visible rectangle of a camera at camera-space depth 1 (perspective) or any depth (ortho). */
struct CameraFrame {
  float left = -1.0f;
  float right = 1.0f;
  float bottom = -1.0f;
  float top = 1.0f;
  bool perspective = true;
};

/** The same frustum the engine renders `camera` with for `area` and `viewport`. */
bool ComputeCameraFrame(const RAS_FrameSettings &settings,
                        KX_Camera *camera,
                        const RAS_Rect &area,
                        const RAS_Rect &viewport,
                        const float zoom,
                        CameraFrame &r_frame)
{
  if (camera == nullptr || camera->GetCameraData() == nullptr || viewport.GetWidth() <= 0 ||
      viewport.GetHeight() <= 0)
  {
    return false;
  }
  RAS_FrameFrustum frustum;
  const float camera_near = camera->GetCameraNear();
  r_frame.perspective = camera->GetCameraData()->m_perspective;
  if (r_frame.perspective) {
    RAS_FramingManager::ComputeFrustum(settings,
                                       area,
                                       viewport,
                                       camera->GetLens(),
                                       camera->GetSensorWidth(),
                                       camera->GetSensorHeight(),
                                       camera->GetSensorFit(),
                                       camera->GetShiftHorizontal(),
                                       camera->GetShiftVertical(),
                                       camera_near,
                                       camera->GetCameraFar(),
                                       frustum);
  }
  else {
    RAS_FramingManager::ComputeOrtho(settings,
                                     area,
                                     viewport,
                                     camera->GetScale(),
                                     camera_near,
                                     camera->GetCameraFar(),
                                     camera->GetSensorFit(),
                                     camera->GetShiftHorizontal(),
                                     camera->GetShiftVertical(),
                                     frustum);
  }
  /* Perspective extents are given on the near plane; scale them to depth 1. */
  const float scale = zoom * (r_frame.perspective && camera_near > 0.0f ? 1.0f / camera_near :
                                                                           1.0f);
  r_frame.left = frustum.x1 * scale;
  r_frame.right = frustum.x2 * scale;
  r_frame.bottom = frustum.y1 * scale;
  r_frame.top = frustum.y2 * scale;
  return std::isfinite(r_frame.left) && std::isfinite(r_frame.right) &&
         std::isfinite(r_frame.bottom) && std::isfinite(r_frame.top) &&
         r_frame.right > r_frame.left && r_frame.top > r_frame.bottom;
}

/** Anchor point of a frame in camera-space units at `depth`, plus the frame height there. */
void AnchorPointInFrame(const CameraFrame &frame,
                        const LN_UIElementAnchor anchor,
                        const float depth,
                        float &r_x,
                        float &r_y,
                        float &r_height)
{
  const float scale = frame.perspective ? depth : 1.0f;
  const float left = frame.left * scale;
  const float right = frame.right * scale;
  const float bottom = frame.bottom * scale;
  const float top = frame.top * scale;
  const int index = int(anchor) - 1;
  const int column = index % 3;
  const int row = index / 3;
  r_x = column == 0 ? left : (column == 1 ? (left + right) * 0.5f : right);
  r_y = row == 0 ? top : (row == 1 ? (bottom + top) * 0.5f : bottom);
  r_height = top - bottom;
}

}  // namespace

bool LN_ComputeCameraPointerRay(KX_Camera *camera,
                                const float pointer_x,
                                const float pointer_y,
                                const float distance,
                                MT_Vector3 &r_from,
                                MT_Vector3 &r_to,
                                const bool beyond_view)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
  RAS_Rasterizer *rasterizer = engine ? engine->GetRasterizer() : nullptr;
  KX_Scene *scene = camera ? camera->GetScene() : nullptr;
  if (camera == nullptr || canvas == nullptr || rasterizer == nullptr || scene == nullptr ||
      canvas->GetWidth() <= 0 || canvas->GetHeight() <= 0)
  {
    return false;
  }

  const RAS_Rect display_area = rasterizer->GetRenderArea(canvas,
                                                          RAS_Rasterizer::RAS_STEREO_LEFTEYE);
  RAS_Rect camera_area;
  RAS_Rect viewport;
  engine->GetSceneViewport(scene, camera, display_area, camera_area, viewport);
  if (viewport.GetWidth() <= 0 || viewport.GetHeight() <= 0) {
    return false;
  }

  if (pointer_x < 0.0f || pointer_x > float(canvas->GetWidth()) || pointer_y < 0.0f ||
      pointer_y > float(canvas->GetHeight()))
  {
    return false;
  }

  /* Input positions use top-left canvas coordinates, while render viewports use bottom-left
   * coordinates. Resolve the actual camera viewport so framing bars and custom viewports never
   * shift hit tests away from what was rendered. */
  const float pointer_y_bottom = float(canvas->GetHeight()) - pointer_y;
  if (!beyond_view &&
      (pointer_x < float(viewport.GetLeft()) || pointer_x > float(viewport.GetRight()) ||
       pointer_y_bottom < float(viewport.GetBottom()) ||
       pointer_y_bottom > float(viewport.GetTop())))
  {
    return false;
  }
  const int drawn_extra = LN_CameraViewportDrawnExtra(camera);
  const float ndc_x = 2.0f * ((pointer_x - float(viewport.GetLeft())) /
                              float(viewport.GetWidth() + drawn_extra)) -
                      1.0f;
  const float ndc_y = 2.0f * ((pointer_y_bottom - float(viewport.GetBottom())) /
                              float(viewport.GetHeight() + drawn_extra)) -
                      1.0f;

  const MT_Matrix4x4 projection = engine->GetCameraProjectionMatrix(
      scene, camera, RAS_Rasterizer::RAS_STEREO_LEFTEYE, viewport, camera_area);
  MT_Matrix4x4 inverse_projection = projection;
  inverse_projection.invert();
  const MT_Vector4 near_camera = inverse_projection * MT_Vector4(ndc_x, ndc_y, -1.0f, 1.0f);
  const MT_Vector4 near_world = MT_Matrix4x4(camera->GetCameraToWorld()) * near_camera;
  if (std::fabs(near_world[3]) <= MT_EPSILON) {
    return false;
  }
  const MT_Vector3 near_position(
      near_world[0] / near_world[3], near_world[1] / near_world[3], near_world[2] / near_world[3]);
  if (!IsFinite(near_position)) {
    return false;
  }
  const bool perspective = camera->GetCameraData()->m_perspective;
  r_from = perspective ? camera->NodeGetWorldPosition() : near_position;
  MT_Vector3 direction = perspective ? near_position - r_from :
                                       -camera->NodeGetWorldOrientation().getColumn(2);
  if (direction.length2() <= MT_EPSILON * MT_EPSILON) {
    return false;
  }
  direction.normalize();
  r_to = r_from + direction * std::max(distance, 0.0f);
  return true;
}

int LN_CameraViewportDrawnExtra(const KX_Camera *camera)
{
  /* KX_Scene::RenderAfterCameraSetup draws such a viewport GetWidth() + 1 wide (the menu camera
   * never has one). */
  return camera != nullptr && camera->GetViewport() ? 1 : 0;
}

KX_Camera *LN_CameraUnderPointer(KX_Scene &scene, const float pointer_x, const float pointer_y)
{
  KX_Camera *active_camera = scene.GetActiveCamera();
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
  EXP_ListValue<KX_Camera> *cameras = scene.GetCameraList();
  /* Use Viewport Render draws the active camera alone. */
  if (canvas == nullptr || cameras == nullptr || engine->UseViewportRender()) {
    return active_camera;
  }
  /* Cameras draw in list order, so the last one containing the pointer is on top. The active
   * camera without a viewport covers the whole window: nothing listed before it shows. */
  const float pointer_y_bottom = float(canvas->GetHeight()) - pointer_y;
  /* Split screen: the mouse is player 0's, so it reaches only their view. */
  RAS_Rect mouse_view;
  const KX_PlayerViews &views = scene.GetPlayerViews();
  if (views.GetMouseView(active_camera, views.GetWindow(), mouse_view) &&
      (pointer_x < float(mouse_view.GetLeft()) || pointer_x > float(mouse_view.GetRight()) ||
       pointer_y_bottom < float(mouse_view.GetBottom()) ||
       pointer_y_bottom > float(mouse_view.GetTop())))
  {
    return nullptr;
  }
  KX_Camera *overlay_camera = scene.GetOverlayCamera();
  for (int index = cameras->GetCount() - 1; index >= 0; --index) {
    KX_Camera *camera = cameras->GetValue(index);
    if (camera == nullptr || camera == overlay_camera) {
      continue;
    }
    if (!camera->GetViewport()) {
      if (camera == active_camera) {
        return camera;
      }
      continue;
    }
    if (pointer_x >= float(camera->GetViewportLeft()) &&
        pointer_x <= float(camera->GetViewportRight()) &&
        pointer_y_bottom >= float(camera->GetViewportBottom()) &&
        pointer_y_bottom <= float(camera->GetViewportTop()))
    {
      /* A round view (Viewport Shape: Circle) leaves its corners to the views behind. */
      if (camera->HasCircleViewport() &&
          !KX_PlayerViews::InsideViewportCircle(RAS_Rect(camera->GetViewportLeft(),
                                                         camera->GetViewportBottom(),
                                                         camera->GetViewportRight(),
                                                         camera->GetViewportTop()),
                                                pointer_x,
                                                pointer_y_bottom))
      {
        continue;
      }
      return camera;
    }
  }
  return active_camera;
}

bool LN_ProjectWorldToScreen(KX_Camera *camera,
                             const MT_Vector3 &world_position,
                             MT_Vector3 &r_screen_position)
{
  if (camera == nullptr) {
    return false;
  }
  const MT_Matrix4x4 world_to_camera(camera->GetWorldToCamera());
  const MT_Matrix4x4 projection = camera->GetProjectionMatrix();
  const MT_Vector4 camera_position = world_to_camera * MT_Vector4(
                                                          world_position.x(),
                                                          world_position.y(),
                                                          world_position.z(),
                                                          1.0f);
  const MT_Vector4 clip_position = projection * camera_position;
  if (std::fabs(clip_position[3]) <= MT_EPSILON) {
    return false;
  }
  const float ndc_x = clip_position[0] / clip_position[3];
  const float ndc_y = clip_position[1] / clip_position[3];
  const float ndc_z = clip_position[2] / clip_position[3];
  if (clip_position[3] <= 0.0f || ndc_z > 1.0f) {
    r_screen_position = MT_Vector3(FLT_MAX, FLT_MAX, 0.0f);
    return true;
  }
  r_screen_position = MT_Vector3((ndc_x + 1.0f) * 0.5f, 1.0f - ((ndc_y + 1.0f) * 0.5f), 0.0f);
  return true;
}

const char *LN_UIResultMessage(const LN_UIResult result)
{
  switch (result) {
    case LN_UIResult::Success:
      return "success";
    case LN_UIResult::NoChange:
      return "no change";
    case LN_UIResult::InvalidHandle:
      return "invalid menu handle";
    case LN_UIResult::InvalidOwner:
      return "invalid owner";
    case LN_UIResult::InvalidCamera:
      return "invalid or missing overlay camera";
    case LN_UIResult::InvalidCollection:
      return "invalid collection";
    case LN_UIResult::WrongScene:
      return "object does not belong to the receiving scene";
    case LN_UIResult::CameraUnavailable:
      return "overlay camera is unavailable";
    case LN_UIResult::CameraConflict:
      return "a different overlay camera is already active";
    case LN_UIResult::CollectionUnavailable:
      return "collection has no runtime objects";
    case LN_UIResult::CollectionConflict:
      return "collection overlaps or conflicts with an active overlay";
    case LN_UIResult::RuntimeUnavailable:
      return "runtime is unavailable";
    case LN_UIResult::InstantiationFailed:
      return "overlay objects could not be instantiated";
    case LN_UIResult::InvalidTarget:
      return "invalid target";
    case LN_UIResult::SessionConflict:
      return "the top menu is already open with different settings";
    case LN_UIResult::AlreadyOpen:
      return "the collection is already open as a deeper menu";
    case LN_UIResult::HandleExhausted:
      return "menu handle space is exhausted";
    case LN_UIResult::StaleOwner:
      return "command owner is no longer available";
    case LN_UIResult::Closed:
      return "the menu was open, so it closed";
    case LN_UIResult::OpenForOtherPlayer:
      return "the menu is open for another player; only that player's Toggle or Back closes it";
    case LN_UIResult::ControlledByOtherPlayer:
      return "another player has this UI";
  }
  return "unknown UI result";
}

LN_UIResult LN_MapOverlayResult(const KX_OverlayCollectionResult result)
{
  switch (result) {
    case KX_OverlayCollectionResult::Success:
      return LN_UIResult::Success;
    case KX_OverlayCollectionResult::NoChange:
      return LN_UIResult::NoChange;
    case KX_OverlayCollectionResult::MissingCamera:
      return LN_UIResult::InvalidCamera;
    case KX_OverlayCollectionResult::MissingCollection:
      return LN_UIResult::InvalidCollection;
    case KX_OverlayCollectionResult::CameraWrongScene:
    case KX_OverlayCollectionResult::CollectionWrongScene:
    case KX_OverlayCollectionResult::CollectionSharedAcrossScenes:
      return LN_UIResult::WrongScene;
    case KX_OverlayCollectionResult::CameraUnavailable:
      return LN_UIResult::CameraUnavailable;
    case KX_OverlayCollectionResult::CameraConflict:
      return LN_UIResult::CameraConflict;
    case KX_OverlayCollectionResult::CollectionHasNoRuntimeObjects:
      return LN_UIResult::CollectionUnavailable;
    case KX_OverlayCollectionResult::CollectionHierarchyConflict:
    case KX_OverlayCollectionResult::CollectionOverlap:
      return LN_UIResult::CollectionConflict;
    case KX_OverlayCollectionResult::RuntimeUnavailable:
      return LN_UIResult::RuntimeUnavailable;
    case KX_OverlayCollectionResult::ObjectInstantiationFailed:
      return LN_UIResult::InstantiationFailed;
  }
  return LN_UIResult::RuntimeUnavailable;
}

namespace {

/** A button of this user's gamepad was just pressed: the user plays with a gamepad. */
bool AnyGamepadButtonPressed(const LN_InputSnapshot *input, const int32_t user)
{
  const LN_GamepadSnapshot *gamepad = input != nullptr ?
                                          input->GetGamepad(LN_InputMap::Get().GamepadOfUser(user)) :
                                          nullptr;
  return gamepad != nullptr && gamepad->connected &&
         std::any_of(gamepad->pressed.begin(), gamepad->pressed.end(), [](const uint8_t value) {
           return value != 0;
         });
}

constexpr std::array<LN_UIAction, 6> kNavigationActions = {LN_UIAction::NavigateUp,
                                                           LN_UIAction::NavigateDown,
                                                           LN_UIAction::NavigateLeft,
                                                           LN_UIAction::NavigateRight,
                                                           LN_UIAction::Next,
                                                           LN_UIAction::Previous};
/** A stick direction starts at the Input Map's stick threshold and holds until the stick falls
 * back under this share of it, so a drifting stick neither navigates nor flickers. */
constexpr float kStickRelease = 0.7f;
/** The repeat times are float settings (0.4f is a little over 0.4 s): a hold reaching them within
 * this counts as reaching them. */
constexpr double kRepeatEpsilon = 1.0e-6;

bool IsWheelBinding(const LN_InputBinding &binding)
{
  return binding.device == LN_InputDevice::Mouse &&
         (binding.input == SCA_IInputDevice::WHEELUPMOUSE ||
          binding.input == SCA_IInputDevice::WHEELDOWNMOUSE);
}

bool SameObjectHandle(const LN_ObjectHandle &a, const LN_ObjectHandle &b)
{
  return a.IsValid() && b.IsValid() && a.kind == b.kind && a.index == b.index &&
         a.generation == b.generation;
}

/** Categories as typed ("Weapon, Shield": Accept Categories, or an item's category property) to
 * lowercase names between commas (",weapon,shield,"); empty when it names none. */
std::string NormalizedCategories(const std::string &text)
{
  return LN_NormalizedCategories(text);
}

/** One of `categories` is in `accepted` (both from NormalizedCategories). */
bool CategoriesOverlap(const std::string &accepted, const std::string &categories)
{
  return LN_CategoriesOverlap(accepted, categories);
}

/** On UI Event's Action: empty matches every element, else one name or several separated by
 * commas ("weapon, armor"), spaces around each ignored. The whole text matching counts too, so
 * an Action that holds a comma keeps working. */
bool ActionListed(const std::string &filter, const std::string &action)
{
  if (filter.empty() || filter == action) {
    return true;
  }
  if (action.empty() || filter.find(',') == std::string::npos) {
    return false;
  }
  const std::string_view text(filter);
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    size_t first = start;
    size_t last = end;
    while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) {
      first++;
    }
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) {
      last--;
    }
    if (text.substr(first, last - first) == action) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

std::string Trimmed(const std::string_view text)
{
  size_t first = 0;
  size_t last = text.size();
  while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) {
    first++;
  }
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) {
    last--;
  }
  return std::string(text.substr(first, last - first));
}

std::string LowerCase(std::string text)
{
  for (char &character : text) {
    character = char(std::tolower(static_cast<unsigned char>(character)));
  }
  return text;
}

/** The names of a comma separated list, spaces around each removed. An empty name keeps its
 * place, so the n-th name stays the n-th ("Quick 1, , Quick 3"). */
std::vector<std::string> CommaList(const std::string &text)
{
  std::vector<std::string> names;
  if (Trimmed(text).empty()) {
    return names;
  }
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    names.push_back(Trimmed(std::string_view(text).substr(start, end - start)));
    start = end + 1;
  }
  return names;
}

/** Grid View Filter shows only some items. */
bool IsFiltered(const LN_UIService::ViewRegistration &view)
{
  return !NormalizedCategories(view.show_categories).empty() || !Trimmed(view.search).empty();
}

/** The two grids show the same list (of one object's property). */
bool SameGridList(const LN_UIService::ViewRegistration &a, const LN_UIService::ViewRegistration &b)
{
  return a.list_object == b.list_object && a.list_property == b.list_property;
}

/** A grid of one cell is a slot (equipment, a quick slot): what is dropped on its item swaps or
 * stacks with it, nothing goes into its list beside it (the second item would never show). */
bool HoldsOneSlot(const LN_UIService::ViewRegistration &view)
{
  return view.slots.size() <= 1;
}

/** An item's own value: an inventory slot is a dictionary that can hold more than its item and
 * count (durability, charges, a name of its own), which comes before its Item Library object's. */
const LN_Value *OwnValue(const LN_Value &item, const std::string &key)
{
  if (!item.exists || item.type != LN_ValueType::Dict) {
    return nullptr;
  }
  const auto entry = item.dict_value.find(key);
  return entry != item.dict_value.end() && entry->second.exists ? &entry->second : nullptr;
}

/** An own value as a text shows it: numbers without needless decimals. */
std::string OwnValueText(const LN_Value &value)
{
  switch (value.type) {
    case LN_ValueType::String:
      return value.string_value;
    case LN_ValueType::Int:
      return std::to_string(value.int_value);
    case LN_ValueType::Float: {
      char text[32];
      std::snprintf(text, sizeof(text), "%g", double(value.float_value));
      return text;
    }
    case LN_ValueType::Bool:
      return value.bool_value ? "True" : "False";
    case LN_ValueType::ObjectRef:
      return value.reference_name;
    default:
      return std::string();
  }
}

}  // namespace

struct LN_UIService::MenuSession {
  enum class State : uint8_t {
    Open = 0,
    CoveredVisible,
    CoveredHidden,
  };

  MenuHandle handle = 0;
  /** The player whose menu it is: its UI Elements are that player's. */
  int32_t user = 0;
  const LN_RuntimeTree *owner_tree = nullptr;
  LN_ObjectHandle owner;
  KX_Camera *camera = nullptr;
  blender::Collection *collection = nullptr;
  LN_UIMenuRouting routing = LN_UIMenuRouting::Modal;
  LN_UIMenuOpenMode open_mode = LN_UIMenuOpenMode::Push;
  LN_UIMenuPresentation presentation = LN_UIMenuPresentation::Screen;
  LN_UIMenuBackBehavior back_behavior = LN_UIMenuBackBehavior::CloseMenu;
  /** Opened with Pause Game: the game is paused while it is open. */
  bool pauses_game = false;
  /** When it opened. Opened by a Logic + Physics step (not a frame update), the next frame's Back
   * may be the very press that opened it (Esc opening a pause menu): see OpenedByThisBack. */
  uint64_t opened_frame = 0;
  bool opened_in_frame_update = true;
  State state = State::Open;
  std::string input_context;
  std::unordered_map<int32_t, LN_ObjectHandle> previous_focus;
  std::vector<LN_ObjectHandle> objects;
  uint64_t membership_generation = 0;
  bool needs_initial_focus = true;
  /** Which copy of the collection's objects is this menu's: another player opened the same
   * collection first, so this player got a copy of their own (KX_Scene::AddOverlayInstance). */
  int32_t instance = 0;
  /** Whose menu it is (MenuOwnerOfObject). */
  LN_ObjectHandle owner_object;
  /** Area Player's View: kept in its player's split screen view, scaled to fit. */
  bool in_player_view = false;
  /** Where it was placed last, in the menu camera's frame: scale, then the centres of the whole
   * window's frame and of the player's view (identity: 1, then equal centres). */
  float placement[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  /** It has a UI Element the keyboard or gamepad can select (from the last frame's elements). See
   * InputMenuIndex. False until they are known: a pass-through menu opened this frame may be a
   * HUD, which must not count as the menu the player is in for a frame (the cursor flashed, Mouse
   * Look waited, pointers let go). */
  bool has_selectable = false;
};

struct LN_UIService::ListenerDeclaration {
  int32_t user = 0;
  bool enabled = true;
};

struct LN_UIService::ListenerPersistentState {
  LN_ObjectHandle object;
  bool over = false;
  bool pressed_inside = false;
  bool dragging = false;
};

struct LN_UIService::PointerState {
  ListenerKey captured_listener;
  LN_ObjectHandle captured_object;
  KX_Camera *captured_camera = nullptr;
  MT_Vector3 press_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 capture_point = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 capture_normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  float capture_depth = 0.0f;
  ListenerKey drop_listener;
  LN_ObjectHandle drop_object;
  std::string drop_group;
  /** The grid item the dragged slot showed when the drag started: scrolling during the drag
   * must not change what is being dragged. */
  LN_Value drag_item;
  int32_t drag_item_index = -1;
  /** How many of drag_item the drag carries: a split stack's part (0: all of it). */
  int32_t drag_amount = 0;
  /** The categories of what is dragged, for Accept Categories: looked up when a target first asks
   * (DraggedItemCategories). */
  std::string drag_category;
  bool drag_category_known = false;
  bool has_drag_item = false;
  bool has_capture = false;
  /** Over a drop target: drop_listener and drop_object name it. drop_refused: its Accept
   * Categories refuse what is dragged, so letting go there puts it back. */
  bool has_drop_target = false;
  bool drop_refused = false;
  bool drag_active = false;
  /** Carried with Grab by keyboard or gamepad instead of dragged by the pointer: it follows the
   * focus, and Grab or Accept puts it down. Navigation from it starts where it was picked up. */
  bool carried = false;
  MT_Vector3 carry_home_screen = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 carry_home_world = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Picked up by a click (Click to Pick Up): dragged with the button up until the next click
   * puts it down. */
  bool picked_up = false;
  /** Pressed with a Set Player Pointer cursor (Accept), not the mouse: the press follows that
   * cursor's ray at the depth it was pressed. */
  bool virtual_pointer = false;
};

const LN_UIService::PointerState *LN_UIService::HeldWorldPointer() const
{
  KX_Camera *overlay_camera = m_scene.GetOverlayCamera();
  for (const auto &pointers : m_pointerStates) {
    const auto pointer = pointers.find(0);
    if (pointer != pointers.end() && pointer->second.has_capture && !pointer->second.carried &&
        !pointer->second.virtual_pointer && pointer->second.captured_camera != nullptr &&
        pointer->second.captured_camera != overlay_camera)
    {
      return &pointer->second;
    }
  }
  return nullptr;
}

KX_Camera *LN_UIService::WorldPointerCamera(const float pointer_x, const float pointer_y) const
{
  /* A press held on a world object keeps its view, so a drag can cross into another one. */
  if (const PointerState *held = HeldWorldPointer()) {
    return held->captured_camera;
  }
  return LN_CameraUnderPointer(m_scene, pointer_x, pointer_y);
}

KX_Camera *LN_UIService::WorldPointerCameraFor(const KX_GameObject *target,
                                               const float pointer_x,
                                               const float pointer_y) const
{
  const PointerState *held = target != nullptr ? HeldWorldPointer() : nullptr;
  if (held != nullptr && m_manager.ResolveObjectHandle(held->captured_object) == target) {
    return held->captured_camera;
  }
  return LN_CameraUnderPointer(m_scene, pointer_x, pointer_y);
}

bool LN_UIService::HoldsPointerInView(const KX_Camera *camera) const
{
  const PointerState *held = HeldWorldPointer();
  return camera != nullptr && held != nullptr && held->captured_camera == camera;
}

struct LN_UIService::Hit {
  const Registration *registration = nullptr;
  float fraction = 1.0f;
  MT_Vector3 point = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 normal = MT_Vector3(0.0f, 0.0f, 0.0f);
  int menu_index = -1;
  int domain_rank = 0;
  uint64_t sequence = 0;
};

struct LN_UIService::SlotVisual {
  LN_ObjectHandle slot;
  std::string item;
  int32_t count = 0;
  bool icon_found = false;
  /** Show Counts / Show Names when the text was written. */
  uint8_t text_mode = 0;
};

struct LN_UIService::ViewPersistentState {
  struct ManagedObject {
    LN_ObjectHandle object;
  };

  using SlotVisual = LN_UIService::SlotVisual;

  LN_ObjectHandle content;
  MT_Vector3 base_local_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Scroll Up/Down moved the view away from this focus: Focus Follow waits for a new one. */
  LN_ObjectHandle scrolled_from_focus;
  std::vector<ManagedObject> managed_objects;
  int32_t first_index = 0;
  int32_t last_requested_first_index = 0;
  size_t binding_fingerprint = 0;
  bool initialized = false;
  /** Grid View copies of the Slot Template and the layout they were placed for. */
  LN_ObjectHandle slot_template;
  std::vector<LN_ObjectHandle> template_copies;
  int32_t template_columns = 0;
  int32_t template_rows = 0;
  bool template_horizontal = false;
  /** The template was an overlay object (a HUD, a menu) when the copies were made: a HUD's grid
   * can lay out before Set Overlay Collection shows it, and copies made then belong to the world. */
  bool template_overlay = false;
  MT_Vector3 template_spacing = MT_Vector3(0.0f, 0.0f, 0.0f);
  std::vector<SlotVisual> slot_visuals;
  /** What each slot last told On UI Event Item Shown (0: nothing yet). */
  std::vector<size_t> shown_announced;
  /** Scrollbar handle as authored (its own local transform, and in the bar's space), where its
   * center is along the bar, and the drag: the grip's offset from the handle center and the
   * bar-space depth of the press. */
  LN_ObjectHandle scrollbar_handle;
  MT_Vector3 handle_base_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 handle_base_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  MT_Vector3 handle_bar_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 handle_bar_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  float handle_center = 0.0f;
  float scrollbar_progress = 0.0f;
  float scrollbar_grab = 0.0f;
  float scrollbar_plane = 0.0f;
  bool scrollbar_dragging = false;
};

struct LN_UIService::CellShown {
  ListenerKey view;
  size_t slot = 0;
  LN_ObjectHandle object;
  LN_Value item;
  int32_t index = -1;
  int32_t user = 0;
  size_t fingerprint = 0;
};

struct LN_UIService::DragGhost {
  LN_ObjectHandle slot;
  LN_ObjectHandle ghost;
  /** The copy and its children, to take out of m_dragGhostObjects when it goes. */
  std::vector<const KX_GameObject *> members;
  LN_ObjectHandle icon;
  LN_ObjectHandle text;
  ListenerKey view;
  size_t slot_index = 0;
  SlotVisual visual;
  /** What the copy shows (what stays in the cell), given to the slot when it comes back. */
  LN_Value shown;
  bool visible = true;
  bool icon_hidden = false;
  /** Item Only (Drag Look): the copy is the dragged item's picture, following the pointer while
   * the slot stays in its cell. Else a split drag's copy left in the cell. */
  bool picture = false;
  /** Item Only: how far the picture floats toward the camera the slot is seen through, in front
   * of the cells. */
  float lift = 0.0f;
  LN_ObjectHandle camera;
};

struct LN_UIService::ViewVisibilityState {
  LN_ObjectHandle object;
  std::unordered_set<ListenerKey, ListenerKeyHash> owners;
};

struct LN_UIService::SlotBinding {
  ListenerKey view;
  LN_Value item;
  int32_t index = -1;
};

struct LN_UIService::ElementRecord {
  LN_ObjectHandle handle;
  KX_GameObject *object = nullptr;
  ElementSettings settings;
  /** Registered in the current frame, with its routing rank for event ordering. */
  bool registered = false;
  int menu_index = -1;
  int domain_rank = 0;
  /** The players it is registered for this frame (one bit each: ElementUsers), and last frame. */
  uint8_t users = 0;
  uint8_t previous_users = 0;
  /** The player who started the drag it follows. */
  int32_t drag_user = 0;
  /** What each player's events compare with: hovered and focused last frame, and for Drop
   * Enter/Exit the dragged element over it and whether it took that one. */
  struct PlayerState {
    bool was_focused = false;
    bool was_over = false;
    LN_ObjectHandle drop_hover_source;
    bool drop_hover_accepted = false;
  };
  std::array<PlayerState, LN_MAX_GAMEPADS> players;
  /** Highlight Object: the resolved object, and whether it is shown (known after the first
   * frame). */
  PartCache highlight_object;
  bool highlight_known = false;
  bool highlight_shown = false;
  /** Refused Object, the same way. */
  PartCache refused_object;
  bool refused_known = false;
  bool refused_shown = false;
  /** Highlighted and refused as last told by Highlight Start/End and Refused Start/End, with the
   * player and (refused) the dragged element the Start event named. */
  bool lit = false;
  bool refused_lit = false;
  int32_t lit_user = 0;
  int32_t refused_user = 0;
  LN_ObjectHandle refused_by;
  /** Tooltip Object (what it shows is kept in m_tooltips). */
  PartCache tooltip_object;
  /** Widget: the object with the property, its part, and the value last shown on the part. */
  PartCache widget_owner;
  PartCache widget_part;
  size_t widget_shown = 0;
  bool widget_shown_known = false;
  /** Slider: the handle's place in the track's space when first seen. Bar: the fill's. */
  MT_Vector3 handle_base = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 handle_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  bool handle_known = false;
  /** Bar: the fill's own scale and its start and end along the bar (in the bar's space) as
   * authored, the bar's long axis, and the part of it shown now. */
  MT_Vector3 fill_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  float fill_start = 0.0f;
  float fill_end = 0.0f;
  int fill_axis = 0;
  float bar_shown = 0.0f;
  bool bar_known = false;
  /** Hover/press scale: the local scale without the look, the blend factor and the scale the
   * look last wrote (anything else there was written by game logic). */
  MT_Vector3 base_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  MT_Vector3 look_scale = MT_Vector3(1.0f, 1.0f, 1.0f);
  float look_factor = 1.0f;
  /** Follow Cursor: position before the drag (local and world, with the parent it was local to;
   * only compared, never dereferenced) and the offset from the grab point. */
  bool following = false;
  /** Item Only (Drag Look): a copy of the item follows the pointer, the element stays. */
  bool drag_picture = false;
  const SG_Node *drag_parent = nullptr;
  MT_Vector3 drag_home = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 drag_home_world = MT_Vector3(0.0f, 0.0f, 0.0f);
  MT_Vector3 drag_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  /** Screen Anchor: the frame (left, right, bottom, top at depth 1) the element's position was
   * last placed for. It starts as the authored frame; a frame change keeps the position's offset
   * from the anchor in frame heights, wherever game logic moved it since. */
  bool anchor_initialized = false;
  bool anchor_perspective = false;
  float anchor_applied_frame[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  /** In a menu placed in its player's view: the menu's placement scale it was last placed for
   * (1: as authored), and the scale of its offset from the anchor then (relative to the authored
   * frame's height, anchor_authored_height). */
  float anchor_applied_scale = 1.0f;
  float anchor_applied_offset = 1.0f;
  float anchor_authored_height = 0.0f;
};

const LN_UIService::InteractionState LN_UIService::s_emptyInteractionState;
const LN_UIService::ViewState LN_UIService::s_emptyViewState;
const std::string LN_UIService::s_emptyContext;
uint64_t LN_UIService::s_menuRevision = 0;
int LN_UIService::s_menuPauseScenes = 0;
bool LN_UIService::s_menuPausedGame = false;

size_t LN_UIService::ListenerKeyHash::operator()(const ListenerKey &key) const
{
  size_t result = std::hash<const LN_RuntimeTree *>{}(key.tree);
  HashCombine(result, key.query_index);
  HashCombine(result, key.element);
  HashCombine(result, key.user);
  return result;
}

bool LN_UIService::IsValidListener(const ListenerKey &listener)
{
  return (listener.tree != nullptr && listener.query_index != LN_INVALID_INDEX) ||
         listener.element != nullptr;
}

size_t LN_UIService::RegistrationKeyHash::operator()(const RegistrationKey &key) const
{
  size_t result = ListenerKeyHash{}(key.listener);
  result ^= std::hash<KX_GameObject *>{}(key.object) + 0x9e3779b97f4a7c15ULL + (result << 6u) +
            (result >> 2u);
  return result;
}

LN_UIService::LN_UIService(LN_Manager &manager, KX_Scene &scene)
    : m_manager(manager), m_scene(scene)
{
}

LN_UIService::~LN_UIService()
{
  /* The game ends or the scene goes: the engine starts its next game unpaused anyway. */
  ReleaseMenuPause(false);
}

void LN_UIService::UpdateMenuPause()
{
  const bool wanted = std::any_of(m_sessions.begin(), m_sessions.end(), [](const auto &item) {
    return item.second.pauses_game;
  });
  if (wanted == m_menuPauseActive) {
    return;
  }
  if (!wanted) {
    ReleaseMenuPause(true);
    return;
  }
  m_menuPauseActive = true;
  if (s_menuPauseScenes++ == 0) {
    KX_KetsjiEngine *engine = KX_GetActiveEngine();
    /* Already paused by game logic: that pause is the game's, and stays when the menu closes. A
     * resume asked for in this update (the menu this one replaces) does not count as paused. */
    s_menuPausedGame = engine != nullptr && !engine->IsGamePauseRequested();
    if (s_menuPausedGame) {
      engine->SetGamePaused(true);
    }
  }
}

void LN_UIService::ReleaseMenuPause(const bool resume)
{
  if (!m_menuPauseActive) {
    return;
  }
  m_menuPauseActive = false;
  if (s_menuPauseScenes > 0 && --s_menuPauseScenes == 0) {
    KX_KetsjiEngine *engine = resume ? KX_GetActiveEngine() : nullptr;
    if (s_menuPausedGame && engine != nullptr) {
      engine->SetGamePaused(false);
    }
    s_menuPausedGame = false;
  }
}

LN_UIService::MenuHandle LN_UIService::AllocateMenuHandle()
{
  if (m_nextMenuHandle > int64_t(std::numeric_limits<MenuHandle>::max())) {
    return 0;
  }
  /* Handles are deliberately never recycled within a scene lifetime. This makes a repeated close
   * distinguishable from an arbitrary invalid integer without retaining an unbounded retired set.
   */
  return MenuHandle(m_nextMenuHandle++);
}

LN_UIService::MenuCommandResult LN_UIService::OpenMenu(
    const LN_RuntimeTree *owner_tree,
    KX_GameObject *owner,
    KX_Camera *camera,
    blender::Collection *collection,
    const LN_UIMenuRouting routing,
    std::string input_context,
    const LN_UIMenuOpenMode open_mode,
    const LN_UIMenuPresentation presentation,
    const LN_UIMenuBackBehavior back_behavior,
    const bool acquire_resource,
    const int32_t user,
    const bool pauses_game,
    const bool toggles,
    KX_GameObject *menu_owner,
    const bool in_player_view)
{
  if (owner_tree == nullptr || owner == nullptr ||
      m_manager.IsGameObjectQueuedForRemoval(owner))
  {
    return {LN_UIResult::InvalidOwner, 0};
  }
  if (user < 0 || user >= LN_MAX_GAMEPADS) {
    return {LN_UIResult::InvalidTarget, 0};
  }
  if (owner->GetScene() != &m_scene) {
    return {LN_UIResult::WrongScene, 0};
  }
  if (collection == nullptr) {
    return {LN_UIResult::InvalidCollection, 0};
  }
  /* Toggle: the key or button that opened the menu closes it again, with the menus its player
   * opened on top of it (Settings over the inventory). */
  if (toggles) {
    /* Each player toggles their own: another player's open copy of it does not count. */
    if (const MenuHandle open = FindSessionForCollection(collection, user); open != 0) {
      const int32_t open_user = user;
      for (int top = TopMenuIndex(open_user); top >= 0 && m_menuStack[size_t(top)] != open;
           top = TopMenuIndex(open_user))
      {
        const MenuHandle above = m_menuStack[size_t(top)];
        CloseMenu(above, LN_UIMenuCloseOperation::Specific);
        /* A close already running (re-entered from object removal) keeps it on the stack. */
        if (std::find(m_menuStack.begin(), m_menuStack.end(), above) != m_menuStack.end()) {
          break;
        }
      }
      const MenuCommandResult closed = CloseMenu(open, LN_UIMenuCloseOperation::Specific);
      return {closed.result == LN_UIResult::Success ? LN_UIResult::Closed : closed.result, open};
    }
  }
  if (camera == nullptr) {
    camera = m_scene.GetOverlayCameraSource();
  }
  if (camera == nullptr) {
    return {LN_UIResult::InvalidCamera, 0};
  }
  if (camera->GetScene() != &m_scene) {
    return {LN_UIResult::WrongScene, 0};
  }
  if (m_manager.IsGameObjectQueuedForRemoval(camera)) {
    return {LN_UIResult::CameraUnavailable, 0};
  }
  if (m_scene.GetOverlayCameraSource() != nullptr &&
      !m_scene.OverlayCameraMatches(camera) &&
      m_scene.GetOverlayCameraSource() != camera)
  {
    return {LN_UIResult::CameraConflict, 0};
  }

  if (input_context.empty()) {
    input_context = GetActiveInputContext(user);
  }

  const int top_index = TopMenuIndex(user);
  if (top_index >= 0) {
    const auto top = m_sessions.find(m_menuStack[size_t(top_index)]);
    if (top != m_sessions.end() && top->second.collection == collection) {
      if (MenuSettingsMatch(top->second,
                            camera,
                            collection,
                            routing,
                            presentation,
                            back_behavior,
                            input_context))
      {
        return {LN_UIResult::NoChange, top->second.handle};
      }
      return {LN_UIResult::SessionConflict, top->second.handle};
    }
  }
  if (const MenuHandle existing = FindSessionForCollection(collection, user); existing != 0) {
    return {LN_UIResult::AlreadyOpen, existing};
  }
  /* Whose menu it is. A menu opened from inside another menu (Settings from the inventory) is
   * that menu's owner's. Found before a replaced menu closes. */
  KX_GameObject *menu_player = menu_owner;
  if (menu_player == nullptr) {
    menu_player = MenuOwnerOfObject(owner);
  }
  if (menu_player == nullptr || m_manager.IsGameObjectQueuedForRemoval(menu_player)) {
    menu_player = owner;
  }

  /* Replace Top replaces the player's top menu that takes their input, never a HUD opened as a
   * menu (which stays below the new menu). */
  const int replaced_index = open_mode == LN_UIMenuOpenMode::ReplaceTop ? InputMenuIndex(user) :
                                                                         -1;
  const MenuHandle replaced_handle = replaced_index >= 0 ? m_menuStack[size_t(replaced_index)] :
                                                           0;
  const MenuHandle handle = AllocateMenuHandle();
  if (handle == 0) {
    return {LN_UIResult::HandleExhausted, 0};
  }

  int32_t instance = 0;
  if (acquire_resource) {
    /* Another player has this menu open: this player gets a copy of their own. */
    const bool shared = FindSessionForCollection(collection) != 0;
    const KX_OverlayCollectionResult overlay_result = m_scene.AcquireOverlayMenuLease(
        camera, collection, handle, true);
    const LN_UIResult result = LN_MapOverlayResult(overlay_result);
    if (!KX_OverlayCollectionResultSucceeded(overlay_result)) {
      return {result, 0};
    }
    /* Copy 0 can be gone with a player who closed while another player's copy stayed. */
    if (shared || !m_scene.OverlayHasCopyZero(collection)) {
      instance = m_scene.AddOverlayInstance(collection, handle);
      if (instance < 0) {
        m_scene.ReleaseOverlayMenuLease(collection, handle);
        return {LN_MapOverlayResult(KX_OverlayCollectionResult::ObjectInstantiationFailed), 0};
      }
    }
    else {
      m_scene.SetOverlayInstanceLease(collection, 0, handle);
    }
  }

  /* A tab button inside the replaced menu opens its successor: the new menu takes over the
   * replaced menu's owner, else it would close with the button's tree. */
  const LN_RuntimeTree *session_owner_tree = owner_tree;
  KX_GameObject *session_owner = owner;
  if (replaced_handle != 0) {
    const auto replaced = m_sessions.find(replaced_handle);
    KX_GameObject *inherited = replaced != m_sessions.end() ?
                                   m_manager.ResolveObjectHandle(replaced->second.owner) :
                                   nullptr;
    if (inherited != nullptr && replaced->second.owner_tree != nullptr &&
        !m_manager.IsGameObjectQueuedForRemoval(inherited) &&
        OwnerLivesOnlyOnClosingCollection(owner, replaced->second))
    {
      session_owner = inherited;
      session_owner_tree = replaced->second.owner_tree;
    }
    CloseMenu(replaced_handle, LN_UIMenuCloseOperation::Specific);
  }

  MenuSession session;
  session.handle = handle;
  session.user = user;
  session.owner_tree = session_owner_tree;
  session.owner = m_manager.MakeObjectHandle(session_owner, session_owner->GetName());
  session.camera = camera;
  session.collection = collection;
  session.routing = routing;
  session.open_mode = open_mode;
  session.presentation = presentation;
  session.back_behavior = back_behavior;
  session.pauses_game = pauses_game;
  session.opened_frame = m_frameIndex;
  session.opened_in_frame_update = m_event == LN_Event::OnUpdate && !m_passive;
  session.input_context = std::move(input_context);
  session.previous_focus = m_focusByUser;
  session.instance = instance;
  session.owner_object = m_manager.MakeObjectHandle(menu_player, menu_player->GetName());
  session.in_player_view = in_player_view;
  SnapshotSessionObjects(session);
  m_sessions.emplace(handle, std::move(session));
  m_menuStack.push_back(handle);
  m_menuIndexValid = false;
  /* In its player's view from the first frame it shows. */
  PlaceMenuInPlayerView(m_sessions[handle]);
  NoteMenuStackChanged(user);
  RecomputeCoverage();
  UpdateMenuPause();
  /* The click that opened player 0's menu does not also click in it (other players have no
   * mouse). */
  if (user == 0 && m_input != nullptr) {
    if (SCA_IInputDevice *device = m_input->GetDevice()) {
      device->ConsumePressTransition(SCA_IInputDevice::LEFTMOUSE);
    }
  }
  return {LN_UIResult::Success, handle};
}

LN_UIService::MenuCommandResult LN_UIService::CloseMenu(const MenuHandle handle,
                                                        const LN_UIMenuCloseOperation operation,
                                                        const int32_t user)
{
  if (operation == LN_UIMenuCloseOperation::All) {
    LN_UIResult result = TopMenuIndex(user) < 0 ? LN_UIResult::NoChange : LN_UIResult::Success;
    for (int top = TopMenuIndex(user); top >= 0; top = TopMenuIndex(user)) {
      const MenuHandle closing = m_menuStack[size_t(top)];
      const MenuCommandResult closed = CloseMenu(closing, LN_UIMenuCloseOperation::Specific);
      if (!closed.Succeeded() && result == LN_UIResult::Success) {
        result = closed.result;
      }
      /* A close that is already running (re-entered from object removal) leaves the menu on the
       * stack until it finishes: do not spin on it. */
      if (std::find(m_menuStack.begin(), m_menuStack.end(), closing) != m_menuStack.end()) {
        break;
      }
    }
    return {result, 0};
  }

  MenuHandle target = handle;
  if (operation == LN_UIMenuCloseOperation::Top) {
    /* The menu the player is in: a HUD opened as a menu is not one. */
    const int top = InputMenuIndex(user);
    if (top < 0) {
      return {LN_UIResult::NoChange, 0};
    }
    target = m_menuStack[size_t(top)];
  }

  if (m_closingMenus.contains(target)) {
    return {LN_UIResult::NoChange, target};
  }
  const auto session_it = m_sessions.find(target);
  if (session_it == m_sessions.end()) {
    const bool was_allocated = target > 0 && int64_t(target) < m_nextMenuHandle;
    return {was_allocated ? LN_UIResult::NoChange : LN_UIResult::InvalidHandle, target};
  }

  const MenuSession session = session_it->second;
  m_closingMenus.insert(target);

  /* Restore focus and cancel presses while the menu's objects still exist: releasing the lease
   * below can delete them at once, which would also drop the focus that must be restored. */
  RestoreFocusAfterMenuClose(session);
  RebaseFocusHistoryAfterMenuClose(session);
  for (size_t phase = 0; phase < 3; ++phase) {
    std::vector<int32_t> cancelled_users;
    for (const auto &pointer : m_pointerStates[phase]) {
      if (pointer.second.has_capture &&
          SessionContainsHandle(session, pointer.second.captured_object) &&
          !OtherSessionContainsHandle(target, pointer.second.captured_object))
      {
        cancelled_users.push_back(pointer.first);
      }
    }
    for (const int32_t user : cancelled_users) {
      CancelPointer(phase, user, true);
    }
  }
  /* The player's copy goes with their menu, also while the overlay stays for other players' menus
   * (copy 0 stays only while Set Overlay Collection also shows it). */
  if (m_scene.OverlayWouldExistWithoutLease(session.collection, target)) {
    if (session.instance != 0 || !m_scene.OverlayOpenedIndependently(session.collection)) {
      m_scene.RemoveOverlayInstance(session.collection, session.instance);
    }
    else {
      /* It stays as the overlay's own: back where it was authored, for the whole window. */
      if (const auto closing = m_sessions.find(target); closing != m_sessions.end()) {
        PlaceMenuInPlayerView(closing->second, true);
      }
      m_scene.SetOverlayInstanceLease(session.collection, 0, 0);
    }
  }
  m_scene.ReleaseOverlayMenuLease(session.collection, target);

  m_menuStack.erase(std::remove(m_menuStack.begin(), m_menuStack.end(), target),
                    m_menuStack.end());
  m_sessions.erase(target);
  m_closingMenus.erase(target);
  NoteMenuStackChanged(session.user);
  RecomputeCoverage();
  UpdateMenuPause();
  return {LN_UIResult::Success, target};
}

void LN_UIService::CloseMenusOwnedBy(KX_GameObject *owner)
{
  if (owner == nullptr) {
    return;
  }
  std::vector<MenuHandle> handles;
  handles.reserve(m_sessions.size());
  for (const auto &item : m_sessions) {
    if (m_manager.ResolveObjectHandle(item.second.owner) == owner) {
      handles.push_back(item.first);
    }
  }
  std::sort(handles.begin(), handles.end(), [this](const MenuHandle a, const MenuHandle b) {
    const auto ai = std::find(m_menuStack.begin(), m_menuStack.end(), a);
    const auto bi = std::find(m_menuStack.begin(), m_menuStack.end(), b);
    return ai > bi;
  });
  for (const MenuHandle handle : handles) {
    CloseMenu(handle);
  }
}

size_t LN_UIService::PhaseIndex(const LN_Event event)
{
  return std::min<size_t>(size_t(event), 2);
}

void LN_UIService::BeginTick(const LN_Event event,
                             const uint64_t tick_index,
                             const LN_InputSnapshot *input_snapshot,
                             const double unscaled_delta_time)
{
  m_event = event;
  m_tickIndex = tick_index;
  m_input = input_snapshot;
  m_passive = false;
  m_frameDelta = std::max(0.0, unscaled_delta_time);
  m_frameIndex++;
  m_frameNavigation.clear();
  m_frameScroll.clear();
  /* ResolveViews collects them again (a frame without views has none). */
  m_wheelSurfaces.clear();
  const float stick_threshold = LN_InputMap::Get().StickThreshold();
  for (int32_t gamepad_index = 0; gamepad_index < LN_MAX_GAMEPADS; ++gamepad_index) {
    const LN_GamepadSnapshot *gamepad = m_input != nullptr ? m_input->GetGamepad(gamepad_index) :
                                                             nullptr;
    for (int stick = 0; stick < 2; ++stick) {
      const StickDirection previous = m_stickDirection[size_t(gamepad_index)][size_t(stick)];
      StickDirection direction = StickDirection::None;
      if (gamepad != nullptr && gamepad->connected) {
        /* x right, y up. */
        const MT_Vector3 position = LN_GamepadStickPosition(*gamepad, stick + 1);
        const float x = position.x();
        const float y = position.y();
        const auto amount = [&](const StickDirection candidate) {
          switch (candidate) {
            case StickDirection::Up:
              return y;
            case StickDirection::Down:
              return -y;
            case StickDirection::Left:
              return -x;
            case StickDirection::Right:
              return x;
            case StickDirection::None:
              break;
          }
          return 0.0f;
        };
        /* One direction at a time: the one pushed furthest. */
        if (previous != StickDirection::None &&
            amount(previous) >= stick_threshold * kStickRelease)
        {
          direction = previous;
        }
        else if (std::abs(x) >= std::abs(y)) {
          direction = x >= stick_threshold ?
                          StickDirection::Right :
                          (x <= -stick_threshold ? StickDirection::Left : StickDirection::None);
        }
        else {
          direction = y >= stick_threshold ?
                          StickDirection::Up :
                          (y <= -stick_threshold ? StickDirection::Down : StickDirection::None);
        }
      }
      /* A stick held while the UI had no input (a rebind took it) waits for the centre. */
      bool &waits = m_stickWaitsForCentre[size_t(gamepad_index)][size_t(stick)];
      if (m_input == nullptr) {
        waits = true;
      }
      else if (waits && direction == StickDirection::None) {
        waits = false;
      }
      if (waits) {
        direction = StickDirection::None;
      }
      m_previousStickDirection[size_t(gamepad_index)][size_t(stick)] = previous;
      m_stickDirection[size_t(gamepad_index)][size_t(stick)] = direction;
    }
  }
  if (m_input == nullptr ||
      !(ActionBindingState(LN_UIAction::Cancel,
                           LN_UIActionState::Triggered,
                           0,
                           nullptr,
                           BindingFilter::All) ||
        ActionBindingState(
            LN_UIAction::Cancel, LN_UIActionState::Active, 0, nullptr, BindingFilter::All)))
  {
    m_cancelConsumed = false;
  }
  for (int32_t user = 1; user < LN_MAX_GAMEPADS; ++user) {
    const uint8_t bit = uint8_t(1u << user);
    if ((m_playerBackUsed & bit) != 0 &&
        (m_input == nullptr ||
         !(ActionBindingState(LN_UIAction::Cancel,
                              LN_UIActionState::Triggered,
                              user,
                              nullptr,
                              BindingFilter::All) ||
           ActionBindingState(
               LN_UIAction::Cancel, LN_UIActionState::Active, user, nullptr, BindingFilter::All))))
    {
      m_playerBackUsed &= uint8_t(~bit);
    }
  }
  m_registrations.clear();
  m_listenerDeclarations.clear();
  m_registeredListeners.clear();
  m_registeredTargets.clear();
  m_viewRegistrations.clear();
  m_viewSlotsVersion++;
  m_currentStates.clear();
  m_currentViewStates.clear();
  m_viewNavigationHandledUsers.clear();
  m_resolving = false;
  m_elementEvents.clear();
  /* A Back that closed a menu before this frame (m_backSounds) is still told by this one. */
  m_soundEvents.clear();
  for (ElementRecord &record : m_elements) {
    record.registered = false;
    record.users = 0;
  }
  for (auto &item : m_sessions) {
    RefreshSessionMembership(item.second);
  }
  InvalidateMenuIndex();
}

void LN_UIService::BeginPassivePhase(const LN_Event event, const LN_InputSnapshot *input_snapshot)
{
  m_event = event;
  m_input = input_snapshot;
  m_passive = true;
}

bool LN_UIService::NavigationOrderLess(const Registration &a, const Registration &b)
{
  if (a.tab_order != b.tab_order) {
    return a.tab_order < b.tab_order;
  }
  if (a.priority != b.priority) {
    return a.priority > b.priority;
  }
  if (a.screen_position.y() != b.screen_position.y()) {
    return a.screen_position.y() < b.screen_position.y();
  }
  return a.screen_position.x() < b.screen_position.x();
}

bool LN_UIService::IsDescendantOf(KX_GameObject *object, KX_GameObject *ancestor)
{
  if (object == nullptr || ancestor == nullptr) {
    return false;
  }
  for (KX_GameObject *current = object; current != nullptr; current = current->GetParent()) {
    if (current == ancestor) {
      return true;
    }
  }
  return false;
}

bool LN_UIService::IsBeingDragged(KX_GameObject *object, KX_GameObject *root) const
{
  if (object == nullptr) {
    return false;
  }
  for (const auto &phase : m_pointerStates) {
    for (const auto &pointer : phase) {
      if (!pointer.second.has_capture || !pointer.second.drag_active) {
        continue;
      }
      KX_GameObject *dragged = m_manager.ResolveObjectHandle(pointer.second.captured_object);
      if (IsDescendantOf(object, dragged) && IsDescendantOf(dragged, root)) {
        return true;
      }
    }
  }
  return false;
}

bool LN_UIService::HasAnchoredAncestor(KX_GameObject *object)
{
  for (KX_GameObject *parent = object != nullptr ? object->GetParent() : nullptr;
       parent != nullptr;
       parent = parent->GetParent())
  {
    const ElementRecord *record = FindElementRecord(parent);
    if (record != nullptr && record->settings.anchor != LN_UIElementAnchor::None) {
      return true;
    }
  }
  return false;
}

bool LN_UIService::RayIntersectsView(const ViewRegistration &view)
{
  Registration registration;
  registration.ray_from = view.ray_from;
  registration.ray_to = view.ray_to;
  registration.bounds_min = view.bounds_min;
  registration.bounds_max = view.bounds_max;
  registration.world_position = view.world_position;
  registration.world_orientation = view.world_orientation;
  registration.world_scale = view.world_scale;
  registration.padding = view.padding;
  float fraction = 1.0f;
  MT_Vector3 point;
  MT_Vector3 normal;
  return RayIntersectsRegistration(registration, fraction, point, normal);
}

void LN_UIService::RegisterView(ViewRegistration registration)
{
  if (m_resolving || registration.listener.tree == nullptr ||
      registration.listener.query_index == LN_INVALID_INDEX || registration.viewport == nullptr ||
      registration.camera == nullptr || registration.viewport->GetScene() != &m_scene ||
      registration.camera->GetScene() != &m_scene ||
      m_manager.IsGameObjectQueuedForRemoval(registration.viewport) ||
      m_manager.IsGameObjectQueuedForRemoval(registration.camera) ||
      !IsFinite(registration.ray_from) ||
      !IsFinite(registration.ray_to) || !IsFinite(registration.bounds_min) ||
      !IsFinite(registration.bounds_max) || !IsFinite(registration.world_position) ||
      !IsFinite(registration.world_orientation) || !IsFinite(registration.world_scale) ||
      !IsFinite(registration.min_offset) || !IsFinite(registration.max_offset) ||
      !IsFinite(registration.wheel_step) || !std::isfinite(registration.padding) ||
      registration.bounds_min.x() > registration.bounds_max.x() ||
      registration.bounds_min.y() > registration.bounds_max.y() ||
      registration.bounds_min.z() > registration.bounds_max.z())
  {
    return;
  }
  /* A view in a menu belongs to the menu's player, like the menu's UI Elements. */
  if (const int menu_index = MenuStackIndexForObject(registration.viewport); menu_index >= 0) {
    registration.user = MenuUser(menu_index);
  }
  if (registration.kind == ViewKind::Scroll &&
      (registration.content == nullptr || registration.content->GetScene() != &m_scene ||
       m_manager.IsGameObjectQueuedForRemoval(registration.content) ||
       registration.content == registration.viewport ||
       IsDescendantOf(registration.viewport, registration.content)))
  {
    return;
  }
  if (std::any_of(
          m_viewRegistrations.begin(),
          m_viewRegistrations.end(),
          [&](const ViewRegistration &item) { return item.listener == registration.listener; }))
  {
    return;
  }
  if (registration.slot_template != nullptr &&
      (registration.slot_template->GetScene() != &m_scene ||
       registration.slot_template == registration.viewport ||
       m_manager.IsGameObjectQueuedForRemoval(registration.slot_template) ||
       !IsFinite(registration.spacing)))
  {
    registration.slot_template = nullptr;
  }
  if (registration.scrollbar != nullptr &&
      (registration.scrollbar->GetScene() != &m_scene ||
       registration.scrollbar == registration.viewport ||
       registration.scrollbar == registration.content ||
       m_manager.IsGameObjectQueuedForRemoval(registration.scrollbar) ||
       !IsFinite(registration.scrollbar_min) || !IsFinite(registration.scrollbar_max) ||
       !IsFinite(registration.handle_min) || !IsFinite(registration.handle_max)))
  {
    registration.scrollbar = nullptr;
  }
  if (registration.scrollbar == nullptr ||
      (registration.scrollbar_handle != nullptr &&
       (registration.scrollbar_handle->GetParent() != registration.scrollbar ||
        m_manager.IsGameObjectQueuedForRemoval(registration.scrollbar_handle))))
  {
    registration.scrollbar_handle = nullptr;
  }
  GridInventory &inventory = registration.inventory;
  if (registration.list_object == nullptr) {
    inventory.handle_moves = false;
  }
  if (!std::isfinite(inventory.distance) || !std::isfinite(inventory.hover_height)) {
    inventory.drop_into_world = false;
    inventory.drop_in_front = false;
  }
  if (inventory.hint_text != nullptr &&
      (inventory.hint_text->GetScene() != &m_scene ||
       m_manager.IsGameObjectQueuedForRemoval(inventory.hint_text)))
  {
    inventory.hint_text = nullptr;
  }
  const ListenerKey listener = registration.listener;
  m_viewRegistrations.push_back(std::move(registration));
  m_viewSlotsVersion++;
  m_currentViewStates.emplace(listener, ViewState());
}

void LN_UIService::ReleaseView(const ListenerKey &listener)
{
  const auto state_it = m_viewStates.find(listener);
  if (state_it != m_viewStates.end()) {
    RemoveTemplateCopies(state_it->second);
    RestoreScrollbarHandle(state_it->second);
    const std::vector<ViewPersistentState::ManagedObject> managed_objects =
        state_it->second.managed_objects;
    const LN_ObjectHandle content_handle = state_it->second.content;
    const MT_Vector3 base_local_position = state_it->second.base_local_position;
    const bool had_content = state_it->second.initialized && content_handle.IsValid();
    m_viewStates.erase(state_it);
    if (had_content) {
      if (KX_GameObject *content = m_manager.ResolveObjectHandle(content_handle)) {
        content->NodeSetLocalPosition(base_local_position);
        content->NodeUpdateGS(0.0);
      }
    }
    for (const ViewPersistentState::ManagedObject &managed : managed_objects) {
      KX_GameObject *object = m_manager.ResolveObjectHandle(managed.object);
      if (object != nullptr) {
        ReleaseViewVisibility(listener, object);
      }
    }
  }
  for (auto binding = m_slotBindings.begin(); binding != m_slotBindings.end();) {
    binding = binding->second.view == listener ? m_slotBindings.erase(binding) :
                                                 std::next(binding);
  }
}

void LN_UIService::AcquireViewVisibility(const ListenerKey &listener, KX_GameObject *object)
{
  if (object == nullptr) {
    return;
  }
  auto [state, inserted] = m_viewVisibilityStates.try_emplace(object);
  if (inserted) {
    state->second.object = m_manager.MakeObjectHandle(object, object->GetName());
  }
  state->second.owners.insert(listener);
}

void LN_UIService::ReleaseViewVisibility(const ListenerKey &listener, KX_GameObject *object)
{
  const auto state = m_viewVisibilityStates.find(object);
  if (state == m_viewVisibilityStates.end()) {
    return;
  }
  state->second.owners.erase(listener);
  if (!state->second.owners.empty()) {
    return;
  }
  if (m_manager.ResolveObjectHandle(state->second.object) == object) {
    object->SetViewClipVisible(true);
  }
  m_viewVisibilityStates.erase(state);
}

bool LN_UIService::RayHitsScrollbar(const ViewRegistration &view)
{
  if (view.scrollbar == nullptr || !view.scrollbar->GetVisible()) {
    return false;
  }
  float fraction = 1.0f;
  MT_Vector3 point;
  MT_Vector3 normal;
  return RayIntersectsRegistration(ScrollbarRegistration(view), fraction, point, normal);
}

bool LN_UIService::ScrollbarDragProgress(const ViewRegistration &view,
                                         ViewPersistentState &persistent,
                                         const float fraction,
                                         float &r_progress)
{
  /* The pointer system captured the press on the bar (the frame before); follow it until the
   * release. */
  const auto &pointers = m_pointerStates[PhaseIndex(m_event)];
  const auto pointer = pointers.find(0);
  const ScrollbarLayout layout = MakeScrollbarLayout(view, fraction);
  if (!layout.valid || pointer == pointers.end() || !pointer->second.has_capture ||
      !(pointer->second.captured_listener == ScrollbarListener(view)))
  {
    persistent.scrollbar_dragging = false;
    return false;
  }
  if (!persistent.scrollbar_dragging) {
    /* A press on the handle keeps its grip there; a press beside it brings the handle under the
     * pointer. */
    MT_Vector3 press;
    if (!WorldToObjectSpace(view.scrollbar, pointer->second.capture_point, press)) {
      return false;
    }
    const float offset = ScrollbarAlong(view, layout, press) - persistent.handle_center;
    persistent.scrollbar_grab = std::abs(offset) <= layout.handle_length * 0.5f ? offset : 0.0f;
    persistent.scrollbar_plane = press[layout.normal];
    persistent.scrollbar_dragging = true;
  }
  r_progress = persistent.scrollbar_progress;
  /* Follow the pointer on the bar's plane, also once it leaves the bar. */
  MT_Vector3 from;
  MT_Vector3 to;
  if (!WorldToObjectSpace(view.scrollbar, view.ray_from, from) ||
      !WorldToObjectSpace(view.scrollbar, view.ray_to, to))
  {
    return true;
  }
  const float depth = to[layout.normal] - from[layout.normal];
  if (std::abs(depth) <= kScaleEpsilon) {
    return true;
  }
  const MT_Vector3 point = from + (to - from) *
                                      ((persistent.scrollbar_plane - from[layout.normal]) / depth);
  const float travel = layout.length - layout.handle_length;
  const float progress = travel > kScaleEpsilon ?
                             (ScrollbarAlong(view, layout, point) - persistent.scrollbar_grab -
                              layout.handle_length * 0.5f) /
                                 travel :
                             0.0f;
  if (std::isfinite(progress)) {
    r_progress = std::clamp(progress, 0.0f, 1.0f);
  }
  return true;
}

void LN_UIService::UpdateScrollbar(const ViewRegistration &view,
                                   ViewPersistentState &persistent,
                                   const float fraction,
                                   const float progress)
{
  const ScrollbarLayout layout = MakeScrollbarLayout(view, fraction);
  if (!layout.valid) {
    return;
  }
  KX_GameObject *handle = view.scrollbar_handle;
  if (m_manager.ResolveObjectHandle(persistent.scrollbar_handle) != handle) {
    RestoreScrollbarHandle(persistent);
    if (handle != nullptr && HandleInBarSpace(view.scrollbar,
                                              handle,
                                              persistent.handle_bar_position,
                                              persistent.handle_bar_scale))
    {
      persistent.scrollbar_handle = m_manager.MakeObjectHandle(handle, handle->GetName());
      persistent.handle_base_position = handle->NodeGetLocalPosition();
      persistent.handle_base_scale = handle->NodeGetLocalScaling();
    }
  }
  persistent.scrollbar_progress = std::clamp(progress, 0.0f, 1.0f);
  persistent.handle_center = layout.handle_length * 0.5f +
                             persistent.scrollbar_progress *
                                 (layout.length - layout.handle_length);
  if (handle != nullptr && m_manager.ResolveObjectHandle(persistent.scrollbar_handle) == handle) {
    /* Size and place the handle in the bar's space, then set it in world space. */
    MT_Vector3 scale = persistent.handle_bar_scale;
    const float extent = view.handle_max[layout.axis] - view.handle_min[layout.axis];
    if (extent > kScaleEpsilon) {
      scale[layout.axis] = std::copysign(layout.handle_length / extent, scale[layout.axis]);
    }
    const float center = layout.axis == 0 ?
                             view.scrollbar_min.x() + persistent.handle_center :
                             view.scrollbar_max[layout.axis] - persistent.handle_center;
    MT_Vector3 position = persistent.handle_bar_position;
    position[layout.axis] = center - (view.handle_min[layout.axis] +
                                      view.handle_max[layout.axis]) *
                                         0.5f * scale[layout.axis];
    const MT_Vector3 bar_scale = view.scrollbar->NodeGetWorldScaling();
    const MT_Vector3 world_position = view.scrollbar->NodeGetWorldPosition() +
                                      view.scrollbar->NodeGetWorldOrientation() *
                                          MT_Vector3(position.x() * bar_scale.x(),
                                                     position.y() * bar_scale.y(),
                                                     position.z() * bar_scale.z());
    const MT_Vector3 world_scale(
        scale.x() * bar_scale.x(), scale.y() * bar_scale.y(), scale.z() * bar_scale.z());
    if ((world_position - handle->NodeGetWorldPosition()).length2() > kScaleEpsilon ||
        (world_scale - handle->NodeGetWorldScaling()).length2() > kScaleEpsilon)
    {
      handle->NodeSetWorldScale(world_scale);
      handle->NodeSetWorldPosition(world_position);
      handle->NodeUpdateGS(0.0);
    }
  }

  /* A blocking control: a press on it is the bar's alone, never a slot's or the game's. */
  Registration registration = ScrollbarRegistration(view);
  registration.listener = ScrollbarListener(view);
  registration.user = view.user;
  registration.priority = view.priority;
  registration.propagation = LN_UIPropagation::Block;
  registration.role = LN_UIInteractableRole::Control;
  registration.interaction_domain = view.overlay ? LN_InteractionDomain::Overlay :
                                                   LN_InteractionDomain::Main;
  registration.capture_on_press = true;
  registration.focus_on_press = false;
  registration.focusable = false;
  registration.navigable = false;
  RegisterInteractable(std::move(registration));
}

void LN_UIService::RestoreScrollbarHandle(ViewPersistentState &persistent)
{
  KX_GameObject *handle = m_manager.ResolveObjectHandle(persistent.scrollbar_handle);
  if (handle != nullptr && handle->GetSGNode() != nullptr) {
    handle->NodeSetLocalScale(persistent.handle_base_scale);
    handle->NodeSetLocalPosition(persistent.handle_base_position);
    handle->NodeUpdateGS(0.0);
  }
  persistent.scrollbar_handle = LN_ObjectHandle();
}

void LN_UIService::ResolveViews()
{
  if (m_resolving) {
    return;
  }

  std::unordered_set<ListenerKey, ListenerKeyHash> current;
  current.reserve(m_viewRegistrations.size());
  for (const ViewRegistration &view : m_viewRegistrations) {
    current.insert(view.listener);
  }
  std::vector<ListenerKey> stale;
  for (const auto &item : m_viewStates) {
    if (!current.contains(item.first)) {
      stale.push_back(item.first);
    }
  }
  for (const ListenerKey &listener : stale) {
    ReleaseView(listener);
  }
  /* Scroll Object View requests wait for their view to lay out, but not for one that is gone. */
  std::erase_if(m_viewScrollRequests, [&](const ViewScrollRequest &request) {
    return request.frame + 2 < m_frameIndex ||
           m_manager.ResolveObjectHandle(request.viewport) == nullptr;
  });
  /* The requests for `view`: every view whose Viewport or Content they name takes them. */
  const auto take_scroll_requests = [&](const ViewRegistration &view) {
    std::vector<ViewScrollRequest> taken;
    for (ViewScrollRequest &request : m_viewScrollRequests) {
      KX_GameObject *object = m_manager.ResolveObjectHandle(request.viewport);
      if (object != nullptr &&
          (object == view.viewport || (view.content != nullptr && object == view.content)))
      {
        taken.push_back(request);
        request.used = true;
      }
    }
    return taken;
  };
  SweepDragGhosts();
  RemoveQueuedTemplateCopies();
  for (ViewRegistration &view : m_viewRegistrations) {
    if (view.kind == ViewKind::Grid && view.enabled && view.slot_template != nullptr &&
        view.rows > 0)
    {
      view.slots = TemplateSlots(view);
      m_viewSlotsVersion++;
    }
  }

  std::unordered_map<KX_GameObject *, const ViewRegistration *> scroll_content_owner;
  std::unordered_map<KX_GameObject *, const ViewRegistration *> grid_slot_owner;
  for (const ViewRegistration &candidate : m_viewRegistrations) {
    if (!candidate.enabled) {
      continue;
    }
    if (candidate.kind == ViewKind::Scroll) {
      const auto existing = scroll_content_owner.find(candidate.content);
      if (existing == scroll_content_owner.end()) {
        scroll_content_owner.emplace(candidate.content, &candidate);
        continue;
      }
      const int candidate_menu = MenuStackIndexForObject(candidate.viewport);
      const int existing_menu = MenuStackIndexForObject(existing->second->viewport);
      if (candidate_menu > existing_menu ||
          (candidate_menu == existing_menu && candidate.priority > existing->second->priority))
      {
        existing->second = &candidate;
      }
      continue;
    }
    for (KX_GameObject *slot : candidate.slots) {
      if (slot == nullptr || slot == candidate.viewport || slot->GetScene() != &m_scene ||
          m_manager.IsGameObjectQueuedForRemoval(slot))
      {
        continue;
      }
      const auto existing = grid_slot_owner.find(slot);
      if (existing == grid_slot_owner.end()) {
        grid_slot_owner.emplace(slot, &candidate);
        continue;
      }
      const int candidate_menu = MenuStackIndexForObject(candidate.viewport);
      const int existing_menu = MenuStackIndexForObject(existing->second->viewport);
      if (candidate_menu > existing_menu ||
          (candidate_menu == existing_menu && candidate.priority > existing->second->priority))
      {
        existing->second = &candidate;
      }
    }
  }

  const auto view_owns_runtime_objects = [&](const ViewRegistration &view) {
    if (view.kind == ViewKind::Scroll) {
      const auto owner = scroll_content_owner.find(view.content);
      return owner != scroll_content_owner.end() && owner->second == &view;
    }
    return std::any_of(view.slots.begin(), view.slots.end(), [&](KX_GameObject *slot) {
      const auto owner = grid_slot_owner.find(slot);
      return owner != grid_slot_owner.end() && owner->second == &view;
    });
  };

  /* A view on a hidden page (a tab not shown) shows nothing: it takes no wheel or stick. Its
   * bounds object may be invisible on purpose, so its slots or content tell. */
  const auto view_shown = [&](const ViewRegistration &view) {
    if (view.kind == ViewKind::Scroll) {
      return view.content != nullptr && view.content->GetVisibleIgnoringViewClip();
    }
    return std::any_of(view.slots.begin(), view.slots.end(), [](KX_GameObject *slot) {
      return slot != nullptr && slot->GetVisibleIgnoringViewClip();
    });
  };

  const int wheel_delta = ScrollWheelSteps(m_input);
  const ViewRegistration *wheel_view = nullptr;
  if (m_input != nullptr) {
    for (const ViewRegistration &candidate : m_viewRegistrations) {
      if (!candidate.enabled || candidate.user != 0 || !view_owns_runtime_objects(candidate) ||
          !view_shown(candidate) ||
          !MenuIndexPassesFloor(MenuStackIndexForObject(candidate.viewport), 0) ||
          !ObjectPassesViewClipping(candidate.viewport))
      {
        continue;
      }
      /* Until the next frame, the wheel of input that reaches Logic + Physics steps first is
       * judged against these where its pointer is (ConsumeInputBeforeResolve). */
      PointerSurface surface;
      surface.object = m_manager.MakeObjectHandle(candidate.viewport,
                                                  candidate.viewport->GetName());
      surface.camera = m_manager.MakeObjectHandle(candidate.camera, candidate.camera->GetName());
      surface.bounds_min = candidate.bounds_min;
      surface.bounds_max = candidate.bounds_max;
      surface.padding = candidate.padding;
      surface.must_be_visible = false;
      m_wheelSurfaces.push_back(surface);
      if (candidate.scrollbar != nullptr && candidate.scrollbar->GetVisible()) {
        m_wheelSurfaces.push_back(MakePointerSurface(ScrollbarRegistration(candidate)));
      }
      if (!RayIntersectsView(candidate) && !RayHitsScrollbar(candidate)) {
        continue;
      }
      if (wheel_view == nullptr ||
          MenuStackIndexForObject(candidate.viewport) >
              MenuStackIndexForObject(wheel_view->viewport) ||
          (MenuStackIndexForObject(candidate.viewport) ==
               MenuStackIndexForObject(wheel_view->viewport) &&
           candidate.priority > wheel_view->priority))
      {
        wheel_view = &candidate;
      }
    }
    /* The wheel scrolls the view under the pointer, not the game camera as well. */
    if (wheel_view != nullptr && wheel_delta != 0) {
      m_manager.ConsumeWheel();
    }
  }
  /* Scroll Up and Scroll Down's other controls (the right stick) scroll the view that holds the
   * player's focus, else the one under the pointer, else the topmost one. */
  std::unordered_map<const ViewRegistration *, int32_t> directed_scroll;
  if (m_input != nullptr) {
    std::unordered_set<int32_t> view_users;
    for (const ViewRegistration &view : m_viewRegistrations) {
      if (view.enabled) {
        view_users.insert(view.user);
      }
    }
    for (const int32_t user : view_users) {
      /* Like navigation: only where keys and gamepad drive the UI (an open menu or a set focus),
       * not a HUD while the right stick turns the camera. */
      if (!DirectedInputAllowed(user)) {
        continue;
      }
      const int32_t step = ScrollStepForUser(user);
      if (step == 0) {
        continue;
      }
      const auto focus = m_focusByUser.find(user);
      KX_GameObject *focused = focus != m_focusByUser.end() ?
                                   m_manager.ResolveObjectHandle(focus->second) :
                                   nullptr;
      const ViewRegistration *target = nullptr;
      int target_rank = -1;
      for (const ViewRegistration &candidate : m_viewRegistrations) {
        if (!candidate.enabled || candidate.user != user ||
            !view_owns_runtime_objects(candidate) || !view_shown(candidate) ||
            !MenuIndexPassesFloor(MenuStackIndexForObject(candidate.viewport), user) ||
            !ObjectAcceptsDirectedInput(candidate.viewport, user) ||
            !ObjectPassesViewClipping(candidate.viewport))
        {
          continue;
        }
        const bool holds_focus =
            focused != nullptr &&
            (candidate.kind == ViewKind::Scroll ?
                 focused != candidate.content && IsDescendantOf(focused, candidate.content) :
                 std::find(candidate.slots.begin(), candidate.slots.end(), focused) !=
                     candidate.slots.end());
        const bool under_pointer = user == 0 && (RayIntersectsView(candidate) ||
                                                 RayHitsScrollbar(candidate));
        const int rank = holds_focus ? 2 : (under_pointer ? 1 : 0);
        if (target == nullptr || rank > target_rank ||
            (rank == target_rank && (MenuStackIndexForObject(candidate.viewport) >
                                         MenuStackIndexForObject(target->viewport) ||
                                     (MenuStackIndexForObject(candidate.viewport) ==
                                          MenuStackIndexForObject(target->viewport) &&
                                      candidate.priority > target->priority))))
        {
          target = &candidate;
          target_rank = rank;
        }
      }
      if (target != nullptr) {
        directed_scroll[target] += step;
      }
    }
  }

  m_slotBindings.clear();
  m_cellsShown.clear();
  std::unordered_map<KX_GameObject *, bool> desired_visibility;
  std::unordered_map<ListenerKey, std::unordered_set<KX_GameObject *>, ListenerKeyHash>
      managed_objects_by_view;
  for (const auto &view_state : m_viewStates) {
    std::unordered_set<KX_GameObject *> &managed_objects =
        managed_objects_by_view[view_state.first];
    managed_objects.reserve(view_state.second.managed_objects.size());
    for (const ViewPersistentState::ManagedObject &managed : view_state.second.managed_objects) {
      if (KX_GameObject *object = m_manager.ResolveObjectHandle(managed.object)) {
        managed_objects.insert(object);
      }
    }
  }
  const auto remember_visibility =
      [&](const ListenerKey &listener, ViewPersistentState &persistent, KX_GameObject *object) {
        if (managed_objects_by_view[listener].insert(object).second) {
          persistent.managed_objects.push_back(
              {m_manager.MakeObjectHandle(object, object->GetName())});
          AcquireViewVisibility(listener, object);
        }
      };
  const auto reconcile_visibility = [&](const ListenerKey &listener,
                                        ViewPersistentState &persistent,
                                        const std::unordered_set<KX_GameObject *> &active) {
    for (auto managed = persistent.managed_objects.begin();
         managed != persistent.managed_objects.end();)
    {
      KX_GameObject *object = m_manager.ResolveObjectHandle(managed->object);
      if (object != nullptr && active.contains(object)) {
        ++managed;
        continue;
      }
      if (object != nullptr) {
        ReleaseViewVisibility(listener, object);
        managed_objects_by_view[listener].erase(object);
      }
      managed = persistent.managed_objects.erase(managed);
    }
  };
  const auto request_visibility = [&](KX_GameObject *object, const bool visible) {
    const auto existing = desired_visibility.find(object);
    if (existing == desired_visibility.end()) {
      desired_visibility.emplace(object, visible);
    }
    else {
      /* Nested views combine clipping restrictions: every owning view must allow the object. */
      existing->second = existing->second && visible;
    }
  };
  /* A scrollbar shows only while the content does not fit, and then takes the pointer. */
  const auto show_scrollbar = [&](const ViewRegistration &view,
                                  ViewPersistentState &persistent,
                                  std::unordered_set<KX_GameObject *> &managed,
                                  const bool needed,
                                  const float fraction,
                                  const float progress) {
    if (view.scrollbar == nullptr) {
      RestoreScrollbarHandle(persistent);
      return;
    }
    std::vector<KX_GameObject *> objects = view.scrollbar->GetChildrenRecursive();
    objects.push_back(view.scrollbar);
    for (KX_GameObject *object : objects) {
      if (object != nullptr && !m_manager.IsGameObjectQueuedForRemoval(object)) {
        managed.insert(object);
        remember_visibility(view.listener, persistent, object);
        request_visibility(object, needed);
      }
    }
    if (needed) {
      UpdateScrollbar(view, persistent, fraction, progress);
    }
    else {
      persistent.scrollbar_dragging = false;
    }
  };
  for (const ViewRegistration &view : m_viewRegistrations) {
    ViewState &result = m_currentViewStates[view.listener];
    if (!view.enabled) {
      ReleaseView(view.listener);
      managed_objects_by_view.erase(view.listener);
      continue;
    }
    if (view.kind == ViewKind::Scroll) {
      const auto owner = scroll_content_owner.find(view.content);
      if (owner == scroll_content_owner.end() || owner->second != &view) {
        ReleaseView(view.listener);
        managed_objects_by_view.erase(view.listener);
        continue;
      }
      const LN_ObjectHandle content_handle = m_manager.MakeObjectHandle(view.content,
                                                                        view.content->GetName());
      const auto existing = m_viewStates.find(view.listener);
      if (existing != m_viewStates.end() && existing->second.initialized &&
          m_manager.ResolveObjectHandle(existing->second.content) != view.content)
      {
        ReleaseView(view.listener);
        managed_objects_by_view.erase(view.listener);
      }
      ViewPersistentState &persistent = m_viewStates[view.listener];
      if (!persistent.initialized) {
        persistent.initialized = true;
        persistent.content = content_handle;
        persistent.base_local_position = view.content->NodeGetLocalPosition();
      }
      result.valid = true;

      const MT_Vector3 minimum(std::min(view.min_offset.x(), view.max_offset.x()),
                               std::min(view.min_offset.y(), view.max_offset.y()),
                               std::min(view.min_offset.z(), view.max_offset.z()));
      const MT_Vector3 maximum(std::max(view.min_offset.x(), view.max_offset.x()),
                               std::max(view.min_offset.y(), view.max_offset.y()),
                               std::max(view.min_offset.z(), view.max_offset.z()));
      int32_t scroll_steps = wheel_view == &view ? wheel_delta : 0;
      if (const auto directed = directed_scroll.find(&view); directed != directed_scroll.end()) {
        scroll_steps += directed->second;
      }
      MT_Vector3 next = persistent.offset + view.wheel_step * float(scroll_steps);
      /* Scroll Object View. Start and end are those of At Start and At End (Normalized 0 and 1);
       * a page is the viewport's size. */
      bool commanded = false;
      KX_GameObject *show_object = nullptr;
      for (const ViewScrollRequest &request : take_scroll_requests(view)) {
        commanded = commanded || request.mode != LN_UIViewScrollMode::ShowItem;
        for (int axis = 0; axis < 2; ++axis) {
          if (!(axis == 0 ? view.horizontal : view.vertical)) {
            continue;
          }
          const bool reversed = view.wheel_step[axis] < 0.0f;
          const float toward_end = reversed ? -1.0f : 1.0f;
          KX_GameObject *parent = view.content->GetParent();
          const float parent_scale = parent != nullptr ?
                                         std::abs(parent->NodeGetWorldScaling()[axis]) :
                                         1.0f;
          const float page = std::abs((view.bounds_max[axis] - view.bounds_min[axis]) *
                                      view.world_scale[axis]) /
                             (parent_scale > kScaleEpsilon ? parent_scale : 1.0f);
          switch (request.mode) {
            case LN_UIViewScrollMode::Top:
              next[axis] = reversed ? maximum[axis] : minimum[axis];
              break;
            case LN_UIViewScrollMode::Bottom:
              next[axis] = reversed ? minimum[axis] : maximum[axis];
              break;
            case LN_UIViewScrollMode::NextPage:
              next[axis] += toward_end * page;
              break;
            case LN_UIViewScrollMode::PreviousPage:
              next[axis] -= toward_end * page;
              break;
            case LN_UIViewScrollMode::Lines:
              next[axis] += toward_end * std::abs(view.wheel_step[axis]) * float(request.lines);
              break;
            case LN_UIViewScrollMode::ShowItem:
              /* Below, once the content is placed. */
              break;
          }
        }
        if (request.mode == LN_UIViewScrollMode::ShowItem) {
          KX_GameObject *object = m_manager.ResolveObjectHandle(request.object);
          if (object != nullptr && object != view.content &&
              IsDescendantOf(object, view.content))
          {
            show_object = object;
            commanded = true;
          }
        }
      }
      const auto focus = m_focusByUser.find(view.user);
      if (scroll_steps != 0 || commanded) {
        persistent.scrolled_from_focus = focus != m_focusByUser.end() ? focus->second :
                                                                        LN_ObjectHandle();
      }
      else if (focus == m_focusByUser.end() ||
               !SameObjectHandle(persistent.scrolled_from_focus, focus->second))
      {
        persistent.scrolled_from_focus = LN_ObjectHandle();
      }
      if (!view.horizontal) {
        next.x() = 0.0f;
      }
      if (!view.vertical) {
        next.y() = 0.0f;
      }
      for (int axis = 0; axis < 3; ++axis) {
        next[axis] = std::clamp(next[axis], minimum[axis], maximum[axis]);
      }
      /* A scrollbar moves the content along the view axis it runs along. The part in view is the
       * viewport against the viewport plus the scroll range. */
      int scrollbar_axis = 1;
      float scrollbar_fraction = 1.0f;
      bool scrollbar_needed = false;
      if (view.scrollbar != nullptr) {
        scrollbar_axis = MakeScrollbarLayout(view, 1.0f).axis == 0 ? 0 : 1;
        const float range = maximum[scrollbar_axis] - minimum[scrollbar_axis];
        scrollbar_needed = range > kScaleEpsilon &&
                           (scrollbar_axis == 0 ? view.horizontal : view.vertical);
        KX_GameObject *parent = view.content->GetParent();
        const float range_size = range * (parent != nullptr ?
                                              std::abs(parent->NodeGetWorldScaling()[scrollbar_axis]) :
                                              1.0f);
        const float viewport_size = std::abs(
            (view.bounds_max[scrollbar_axis] - view.bounds_min[scrollbar_axis]) *
            view.world_scale[scrollbar_axis]);
        if (viewport_size + range_size > kScaleEpsilon) {
          scrollbar_fraction = viewport_size / (viewport_size + range_size);
        }
      }
      float drag_progress = 0.0f;
      const bool scrollbar_dragged = scrollbar_needed &&
                                     ScrollbarDragProgress(
                                         view, persistent, scrollbar_fraction, drag_progress);
      if (scrollbar_dragged) {
        const float range = maximum[scrollbar_axis] - minimum[scrollbar_axis];
        next[scrollbar_axis] = view.wheel_step[scrollbar_axis] < 0.0f ?
                                   maximum[scrollbar_axis] - drag_progress * range :
                                   minimum[scrollbar_axis] + drag_progress * range;
      }
      view.content->NodeSetLocalPosition(persistent.base_local_position + next);
      view.content->NodeUpdateGS(0.0);
      /* Moves `next` the least so `object`'s origin is inside the viewport (and its padding). */
      const auto bring_into_view = [&](KX_GameObject *object) {
        if (std::abs(view.world_scale.x()) <= kScaleEpsilon ||
            std::abs(view.world_scale.y()) <= kScaleEpsilon ||
            std::abs(view.world_scale.z()) <= kScaleEpsilon)
        {
          return;
        }
        const MT_Vector3 local = PointInViewSpace(view, object->NodeGetWorldPosition());
        const float padding = std::max(0.0f, view.padding);
        const MT_Vector3 low(view.bounds_min.x() - padding,
                             view.bounds_min.y() - padding,
                             view.bounds_min.z() - padding);
        const MT_Vector3 high(view.bounds_max.x() + padding,
                              view.bounds_max.y() + padding,
                              view.bounds_max.z() + padding);
        MT_Vector3 correction(0.0f, 0.0f, 0.0f);
        for (int axis = 0; axis < 3; ++axis) {
          if (local[axis] < low[axis]) {
            correction[axis] = low[axis] - local[axis];
          }
          else if (local[axis] > high[axis]) {
            correction[axis] = high[axis] - local[axis];
          }
        }
        if (!view.horizontal) {
          correction.x() = 0.0f;
        }
        if (!view.vertical) {
          correction.y() = 0.0f;
        }
        const MT_Vector3 scaled(correction.x() * view.world_scale.x(),
                                correction.y() * view.world_scale.y(),
                                correction.z() * view.world_scale.z());
        next += WorldDeltaToObjectLocal(view.content, view.world_orientation * scaled);
        for (int axis = 0; axis < 3; ++axis) {
          next[axis] = std::clamp(next[axis], minimum[axis], maximum[axis]);
        }
      };
      if (show_object != nullptr && !scrollbar_dragged) {
        bring_into_view(show_object);
      }
      /* Not while the player scrolls away from the focus with Scroll Up or Scroll Down. */
      else if (view.focus_follow && !scrollbar_dragged &&
               !persistent.scrolled_from_focus.IsValid())
      {
        KX_GameObject *focused = focus != m_focusByUser.end() ?
                                     m_manager.ResolveObjectHandle(focus->second) :
                                     nullptr;
        /* Not for a dragged or carried focus: it floats away from its place on purpose. */
        if (focused != nullptr && focused != view.content &&
            IsDescendantOf(focused, view.content) && !IsBeingDragged(focused, focused))
        {
          bring_into_view(focused);
        }
      }
      result.scrolled = (next - persistent.offset).length2() > kScaleEpsilon * kScaleEpsilon;
      persistent.offset = next;
      result.offset = next;
      for (int axis = 0; axis < 3; ++axis) {
        const float range = maximum[axis] - minimum[axis];
        if (range <= kScaleEpsilon) {
          result.normalized[axis] = 0.0f;
          continue;
        }
        /* Normalized zero always means the wheel-defined start, even for the common case where
         * content moves toward a negative minimum. This keeps At Start/At End consistent with
         * Grid View and with creator expectations. */
        result.normalized[axis] = view.wheel_step[axis] < 0.0f ?
                                      (maximum[axis] - next[axis]) / range :
                                      (next[axis] - minimum[axis]) / range;
      }
      result.at_start = (!view.horizontal || result.normalized.x() <= kScaleEpsilon) &&
                        (!view.vertical || result.normalized.y() <= kScaleEpsilon);
      result.at_end = (!view.horizontal || result.normalized.x() >= 1.0f - kScaleEpsilon) &&
                      (!view.vertical || result.normalized.y() >= 1.0f - kScaleEpsilon);
      view.content->NodeSetLocalPosition(persistent.base_local_position + next);
      view.content->NodeUpdateGS(0.0);

      std::unordered_set<KX_GameObject *> managed_this_tick;
      if (view.cull_outside) {
        const std::vector<KX_GameObject *> descendants = view.content->GetChildrenRecursive();
        for (KX_GameObject *object : descendants) {
          if (object == nullptr || object == view.viewport ||
              m_manager.IsGameObjectQueuedForRemoval(object))
          {
            continue;
          }
          managed_this_tick.insert(object);
          remember_visibility(view.listener, persistent, object);
          /* A dragged item stays visible outside the view until it is dropped. */
          const bool visible = PointInsideView(view, object->NodeGetWorldPosition()) ||
                               IsBeingDragged(object, view.content);
          request_visibility(object, visible);
        }
      }
      show_scrollbar(view,
                     persistent,
                     managed_this_tick,
                     scrollbar_needed,
                     scrollbar_fraction,
                     scrollbar_dragged ? drag_progress : result.normalized[scrollbar_axis]);
      reconcile_visibility(view.listener, persistent, managed_this_tick);
    }
    else {
      ViewPersistentState &persistent = m_viewStates[view.listener];
      result.valid = true;
      if (!persistent.initialized) {
        persistent.initialized = true;
        persistent.first_index = std::max(0, view.requested_first_index);
        persistent.last_requested_first_index = view.requested_first_index;
      }
      else if (view.requested_first_index != persistent.last_requested_first_index) {
        persistent.first_index = std::max(0, view.requested_first_index);
        persistent.last_requested_first_index = view.requested_first_index;
      }
      const int32_t slot_count = int32_t(
          std::min(view.slots.size(), size_t(std::numeric_limits<int32_t>::max())));
      const int32_t list_size = int32_t(
          std::min(view.items.size(), size_t(std::numeric_limits<int32_t>::max())));
      /* Filter: the grid shows only these places of the list, in order. Scrolling, pages and
       * navigation count what it shows; a slot stays bound to its item's place in the list. */
      const std::string filter_categories = NormalizedCategories(view.show_categories);
      const std::string filter_search = LowerCase(Trimmed(view.search));
      const bool filtered = !filter_categories.empty() || !filter_search.empty();
      std::vector<int32_t> shown_places;
      if (filtered) {
        for (int32_t place = 0; place < list_size; ++place) {
          if (FilterShows(view, view.items[size_t(place)], filter_categories, filter_search)) {
            shown_places.push_back(place);
          }
        }
      }
      const auto place_of = [&](const int64_t position) {
        return filtered ? shown_places[size_t(position)] : int32_t(position);
      };
      const int32_t item_count = filtered ? int32_t(shown_places.size()) : list_size;
      const bool has_visible_items = slot_count > 0 && item_count > 0;
      /* Scroll by whole lines (rows, or columns of a horizontal grid) so items never move within
       * a line; the last line may be partly empty. A line is never longer than the slot pool.
       * Without template Rows, a horizontal grid's column is the slots divided by Columns. */
      const int64_t slot_pool = std::max(1, slot_count);
      const int64_t columns = std::clamp<int64_t>(view.columns, 1, slot_pool);
      const int64_t line = !view.horizontal ? columns :
                           view.rows > 0    ? std::clamp<int64_t>(view.rows, 1, slot_pool) :
                                              (slot_pool + columns - 1) / columns;
      /* The first line that still shows the last item; rounded up below to a whole line. */
      const int64_t last_first = std::max<int64_t>(0, int64_t(item_count) - slot_count);
      const int32_t maximum_first =
          has_visible_items ? int32_t(std::min<int64_t>((last_first + line - 1) / line * line,
                                                        std::numeric_limits<int32_t>::max())) :
                              0;
      const auto line_start = [&](const int64_t first) {
        const int64_t clamped = std::clamp(first, int64_t(0), int64_t(maximum_first));
        return int32_t(clamped - clamped % line);
      };
      const int32_t previous_first = persistent.first_index;
      /* A page is the whole lines the slots show (a partly filled last line of slots does not
       * count). Pages turn by that much from wherever the view is, so none is skipped; Page names
       * the page boundary nearest to the view (the last at the end). */
      const int64_t page_lines = std::max<int64_t>(1, int64_t(slot_count) / line);
      const int64_t last_line = int64_t(maximum_first) / line;
      const int64_t page_count = 1 + (last_line + page_lines - 1) / page_lines;
      const auto page_of = [&](const int32_t first) -> int64_t {
        if (maximum_first > 0 && first >= maximum_first) {
          return page_count;
        }
        return std::clamp<int64_t>(
            (int64_t(first) / line * 2 + page_lines) / (2 * page_lines) + 1, 1, page_count);
      };
      for (const ViewScrollRequest &request : take_scroll_requests(view)) {
        if (slot_count <= 0) {
          continue;
        }
        switch (request.mode) {
          case LN_UIViewScrollMode::ShowItem: {
            /* The least scroll that shows it: its line at the top or at the bottom. By index, or
             * an item like it (by name; an object item, also by the object's name): none if one
             * is in view, else the nearest. */
            if (item_count <= 0) {
              break;
            }
            /* In view: a slot shows it (also one in a partly filled last row of slots). */
            const int64_t first = persistent.first_index;
            const auto lines_away = [&](const int64_t index) {
              if (index < first) {
                return (first - index + line - 1) / line;
              }
              return index < first + slot_count ? int64_t(0) :
                                                  (index - (first + slot_count)) / line + 1;
            };
            int64_t shown = -1;
            if (request.has_index && !filtered) {
              shown = std::clamp<int64_t>(request.index, 0, item_count - 1);
            }
            else if (request.has_index) {
              /* An index in the list: where the grid shows that place, if it does. */
              const auto at = std::find(shown_places.begin(), shown_places.end(), request.index);
              shown = at != shown_places.end() ? int64_t(at - shown_places.begin()) : -1;
            }
            else {
              KX_GameObject *object = m_manager.ResolveObjectHandle(request.object);
              for (int32_t candidate = 0; candidate < item_count; ++candidate) {
                const LN_Value &item = view.items[size_t(place_of(candidate))];
                /* Its shown name (a recipe line's result) or its own (the line itself). */
                std::string name;
                std::string entry;
                int32_t count = 0;
                if (item.exists && item.type == LN_ValueType::ObjectRef) {
                  name = item.reference_name;
                }
                else if (!LN_InventoryShownEntry(item, name, count)) {
                  continue;
                }
                LN_InventoryEntry(item, entry, count);
                if (((!request.item.empty() && (name == request.item || entry == request.item)) ||
                     (object != nullptr && name == object->GetName())) &&
                    (shown < 0 || lines_away(candidate) < lines_away(shown)))
                {
                  shown = candidate;
                }
              }
            }
            if (shown < 0) {
              break;
            }
            /* Its line at the top, or the least scroll that brings it into the last slots. */
            if (shown < first) {
              persistent.first_index = line_start(shown);
            }
            else if (shown >= first + slot_count) {
              persistent.first_index = line_start((shown - slot_count + line) / line * line);
            }
            break;
          }
          case LN_UIViewScrollMode::Top:
            persistent.first_index = 0;
            break;
          case LN_UIViewScrollMode::Bottom:
            persistent.first_index = maximum_first;
            break;
          case LN_UIViewScrollMode::NextPage:
          case LN_UIViewScrollMode::PreviousPage:
            persistent.first_index = line_start(
                int64_t(persistent.first_index) +
                (request.mode == LN_UIViewScrollMode::NextPage ? 1 : -1) * page_lines * line);
            break;
          case LN_UIViewScrollMode::Lines:
            persistent.first_index = line_start(int64_t(persistent.first_index) +
                                                int64_t(request.lines) * line);
            break;
        }
      }
      int64_t scroll_steps = wheel_view == &view ? wheel_delta : 0;
      if (const auto directed = directed_scroll.find(&view); directed != directed_scroll.end()) {
        scroll_steps += directed->second;
      }
      if (scroll_steps != 0 && slot_count > 0) {
        const int64_t amount = std::min<int64_t>(std::max<int64_t>(line, maximum_first),
                                                 line * int64_t(std::max(1, view.wheel_rows)));
        persistent.first_index = line_start(int64_t(persistent.first_index) -
                                            scroll_steps * amount);
      }
      persistent.first_index = line_start(persistent.first_index);
      /* A scrollbar drag picks the first line. The part in view is the lines the slots show. */
      const int64_t total_lines = (int64_t(item_count) + line - 1) / line;
      const int64_t visible_lines = (int64_t(slot_count) + line - 1) / line;
      const float scrollbar_fraction = total_lines > 0 ? std::min(1.0f,
                                                                  float(visible_lines) /
                                                                      float(total_lines)) :
                                                         1.0f;
      float drag_progress = 0.0f;
      const bool scrollbar_dragged = maximum_first > 0 &&
                                     ScrollbarDragProgress(
                                         view, persistent, scrollbar_fraction, drag_progress);
      if (scrollbar_dragged) {
        const int64_t last_line = maximum_first / line;
        persistent.first_index = line_start(
            std::llround(double(drag_progress) * double(last_line)) * line);
      }

      /* A virtual grid exposes physical slot objects, but keyboard/gamepad navigation must still
       * reach every logical item. When navigation moves beyond the authored slot pool, recycle one
       * row/item and retain focus on the same physical slot. ResolveInteractions then skips the
       * same action once, preventing a second wrap among the newly rebound slots. */
      if (view.focus_follow && slot_count > 0 && item_count > slot_count) {
        const auto focus = m_focusByUser.find(view.user);
        KX_GameObject *focused = focus != m_focusByUser.end() ?
                                     m_manager.ResolveObjectHandle(focus->second) :
                                     nullptr;
        const auto focused_owner = grid_slot_owner.find(focused);
        const auto slot = std::find(view.slots.begin(), view.slots.begin() + slot_count, focused);
        /* A hidden focus is only shown by the first press (ResolveNavigationForUser). */
        if (focused_owner != grid_slot_owner.end() && focused_owner->second == &view &&
            FocusShown(view.user) && DirectedInputAllowed(view.user) &&
            ObjectAcceptsDirectedInput(focused, view.user) &&
            slot != view.slots.begin() + slot_count)
        {
          const int32_t slot_index = int32_t(slot - view.slots.begin());
          const int32_t item_index = persistent.first_index + slot_index;
          LN_UIAction action = LN_UIAction::Next;
          bool from_press = false;
          const bool has_action = NavigationForUser(view.user, action, from_press);
          /* Page one line and move focus to the slot that now shows the next item. Down and Up
           * page a vertical grid at its last and first row, Right and Left a horizontal grid at
           * its last and first column. */
          const LN_UIAction forward = view.horizontal ? LN_UIAction::NavigateRight :
                                                        LN_UIAction::NavigateDown;
          const LN_UIAction backward = view.horizontal ? LN_UIAction::NavigateLeft :
                                                         LN_UIAction::NavigateUp;
          int64_t requested = persistent.first_index;
          int64_t target_item = item_index;
          if (has_action && action == LN_UIAction::Next && slot_index == slot_count - 1 &&
              item_index + 1 < item_count)
          {
            requested += line;
            target_item = int64_t(item_index) + 1;
          }
          else if (has_action && action == LN_UIAction::Previous && slot_index == 0 &&
                   item_index > 0)
          {
            requested -= line;
            target_item = int64_t(item_index) - 1;
          }
          else if (has_action && action == forward && int64_t(slot_index) + line >= slot_count &&
                   int64_t(item_index) + line < item_count)
          {
            requested += line;
            target_item = int64_t(item_index) + line;
          }
          else if (has_action && action == backward && int64_t(slot_index) - line < 0 &&
                   int64_t(item_index) - line >= 0)
          {
            requested -= line;
            target_item = int64_t(item_index) - line;
          }
          const int32_t next_first = line_start(requested);
          if (next_first != persistent.first_index) {
            persistent.first_index = next_first;
            m_viewNavigationHandledUsers.insert(view.user);
            /* The keys turn the page with the selection: what the mouse held is theirs now. */
            HandPickUpToKeys(view.user);
            if (from_press) {
              ConsumeActionPress(action, view.user);
            }
            const int64_t wanted = target_item - next_first;
            int64_t target_slot = wanted;
            /* The cell of a dragged or carried slot is empty: take the nearest filled one. */
            const auto dragged = [&](const int64_t slot) {
              return IsBeingDragged(view.slots[size_t(slot)], view.slots[size_t(slot)]);
            };
            if (target_slot >= 0 && target_slot < slot_count && dragged(target_slot)) {
              target_slot = -1;
              for (int64_t step = 1; step < slot_count && target_slot < 0; ++step) {
                for (const int64_t other : {wanted + step, wanted - step}) {
                  if (other >= 0 && other < slot_count && next_first + other < item_count &&
                      !dragged(other))
                  {
                    target_slot = other;
                    break;
                  }
                }
              }
            }
            if (target_slot >= 0 && target_slot < slot_count &&
                view.slots[size_t(target_slot)] != nullptr)
            {
              KX_GameObject *target = view.slots[size_t(target_slot)];
              m_focusByUser[view.user] = m_manager.MakeObjectHandle(target, target->GetName());
            }
          }
        }
      }

      size_t fingerprint = size_t(persistent.first_index);
      HashCombine(fingerprint, view.items.size());
      /* Where a drag's item is in the list now: its index when the drag started, unless the list
       * changed meanwhile (-1: gone). */
      const auto dragged_index_of = [&](const PointerState &drag) {
        const size_t dragged = ValueFingerprint(drag.drag_item);
        const int32_t index = drag.drag_item_index;
        if (index >= 0 && index < list_size &&
            ValueFingerprint(view.items[size_t(index)]) == dragged)
        {
          return index;
        }
        for (int32_t other = 0; other < list_size; ++other) {
          if (ValueFingerprint(view.items[size_t(other)]) == dragged) {
            return other;
          }
        }
        return int32_t(-1);
      };
      /* Item Only drags from this grid: the dragged item's cell shows what stays in it (the rest
       * of a split stack, else nothing), whichever slot shows it after a scroll. */
      std::unordered_map<int32_t, LN_Value> left_in_cells;
      for (KX_GameObject *object : view.slots) {
        const PointerState *drag = object != nullptr && DragPictureOf(object) != nullptr ?
                                       FrozenDragOf(object) :
                                       nullptr;
        const int32_t index = drag != nullptr ? dragged_index_of(*drag) : -1;
        if (index >= 0) {
          left_in_cells[index] = ItemLeftInCell(*drag);
        }
      }
      /* Filtered, an empty cell stands for the list's first empty place (Keep Places), else the
       * place past its end. */
      int32_t first_free_place = list_size;
      for (int32_t at = 0; filtered && at < list_size; ++at) {
        if (LN_InventoryIsEmptyPlace(view.items[size_t(at)])) {
          first_free_place = at;
          break;
        }
      }
      /* On UI Event Item Shown: what a slot shows now, when it is not what it last announced. */
      const auto note_shown = [&](const int32_t slot,
                                  KX_GameObject *object,
                                  const LN_Value &shown,
                                  const int32_t index) {
        const LN_ObjectHandle handle = m_manager.MakeObjectHandle(object, object->GetName());
        size_t shown_fingerprint = ValueFingerprint(shown);
        HashCombine(shown_fingerprint, size_t(handle.index));
        HashCombine(shown_fingerprint, size_t(handle.generation));
        HashCombine(shown_fingerprint, size_t(index) + 1);
        shown_fingerprint |= 1;
        if (persistent.shown_announced.size() <= size_t(slot)) {
          persistent.shown_announced.resize(size_t(slot) + 1, 0);
        }
        if (persistent.shown_announced[size_t(slot)] != shown_fingerprint) {
          m_cellsShown.push_back(
              {view.listener, size_t(slot), handle, shown, index, view.user, shown_fingerprint});
        }
      };
      std::unordered_set<KX_GameObject *> managed_this_tick;
      for (int32_t slot = 0; slot < slot_count; ++slot) {
        KX_GameObject *object = view.slots[size_t(slot)];
        const auto owner = grid_slot_owner.find(object);
        if (object == nullptr || object->GetScene() != &m_scene || object == view.viewport ||
            m_manager.IsGameObjectQueuedForRemoval(object) ||
            owner == grid_slot_owner.end() || owner->second != &view)
        {
          continue;
        }
        managed_this_tick.insert(object);
        remember_visibility(view.listener, persistent, object);
        const int32_t item_index = persistent.first_index + slot;
        /* A dragged slot keeps the item it had when the drag started, even if the grid scrolls
         * meanwhile. Following the pointer, it leaves its cell empty until it is put down; with
         * Item Only it stays in its cell and a copy of the item follows instead. */
        const PointerState *frozen_drag = FrozenDragOf(object);
        /* With Show Empty, the cells past the last item stay as empty slots. */
        const bool filled = item_index >= 0 && item_index < item_count;
        /* The slot's place in the list: an empty cell is the place past the end (Keep Places
         * pads up to it); filtered, the list's first free place, where an item from another
         * grid goes. */
        const int32_t place = filled ? place_of(item_index) : filtered ? first_free_place :
                                                                         item_index;
        const bool active_slot = frozen_drag != nullptr || filled ||
                                 (view.show_empty && item_index >= 0);
        /* A dragged slot stays visible outside the view until it is dropped. */
        const bool inside = PointInsideView(view, object->NodeGetWorldPosition()) ||
                            IsBeingDragged(object, object);
        const bool visible = active_slot && (!view.cull_outside || inside);
        request_visibility(object, visible);
        /* A slot's icon and text are its children: they show and hide with the slot. Its own
         * Highlight, Refused and Tooltip Objects and widget parts are neither (a highlight frame
         * is no icon, a tooltip's text no count). */
        KX_GameObject *icon = nullptr;
        KX_GameObject *text = nullptr;
        const ElementRecord *slot_record = FindElementRecord(object);
        for (KX_GameObject *child : object->GetChildrenRecursive()) {
          if (child != nullptr && !m_manager.IsGameObjectQueuedForRemoval(child)) {
            managed_this_tick.insert(child);
            remember_visibility(view.listener, persistent, child);
            request_visibility(child, visible);
          }
        }
        SlotParts(object, icon, text, nullptr);
        if (frozen_drag != nullptr) {
          /* If the list changed meanwhile, the index is where the item is now (-1: gone). */
          const size_t dragged = ValueFingerprint(frozen_drag->drag_item);
          const int32_t index = dragged_index_of(*frozen_drag);
          SlotBinding binding;
          binding.view = view.listener;
          binding.item = frozen_drag->drag_item;
          binding.index = index;
          m_slotBindings[object] = std::move(binding);
          /* The same as before the drag while nothing changed. */
          HashCombine(fingerprint, dragged);
          HashCombine(fingerprint, static_cast<const void *>(object));
          HashCombine(fingerprint, size_t(frozen_drag->drag_amount));
          /* A split stack: the part it carries goes with the slot, the rest stays in the cell (a
           * copy of the slot there). A slot that does not follow the pointer stays in its cell:
           * it shows the rest. */
          const bool follows = slot_record != nullptr && slot_record->settings.draggable &&
                               slot_record->settings.follow_cursor;
          const LN_Value no_item;
          const LN_Value &cell_item = filled ? view.items[size_t(place)] : no_item;
          LN_Value shown = frozen_drag->drag_item;
          std::string name;
          int32_t count = 0;
          if (DragPictureOf(object) != nullptr) {
            /* Item Only: the slot stays in its cell (a copy of the item follows the pointer) and
             * shows what is there: what is left of the dragged item, or the cell's own item when
             * the grid scrolled meanwhile. */
            const auto left = filled ? left_in_cells.find(place) : left_in_cells.end();
            shown = left != left_in_cells.end() ? left->second : cell_item;
          }
          else if (frozen_drag->drag_amount > 0 && follows) {
            StartDragGhost(object);
            UpdateDragGhost(view,
                            size_t(slot),
                            object,
                            cell_item,
                            filled && index == place,
                            filled || view.show_empty);
            shown = LN_InventoryEntryWithCount(frozen_drag->drag_item, frozen_drag->drag_amount);
          }
          else if (frozen_drag->drag_amount > 0 &&
                   LN_InventoryEntry(frozen_drag->drag_item, name, count))
          {
            shown = LN_InventoryEntryWithCount(frozen_drag->drag_item,
                                               count - frozen_drag->drag_amount);
          }
          if (KX_GameObject *hidden = ShowSlotItem(
                  view, persistent, size_t(slot), object, shown, icon, text))
          {
            request_visibility(hidden, false);
          }
          note_shown(slot, object, shown, index);
        }
        else if (active_slot) {
          const LN_Value no_item;
          const LN_Value &item = filled ? view.items[size_t(place)] : no_item;
          SlotBinding binding;
          binding.view = view.listener;
          binding.item = item;
          binding.index = place;
          m_slotBindings[object] = std::move(binding);
          HashCombine(fingerprint, ValueFingerprint(item));
          /* New template copies are new slot objects to rebind. */
          HashCombine(fingerprint, static_cast<const void *>(object));
          /* An Item Only drag's item after a scroll: this cell shows what is left of it. */
          const auto left = filled ? left_in_cells.find(place) : left_in_cells.end();
          const LN_Value &shown = left != left_in_cells.end() ? left->second : item;
          if (KX_GameObject *hidden = ShowSlotItem(
                  view, persistent, size_t(slot), object, shown, icon, text))
          {
            request_visibility(hidden, false);
          }
          note_shown(slot, object, shown, place);
        }
      }
      /* While dragged the handle follows the pointer smoothly; the rows snap. */
      show_scrollbar(view,
                     persistent,
                     managed_this_tick,
                     maximum_first > 0,
                     scrollbar_fraction,
                     scrollbar_dragged ? drag_progress :
                                         (maximum_first > 0 ? float(persistent.first_index) /
                                                                  float(maximum_first) :
                                                              0.0f));
      reconcile_visibility(view.listener, persistent, managed_this_tick);

      /* Virtualized slots are the focusable objects; scrolling rebinds their data but must not
       * partially undo the requested wheel step in an attempt to preserve an old item index. If
       * data mutation makes the focused physical slot inactive, move focus to a remaining visible
       * slot owned by this view, or clear it when the view has no focusable item left. */
      if (view.focus_follow) {
        const auto focus = m_focusByUser.find(view.user);
        KX_GameObject *focused = focus != m_focusByUser.end() ?
                                     m_manager.ResolveObjectHandle(focus->second) :
                                     nullptr;
        const auto owner = grid_slot_owner.find(focused);
        if (focused != nullptr && owner != grid_slot_owner.end() && owner->second == &view) {
          const auto focused_binding = m_slotBindings.find(focused);
          /* A dragged or carried slot is judged by its cell, not by where it floats. */
          const bool focus_remains_visible =
              focused_binding != m_slotBindings.end() &&
              focused_binding->second.view == view.listener &&
              focused->GetVisibleIgnoringViewClip() &&
              (!view.cull_outside || IsBeingDragged(focused, focused) ||
               PointInsideView(view, focused->NodeGetWorldPosition()));
          if (!focus_remains_visible) {
            KX_GameObject *replacement = nullptr;
            for (KX_GameObject *slot : view.slots) {
              const auto binding = m_slotBindings.find(slot);
              if (slot != nullptr && binding != m_slotBindings.end() &&
                  binding->second.view == view.listener && slot->GetVisibleIgnoringViewClip() &&
                  (!view.cull_outside || PointInsideView(view, slot->NodeGetWorldPosition())))
              {
                replacement = slot;
                break;
              }
            }
            if (replacement != nullptr) {
              m_focusByUser[view.user] = m_manager.MakeObjectHandle(replacement,
                                                                    replacement->GetName());
            }
            else {
              m_focusByUser.erase(view.user);
            }
          }
        }
      }
      result.items_rebound = fingerprint != persistent.binding_fingerprint;
      persistent.binding_fingerprint = fingerprint;
      result.scrolled = persistent.first_index != previous_first;
      result.first_visible_index = has_visible_items ? persistent.first_index : -1;
      result.last_visible_index = has_visible_items ?
                                      std::min(item_count - 1,
                                               persistent.first_index + slot_count - 1) :
                                      -1;
      result.at_start = persistent.first_index == 0;
      result.at_end = persistent.first_index == maximum_first;
      result.page = int32_t(page_of(persistent.first_index));
      result.page_count = int32_t(std::min<int64_t>(page_count, std::numeric_limits<int32_t>::max()));
      result.offset = MT_Vector3(float(persistent.first_index), 0.0f, 0.0f);
      result.normalized.x() = maximum_first > 0 ?
                                  float(persistent.first_index) / float(maximum_first) :
                                  0.0f;
    }
  }
  /* Views clip through their own visibility layer, so Set Visibility by game logic still works on
   * the objects they manage. */
  for (const auto &item : desired_visibility) {
    item.first->SetViewClipVisible(item.second);
  }
  std::erase_if(m_viewScrollRequests,
                [](const ViewScrollRequest &request) { return request.used; });
  RemoveQueuedTemplateCopies();
}

void LN_UIService::RequestViewScroll(KX_GameObject *viewport,
                                     const LN_UIViewScrollMode mode,
                                     const int32_t lines,
                                     const ScrollTarget &target)
{
  if (viewport == nullptr) {
    return;
  }
  ViewScrollRequest &request = m_viewScrollRequests.emplace_back();
  request.viewport = m_manager.MakeObjectHandle(viewport, viewport->GetName());
  request.mode = mode;
  request.lines = lines;
  request.has_index = target.has_index;
  request.index = target.index;
  request.item = target.item;
  if (target.object != nullptr) {
    request.object = m_manager.MakeObjectHandle(target.object, target.object->GetName());
  }
  request.frame = m_frameIndex;
}

const LN_UIService::ViewState &LN_UIService::GetViewState(const ListenerKey &listener) const
{
  const auto item = m_currentViewStates.find(listener);
  if (item == m_currentViewStates.end()) {
    return s_emptyViewState;
  }
  if (!m_passive) {
    return item->second;
  }
  m_passiveViewState = item->second;
  m_passiveViewState.scrolled = false;
  m_passiveViewState.items_rebound = false;
  return m_passiveViewState;
}

bool LN_UIService::GetBoundItem(KX_GameObject *object, LN_Value &r_item, int32_t &r_index) const
{
  const auto item = m_slotBindings.find(object);
  if (item == m_slotBindings.end()) {
    r_item = LN_Value();
    r_index = -1;
    return false;
  }
  r_item = item->second.item;
  r_index = item->second.index;
  return true;
}

bool LN_UIService::IsGridSlot(const KX_GameObject *object) const
{
  /* Asked for every element each frame: index the grids' slots again only when views change. */
  if (m_gridSlotsVersion != m_viewSlotsVersion) {
    m_gridSlots.clear();
    for (const ViewRegistration &view : m_viewRegistrations) {
      if (view.kind == ViewKind::Grid) {
        m_gridSlots.insert(view.slots.begin(), view.slots.end());
      }
    }
    m_gridSlotsVersion = m_viewSlotsVersion;
  }
  return m_gridSlots.contains(object);
}

bool LN_UIService::ObjectPassesViewClipping(KX_GameObject *object) const
{
  if (object == nullptr) {
    return false;
  }
  /* A dragged item keeps its press when it leaves the view it came from. */
  for (const ViewRegistration &view : m_viewRegistrations) {
    if (!view.enabled) {
      continue;
    }
    if (view.kind == ViewKind::Scroll && IsDescendantOf(object, view.content) &&
        !PointInsideView(view, object->NodeGetWorldPosition()) &&
        !IsBeingDragged(object, view.content))
    {
      return false;
    }
    if (view.kind == ViewKind::Grid && IsGridSlot(object)) {
      const auto binding = m_slotBindings.find(object);
      if (binding == m_slotBindings.end()) {
        /* A slot of this view that shows no item. */
        if (std::find(view.slots.begin(), view.slots.end(), object) != view.slots.end()) {
          return false;
        }
        continue;
      }
      /* A slot may be authored in multiple views. Only the deterministic winning view owns its
       * binding and clipping; a lower-priority declaration must not hide the winner's slot. */
      if (binding->second.view == view.listener &&
          !PointInsideView(view, object->NodeGetWorldPosition()) &&
          !IsBeingDragged(object, object))
      {
        return false;
      }
    }
  }
  return true;
}

void LN_UIService::DeclareInteractable(const ListenerKey listener,
                                       const int32_t user,
                                       const bool enabled)
{
  if (m_resolving || !IsValidListener(listener)) {
    return;
  }
  const ListenerDeclaration declaration{std::max(user, 0), enabled};
  const auto [item, inserted] = m_listenerDeclarations.try_emplace(listener, declaration);
  if (!inserted) {
    /* One compiled query declares exactly once per phase. Keep the first declaration if malformed
     * runtime code attempts to reuse the same key with incompatible semantics. */
    return;
  }
  m_currentStates.try_emplace(listener, InteractionState());
}

void LN_UIService::RegisterInteractable(Registration registration)
{
  if (m_resolving || !IsValidListener(registration.listener) || registration.object == nullptr ||
      registration.camera == nullptr || registration.object->GetScene() != &m_scene ||
      registration.camera->GetScene() != &m_scene ||
      m_manager.IsGameObjectQueuedForRemoval(registration.object) ||
      m_manager.IsGameObjectQueuedForRemoval(registration.camera) ||
      !IsFinite(registration.ray_from) ||
      !IsFinite(registration.ray_to) || !IsFinite(registration.bounds_min) ||
      !IsFinite(registration.bounds_max) || !IsFinite(registration.world_position) ||
      !IsFinite(registration.world_orientation) || !IsFinite(registration.world_scale) ||
      !IsFinite(registration.screen_position) || !std::isfinite(registration.padding) ||
      !std::isfinite(registration.drag_threshold) ||
      (registration.has_precomputed_hit && (!std::isfinite(registration.precomputed_fraction) ||
                                            !IsFinite(registration.precomputed_point) ||
                                            !IsFinite(registration.precomputed_normal))) ||
      registration.bounds_min.x() > registration.bounds_max.x() ||
      registration.bounds_min.y() > registration.bounds_max.y() ||
      registration.bounds_min.z() > registration.bounds_max.z())
  {
    return;
  }
  if (!m_listenerDeclarations.contains(registration.listener)) {
    DeclareInteractable(registration.listener, registration.user, registration.enabled);
  }
  const auto declaration = m_listenerDeclarations.find(registration.listener);
  if (declaration == m_listenerDeclarations.end() ||
      declaration->second.user != registration.user)
  {
    return;
  }
  /* A listener has exactly one registration. */
  if (!m_registeredListeners.insert(registration.listener).second) {
    return;
  }
  if (!m_registeredTargets.insert({registration.listener, registration.object}).second) {
    return;
  }
  const ListenerKey listener = registration.listener;
  m_registrations.push_back(std::move(registration));
  m_currentStates.try_emplace(listener, InteractionState());
}

bool LN_UIService::RayIntersectsRegistration(const Registration &registration,
                                             float &r_fraction,
                                             MT_Vector3 &r_point,
                                             MT_Vector3 &r_normal)
{
  if (registration.has_precomputed_hit) {
    if (!registration.precomputed_hit || !std::isfinite(registration.precomputed_fraction) ||
        !IsFinite(registration.precomputed_point) || !IsFinite(registration.precomputed_normal))
    {
      return false;
    }
    r_fraction = std::clamp(registration.precomputed_fraction, 0.0f, 1.0f);
    r_point = registration.precomputed_point;
    r_normal = registration.precomputed_normal;
    return true;
  }
  const MT_Vector3 scale = registration.world_scale;
  if (std::abs(scale.x()) <= kScaleEpsilon || std::abs(scale.y()) <= kScaleEpsilon ||
      std::abs(scale.z()) <= kScaleEpsilon)
  {
    return false;
  }

  const MT_Matrix3x3 inverse_orientation = registration.world_orientation.transposed();
  auto world_to_local = [&](const MT_Vector3 &point) {
    const MT_Vector3 unrotated = inverse_orientation * (point - registration.world_position);
    return MT_Vector3(
        unrotated.x() / scale.x(), unrotated.y() / scale.y(), unrotated.z() / scale.z());
  };
  const MT_Vector3 local_from = world_to_local(registration.ray_from);
  const MT_Vector3 local_to = world_to_local(registration.ray_to);
  const MT_Vector3 direction = local_to - local_from;
  const MT_Vector3 minimum = registration.bounds_min - MT_Vector3(registration.padding,
                                                                  registration.padding,
                                                                  registration.padding);
  const MT_Vector3 maximum = registration.bounds_max + MT_Vector3(registration.padding,
                                                                  registration.padding,
                                                                  registration.padding);

  float enter = 0.0f;
  float leave = 1.0f;
  int enter_axis = -1;
  float enter_sign = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    const float origin = local_from[axis];
    const float delta = direction[axis];
    if (std::abs(delta) <= kScaleEpsilon) {
      if (origin < minimum[axis] || origin > maximum[axis]) {
        return false;
      }
      continue;
    }
    float near_t = (minimum[axis] - origin) / delta;
    float far_t = (maximum[axis] - origin) / delta;
    float near_sign = -1.0f;
    if (near_t > far_t) {
      std::swap(near_t, far_t);
      near_sign = 1.0f;
    }
    if (near_t > enter) {
      enter = near_t;
      enter_axis = axis;
      enter_sign = near_sign;
    }
    leave = std::min(leave, far_t);
    if (enter > leave) {
      return false;
    }
  }
  if (leave < 0.0f || enter > 1.0f) {
    return false;
  }

  r_fraction = std::clamp(enter, 0.0f, 1.0f);
  r_point = registration.ray_from + (registration.ray_to - registration.ray_from) * r_fraction;
  MT_Vector3 local_normal(0.0f, 0.0f, 0.0f);
  if (enter_axis >= 0) {
    local_normal[enter_axis] = enter_sign;
  }
  else {
    const MT_Vector3 local_point = local_from + direction * r_fraction;
    const float distances[6] = {std::abs(local_point.x() - minimum.x()),
                                std::abs(maximum.x() - local_point.x()),
                                std::abs(local_point.y() - minimum.y()),
                                std::abs(maximum.y() - local_point.y()),
                                std::abs(local_point.z() - minimum.z()),
                                std::abs(maximum.z() - local_point.z())};
    const int side = int(std::min_element(distances, distances + 6) - distances);
    local_normal[side / 2] = side % 2 == 0 ? -1.0f : 1.0f;
  }
  MT_Vector3 scaled_normal(
      local_normal.x() / scale.x(), local_normal.y() / scale.y(), local_normal.z() / scale.z());
  r_normal = registration.world_orientation * scaled_normal;
  if (r_normal.length2() > kScaleEpsilon) {
    r_normal.normalize();
  }
  return std::isfinite(r_fraction) && IsFinite(r_point) && IsFinite(r_normal);
}

void LN_UIService::SetPointerRayForTests(PointerRayFunction function)
{
  m_pointerRayForTests = std::move(function);
}

LN_UIService::PointerSurface LN_UIService::MakePointerSurface(
    const Registration &registration) const
{
  PointerSurface surface;
  if (registration.object != nullptr) {
    surface.object = m_manager.MakeObjectHandle(registration.object,
                                                registration.object->GetName());
  }
  if (registration.camera != nullptr) {
    surface.camera = m_manager.MakeObjectHandle(registration.camera,
                                                registration.camera->GetName());
  }
  surface.bounds_min = registration.bounds_min;
  surface.bounds_max = registration.bounds_max;
  surface.padding = registration.padding;
  surface.element = registration.listener.element != nullptr;
  surface.main_world = registration.interaction_domain == LN_InteractionDomain::Main;
  /* A node's Physics Surface hit test in the game world: the frame took the physics ray's hit
   * (UI Elements there are hit by their bounds, behind no wall). */
  surface.physics_hit = surface.main_world && registration.listener.element == nullptr &&
                        registration.has_precomputed_hit;
  return surface;
}

bool LN_UIService::PointerOverSurface(const std::vector<PointerSurface> &surfaces,
                                      const LN_MouseSnapshot &mouse)
{
  if (surfaces.empty() || !mouse.has_position) {
    return false;
  }
  /* One pointer ray per camera and, for the game world, one physics ray along it. */
  struct CameraRay {
    KX_Camera *camera = nullptr;
    MT_Vector3 from = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 to = MT_Vector3(0.0f, 0.0f, 0.0f);
    bool valid = false;
    bool world_cast = false;
    KX_GameObject *world_hit = nullptr;
    float world_fraction = 1.0f;
  };
  std::vector<CameraRay> rays;
  const auto ray_for = [&](KX_Camera *camera) -> CameraRay & {
    for (CameraRay &ray : rays) {
      if (ray.camera == camera) {
        return ray;
      }
    }
    CameraRay &ray = rays.emplace_back();
    ray.camera = camera;
    ray.valid = m_pointerRayForTests ?
                    m_pointerRayForTests(
                        camera, float(mouse.x), float(mouse.y), ray.from, ray.to) :
                    LN_ComputeCameraPointerRay(camera,
                                               float(mouse.x),
                                               float(mouse.y),
                                               std::max(camera->GetCameraFar(), 0.0f),
                                               ray.from,
                                               ray.to,
                                               HoldsPointerInView(camera));
    return ray;
  };
  const auto cast_world_ray = [&](CameraRay &ray) {
    if (ray.world_cast) {
      return;
    }
    ray.world_cast = true;
    PHY_IPhysicsEnvironment *environment = m_scene.GetPhysicsEnvironment();
    if (environment == nullptr) {
      return;
    }
    UIWorldRayCallback callback(m_scene);
    callback.m_ignored = ShownPointerCursors();
    callback.m_camera = ray.camera;
    KX_RayCast::RayTest(environment, ray.from, ray.to, callback);
    const float length = (ray.to - ray.from).length();
    if (callback.m_object != nullptr && length > MT_EPSILON) {
      ray.world_hit = callback.m_object;
      ray.world_fraction = (callback.m_hitPoint - ray.from).length() / length;
    }
  };

  for (const PointerSurface &surface : surfaces) {
    KX_GameObject *object = m_manager.ResolveObjectHandle(surface.object);
    KX_GameObject *camera = m_manager.ResolveObjectHandle(surface.camera);
    if (object == nullptr || camera == nullptr ||
        camera->GetGameObjectType() != SCA_IObject::OBJ_CAMERA ||
        m_manager.IsGameObjectQueuedForRemoval(object) ||
        (surface.must_be_visible && !object->GetVisible()))
    {
      continue;
    }
    CameraRay &ray = ray_for(static_cast<KX_Camera *>(camera));
    if (!ray.valid) {
      continue;
    }
    if (surface.physics_hit) {
      cast_world_ray(ray);
      if (ray.world_hit != nullptr &&
          (ray.world_hit == object || IsDescendantOf(ray.world_hit, object)))
      {
        return true;
      }
      continue;
    }
    /* Where the object is now: what the player sees, also when it moved since the last frame. */
    Registration shape;
    shape.ray_from = ray.from;
    shape.ray_to = ray.to;
    shape.bounds_min = surface.bounds_min;
    shape.bounds_max = surface.bounds_max;
    shape.world_position = object->NodeGetWorldPosition();
    shape.world_orientation = object->NodeGetWorldOrientation();
    shape.world_scale = object->NodeGetWorldScaling();
    shape.padding = surface.padding;
    if (surface.element) {
      if (const ElementRecord *record = FindElementRecord(object)) {
        ElementHitTransform(*record, shape.world_position, shape.world_scale);
      }
    }
    float fraction = 1.0f;
    MT_Vector3 point;
    MT_Vector3 normal;
    if (!RayIntersectsRegistration(shape, fraction, point, normal)) {
      continue;
    }
    /* Not through a wall (as CollectElements decides for UI Elements in the game world). */
    if (surface.main_world) {
      cast_world_ray(ray);
      if (ray.world_hit != nullptr && ray.world_hit != object &&
          !IsDescendantOf(ray.world_hit, object) && ray.world_fraction + 1.0e-4f < fraction)
      {
        continue;
      }
    }
    return true;
  }
  return false;
}

bool LN_UIService::SessionContainsObject(const MenuSession &session, KX_GameObject *object) const
{
  if (object == nullptr) {
    return false;
  }
  if (session.collection != nullptr &&
      m_scene.RuntimeObjectBelongsToOverlayCollection(
          object, session.collection, session.instance))
  {
    return true;
  }
  return std::any_of(
      session.objects.begin(), session.objects.end(), [&](const LN_ObjectHandle &handle) {
        return m_manager.ResolveObjectHandle(handle) == object;
      });
}

bool LN_UIService::SessionContainsHandle(const MenuSession &session,
                                         const LN_ObjectHandle &object) const
{
  return object.IsValid() && std::any_of(session.objects.begin(),
                                         session.objects.end(),
                                         [&](const LN_ObjectHandle &candidate) {
                                           return SameObjectHandle(candidate, object);
                                         });
}

bool LN_UIService::OtherSessionContainsHandle(const MenuHandle excluded_handle,
                                              const LN_ObjectHandle &object) const
{
  return std::any_of(m_sessions.begin(), m_sessions.end(), [&](const auto &item) {
    return item.first != excluded_handle && SessionContainsHandle(item.second, object);
  });
}

int LN_UIService::MenuStackIndexForObject(KX_GameObject *object) const
{
  if (object == nullptr || m_menuStack.empty()) {
    return -1;
  }
  /* Asked several times per control each frame: index every open menu's objects once, in time
   * linear to the menus' sizes, instead of searching each menu's collection on every call. */
  if (!m_menuIndexValid) {
    m_menuIndexByObject.clear();
    std::vector<KX_GameObject *> objects;
    for (size_t index = 0; index < m_menuStack.size(); ++index) {
      const auto session = m_sessions.find(m_menuStack[index]);
      if (session == m_sessions.end()) {
        continue;
      }
      objects.clear();
      m_scene.AppendOverlayRuntimeObjectsInCollection(
          session->second.collection, objects, session->second.instance);
      for (const LN_ObjectHandle &handle : session->second.objects) {
        objects.push_back(m_manager.ResolveObjectHandle(handle));
      }
      /* Upper menus come later and win, like the search from the top did. */
      for (const KX_GameObject *member : objects) {
        if (member != nullptr) {
          m_menuIndexByObject[member] = int(index);
        }
      }
    }
    m_menuIndexValid = true;
  }
  const auto item = m_menuIndexByObject.find(object);
  return item != m_menuIndexByObject.end() ? item->second : -1;
}

int32_t LN_UIService::UserOfObject(KX_GameObject *object) const
{
  const int index = MenuStackIndexForObject(object);
  if (index >= 0) {
    return MenuUser(index);
  }
  if (const int32_t controller = ControllerOf(object); controller >= 0) {
    return controller;
  }
  for (KX_GameObject *item = object; item != nullptr; item = item->GetParent()) {
    const blender::Object *blender_object = item->GetBlenderObject();
    if (blender_object != nullptr && (blender_object->game_ui.flag & blender::OB_UI_ELEMENT)) {
      return (blender_object->game_ui.flag & blender::OB_UI_ANY_PLAYER) ?
                 0 :
                 std::clamp(int32_t(blender_object->game_ui.player), 0, LN_MAX_GAMEPADS - 1);
    }
  }
  return 0;
}

bool LN_UIService::FocusShown(const int32_t user) const
{
  /* A pointer cursor shows where the player is, not a selection. */
  if (PointerActive(user)) {
    return false;
  }
  /* Only user zero has a mouse; other local users always play with a gamepad. Player 0 sees it
   * after using the keys, and at once while a gamepad is what they last used. */
  return user != 0 || m_directedUsers.contains(user) || LN_InputMap::Get().LastUsedGamepad();
}

bool LN_UIService::DirectedInputAllowed(const int32_t user) const
{
  /* While player 0 types into a Text Field, the keys are the text's, not navigation's. */
  if (user == 0 && m_editingText.IsValid()) {
    return false;
  }
  return InputMenuIndex(user) >= 0 || m_focusOutsideMenuUsers.contains(user) ||
         HoldsControl(user);
}

bool LN_UIService::HoldsControl(const int32_t user) const
{
  return std::any_of(m_controls.begin(), m_controls.end(), [&](const UIControl &control) {
    return control.user == user && m_manager.ResolveObjectHandle(control.root) != nullptr;
  });
}

int32_t LN_UIService::ControllerOf(KX_GameObject *object) const
{
  for (const UIControl &control : m_controls) {
    KX_GameObject *root = m_manager.ResolveObjectHandle(control.root);
    if (root != nullptr && IsDescendantOf(object, root)) {
      return control.user;
    }
  }
  return -1;
}

LN_UIResult LN_UIService::TakeControl(KX_GameObject *root, const int32_t user, const bool take)
{
  if (root == nullptr || user < 0 || user >= LN_MAX_GAMEPADS ||
      m_manager.IsGameObjectQueuedForRemoval(root))
  {
    return LN_UIResult::InvalidTarget;
  }
  if (root->GetScene() != &m_scene) {
    return LN_UIResult::WrongScene;
  }
  /* Another player's control of it, of a part of it or of what it is part of. */
  for (const UIControl &control : m_controls) {
    KX_GameObject *other = m_manager.ResolveObjectHandle(control.root);
    if (other != nullptr && control.user != user &&
        (IsDescendantOf(root, other) || IsDescendantOf(other, root)))
    {
      return LN_UIResult::ControlledByOtherPlayer;
    }
  }
  const auto own = std::find_if(m_controls.begin(), m_controls.end(), [&](const UIControl &item) {
    return item.user == user && m_manager.ResolveObjectHandle(item.root) == root;
  });
  if (take) {
    if (own != m_controls.end()) {
      return LN_UIResult::NoChange;
    }
    UIControl &control = m_controls.emplace_back();
    control.root = m_manager.MakeObjectHandle(root, root->GetName());
    control.user = user;
    return LN_UIResult::Success;
  }
  if (own == m_controls.end()) {
    return LN_UIResult::NoChange;
  }
  m_controls.erase(own);
  /* The selection it gave the player goes with it. */
  const auto focus = m_focusByUser.find(user);
  if (focus != m_focusByUser.end() &&
      IsDescendantOf(m_manager.ResolveObjectHandle(focus->second), root))
  {
    m_focusByUser.erase(focus);
    m_focusOutsideMenuUsers.erase(user);
  }
  return LN_UIResult::Success;
}

int32_t LN_UIService::MenuUser(const int index) const
{
  if (index < 0 || size_t(index) >= m_menuStack.size()) {
    return 0;
  }
  const auto session = m_sessions.find(m_menuStack[size_t(index)]);
  return session != m_sessions.end() ? session->second.user : 0;
}

int LN_UIService::TopMenuIndex(const int32_t user) const
{
  for (int index = int(m_menuStack.size()) - 1; index >= 0; --index) {
    const auto session = m_sessions.find(m_menuStack[size_t(index)]);
    if (session != m_sessions.end() && session->second.user == user) {
      return index;
    }
  }
  return -1;
}

int LN_UIService::InputMenuIndex(const int32_t user) const
{
  for (int index = int(m_menuStack.size()) - 1; index >= 0; --index) {
    const auto session = m_sessions.find(m_menuStack[size_t(index)]);
    if (session == m_sessions.end() || session->second.user != user) {
      continue;
    }
    if (!IsHudLayer(session->second)) {
      return index;
    }
  }
  return -1;
}

bool LN_UIService::IsHudLayer(const MenuSession &session)
{
  return session.routing == LN_UIMenuRouting::PassThrough &&
         session.back_behavior != LN_UIMenuBackBehavior::CloseMenu && !session.has_selectable;
}

void LN_UIService::UpdateMenuSelectables()
{
  for (auto &item : m_sessions) {
    item.second.has_selectable = false;
  }
  for (const Registration &registration : m_registrations) {
    if (!registration.enabled || !registration.focusable || !registration.navigable) {
      continue;
    }
    const int index = MenuStackIndexForObject(registration.object);
    if (index < 0 || size_t(index) >= m_menuStack.size()) {
      continue;
    }
    const auto session = m_sessions.find(m_menuStack[size_t(index)]);
    if (session != m_sessions.end() && session->second.user == registration.user) {
      session->second.has_selectable = true;
    }
  }
}

int LN_UIService::ModalFloorIndex(const int32_t user) const
{
  for (int index = int(m_menuStack.size()) - 1; index >= 0; --index) {
    const auto session = m_sessions.find(m_menuStack[size_t(index)]);
    if (session != m_sessions.end() && session->second.user == user &&
        session->second.routing == LN_UIMenuRouting::Modal)
    {
      return index;
    }
  }
  return -1;
}

bool LN_UIService::MenuIndexPassesFloor(const int index, const int32_t outside_user) const
{
  return index >= ModalFloorIndex(index >= 0 ? MenuUser(index) : outside_user);
}

bool LN_UIService::ObjectPassesModalRouting(KX_GameObject *object, const int32_t user) const
{
  return object != nullptr && SessionAllowsInteraction(object) &&
         MenuIndexPassesFloor(MenuStackIndexForObject(object), user);
}

bool LN_UIService::ObjectAcceptsDirectedInput(KX_GameObject *object, const int32_t user) const
{
  if (!ObjectPassesModalRouting(object, user)) {
    return false;
  }
  const int top = InputMenuIndex(user);
  if (top < 0) {
    return true;
  }
  return MenuStackIndexForObject(object) == top;
}

bool LN_UIService::SessionAllowsInteraction(KX_GameObject *object) const
{
  if (object == nullptr) {
    return false;
  }
  const int index = MenuStackIndexForObject(object);
  if (index < 0 || size_t(index) >= m_menuStack.size()) {
    return true;
  }
  const auto session = m_sessions.find(m_menuStack[size_t(index)]);
  return session != m_sessions.end() &&
         session->second.state != MenuSession::State::CoveredHidden;
}

void LN_UIService::SnapshotSessionObjects(MenuSession &session)
{
  session.objects.clear();
  std::vector<KX_GameObject *> session_objects;
  m_scene.AppendOverlayCollectionRuntimeObjects(
      session.collection, session_objects, session.instance);
  session.objects.reserve(session_objects.size());
  for (KX_GameObject *object : session_objects) {
    if (object != nullptr && !m_manager.IsGameObjectQueuedForRemoval(object)) {
      session.objects.push_back(m_manager.MakeObjectHandle(object, object->GetName()));
    }
  }
  session.membership_generation = m_scene.GetOverlayMembershipGeneration(session.collection);
}

void LN_UIService::RefreshSessionMembership(MenuSession &session)
{
  const uint64_t generation = m_scene.GetOverlayMembershipGeneration(session.collection);
  if (generation == 0 || generation == session.membership_generation) {
    return;
  }
  std::unordered_set<KX_GameObject *> previous;
  previous.reserve(session.objects.size());
  for (const LN_ObjectHandle &handle : session.objects) {
    if (KX_GameObject *object = m_manager.ResolveObjectHandle(handle)) {
      previous.insert(object);
    }
  }
  SnapshotSessionObjects(session);
  std::unordered_set<KX_GameObject *> current;
  current.reserve(session.objects.size());
  for (const LN_ObjectHandle &handle : session.objects) {
    if (KX_GameObject *object = m_manager.ResolveObjectHandle(handle)) {
      current.insert(object);
    }
  }
  for (KX_GameObject *object : previous) {
    if (!current.contains(object)) {
      ReleaseObjectState(object);
    }
  }
}

bool LN_UIService::MenuSettingsMatch(const MenuSession &session,
                                    KX_Camera *camera,
                                    blender::Collection *collection,
                                    const LN_UIMenuRouting routing,
                                    const LN_UIMenuPresentation presentation,
                                    const LN_UIMenuBackBehavior back_behavior,
                                    const std::string &input_context) const
{
  const bool same_camera = session.camera == camera ||
                           (m_scene.OverlayCameraMatches(session.camera) &&
                            m_scene.OverlayCameraMatches(camera));
  return same_camera && session.collection == collection && session.routing == routing &&
         session.presentation == presentation && session.back_behavior == back_behavior &&
         session.input_context == input_context;
}

LN_UIService::MenuHandle LN_UIService::FindSessionForCollection(
    const blender::Collection *collection, const int32_t user) const
{
  if (collection == nullptr) {
    return 0;
  }
  for (const MenuHandle handle : m_menuStack) {
    const auto session = m_sessions.find(handle);
    if (session != m_sessions.end() && session->second.collection == collection &&
        (user < 0 || session->second.user == user))
    {
      return handle;
    }
  }
  return 0;
}

KX_GameObject *LN_UIService::MenuOwnerOfObject(KX_GameObject *object) const
{
  const int index = MenuStackIndexForObject(object);
  if (index < 0 || size_t(index) >= m_menuStack.size()) {
    return nullptr;
  }
  const auto session = m_sessions.find(m_menuStack[size_t(index)]);
  return session != m_sessions.end() ?
             m_manager.ResolveObjectHandle(session->second.owner_object) :
             nullptr;
}

bool LN_UIService::OwnerLivesOnlyOnClosingCollection(KX_GameObject *owner,
                                                     const MenuSession &session) const
{
  if (owner == nullptr || session.collection == nullptr) {
    return false;
  }
  /* The overlay staying for other menus keeps this menu's objects only for copy 0 also shown by
   * Set Overlay Collection: a player's copy goes with their menu. */
  if (m_scene.OverlayWouldExistWithoutLease(session.collection, session.handle) &&
      session.instance == 0 && m_scene.OverlayOpenedIndependently(session.collection))
  {
    return false;
  }
  return m_scene.RuntimeObjectBelongsToOverlayCollection(
      owner, session.collection, session.instance);
}

bool LN_UIService::ClosingWouldDestroyOwner(KX_GameObject *owner,
                                            const MenuHandle handle,
                                            const LN_UIMenuCloseOperation operation,
                                            const int32_t user) const
{
  if (owner == nullptr || m_menuStack.empty()) {
    return false;
  }
  if (operation == LN_UIMenuCloseOperation::All) {
    return std::any_of(
        m_menuStack.begin(), m_menuStack.end(), [this, owner, user](const MenuHandle item) {
          const auto session = m_sessions.find(item);
          return session != m_sessions.end() && session->second.user == user &&
                 OwnerLivesOnlyOnClosingCollection(owner, session->second);
        });
  }
  MenuHandle target = handle;
  if (operation == LN_UIMenuCloseOperation::Top) {
    const int top = InputMenuIndex(user);
    if (top < 0) {
      return false;
    }
    target = m_menuStack[size_t(top)];
  }
  const auto session = m_sessions.find(target);
  return session != m_sessions.end() && OwnerLivesOnlyOnClosingCollection(owner, session->second);
}

bool LN_UIService::ReplaceTopWouldDestroyOwner(KX_GameObject *owner, const int32_t user) const
{
  const int top = InputMenuIndex(user);
  if (owner == nullptr || top < 0) {
    return false;
  }
  const auto session = m_sessions.find(m_menuStack[size_t(top)]);
  if (session == m_sessions.end() || !OwnerLivesOnlyOnClosingCollection(owner, session->second)) {
    return false;
  }
  /* The new menu takes over the replaced menu's owner when that one lives on (see OpenMenu). */
  KX_GameObject *inherited = m_manager.ResolveObjectHandle(session->second.owner);
  return inherited == nullptr || session->second.owner_tree == nullptr ||
         m_manager.IsGameObjectQueuedForRemoval(inherited) ||
         OwnerLivesOnlyOnClosingCollection(inherited, session->second);
}

void LN_UIService::RecomputeCoverage()
{
  /* Each player's menus cover only that player's menus below them: in split screen one player's
   * full-screen menu must not hide another player's. */
  std::unordered_map<int32_t, bool> hide_below;
  for (int index = int(m_menuStack.size()) - 1; index >= 0; --index) {
    const auto session_it = m_sessions.find(m_menuStack[size_t(index)]);
    if (session_it == m_sessions.end()) {
      continue;
    }
    MenuSession &session = session_it->second;
    const auto covered = hide_below.find(session.user);
    if (covered == hide_below.end()) {
      session.state = MenuSession::State::Open;
      hide_below.emplace(session.user,
                         session.presentation == LN_UIMenuPresentation::Screen);
    }
    else {
      session.state = covered->second ? MenuSession::State::CoveredHidden :
                                        MenuSession::State::CoveredVisible;
      if (session.presentation == LN_UIMenuPresentation::Screen) {
        covered->second = true;
      }
    }
    m_scene.SetOverlayMenuLeaseVisible(
        session.collection, session.handle, session.state != MenuSession::State::CoveredHidden);
  }
}

bool LN_UIService::IsRegisteredFocusable(const int32_t user, KX_GameObject *object) const
{
  return object != nullptr &&
         std::any_of(m_registrations.begin(),
                     m_registrations.end(),
                     [&](const Registration &registration) {
                       return registration.user == user && registration.enabled &&
                              registration.focusable && registration.object == object;
                     });
}

bool LN_UIService::ContextMatches(const std::string &requested, const int32_t user) const
{
  return requested.empty() || requested == GetActiveInputContext(user);
}

bool LN_UIService::TryConsumeCancelToken()
{
  if (m_cancelConsumed || m_input == nullptr) {
    return false;
  }
  SCA_IInputDevice *device = m_input->GetDevice();
  const LN_InputMap &map = LN_InputMap::Get();
  const uint8_t held = LN_InputModifiersHeld(*m_input);
  /* The UI used this press: Keyboard Key and Gamepad Button nodes must not react to it again, or
   * "Esc → Open Object Menu" would reopen the menu that this Esc just closed. */
  for (const LN_InputBinding &binding : map.Action(map.FindBuiltin(LN_UIAction::Cancel)).bindings)
  {
    if (LN_InputBindingStick(binding) != 0) {
      if (StickBindingState(binding, LN_UIActionState::Triggered, 0)) {
        m_cancelConsumed = true;
        return true;
      }
      continue;
    }
    if (!LN_ReadInputBinding(binding, *m_input, 0, held).pressed) {
      continue;
    }
    if (binding.device == LN_InputDevice::Gamepad) {
      if (binding.code <= GAME_INPUT_GAMEPAD_DPAD_RIGHT) {
        m_manager.ConsumeGamepadPress(LN_InputMap::Get().GamepadOfUser(0), binding.code);
      }
    }
    else if (IsWheelBinding(binding)) {
      m_manager.ConsumeWheel();
    }
    /* The press token is shared with the Exit Key: the one that takes it first owns the press. */
    else if (device != nullptr &&
             !device->ConsumePressTransition(SCA_IInputDevice::SCA_EnumInputs(binding.input)))
    {
      continue;
    }
    else {
      m_manager.ConsumeInputPress(binding.input);
    }
    m_cancelConsumed = true;
    return true;
  }
  return false;
}

bool LN_UIService::OpenedByThisBack(const MenuSession &session) const
{
  /* Opened by a Logic + Physics step since the last frame update (an Input Action route): that
   * step may have seen this frame's Back first and opened the menu with it. The press is used up,
   * and the menu stays. */
  return !session.opened_in_frame_update && session.opened_frame + 1 >= m_frameIndex;
}

void LN_UIService::ProcessPlayerBack(const int32_t user)
{
  const int top = InputMenuIndex(user);
  if (top < 0 || m_input == nullptr ||
      !GetActionState(
          LN_UIAction::Cancel, LN_UIActionState::Triggered, user, GetActiveInputContext(user)))
  {
    return;
  }
  /* A carry of this player is put back by its own Back in ResolveCarryForUser. */
  for (const auto &pointers : m_pointerStates) {
    const auto pointer = pointers.find(user);
    if (pointer != pointers.end() && (pointer->second.has_capture || pointer->second.drag_active))
    {
      return;
    }
  }
  const auto session = m_sessions.find(m_menuStack[size_t(top)]);
  if (session == m_sessions.end()) {
    return;
  }
  /* Custom leaves the press to the game (this player's Input Action "Back"), unless it is the
   * press that opened the menu. */
  const bool opened_by_it = OpenedByThisBack(session->second);
  if (session->second.back_behavior == LN_UIMenuBackBehavior::Custom && !opened_by_it) {
    return;
  }
  /* Close Menu closes this player's top menu; Ignore uses the press up so that nothing else
   * reacts to it (as for player 0). */
  ConsumeBindingPresses(LN_UIAction::Cancel, user, *m_input, BindingFilter::NotMouse);
  m_playerBackUsed |= uint8_t(1u << user);
  if (opened_by_it) {
    return;
  }
  if (session->second.back_behavior == LN_UIMenuBackBehavior::CloseMenu) {
    NoteBackSound(session->second);
    CloseMenu(session->second.handle, LN_UIMenuCloseOperation::Specific);
  }
}

void LN_UIService::NoteBackSound(const MenuSession &session)
{
  SoundEvent &back = m_backSounds.emplace_back();
  back.sound = UISound::Back;
  back.user = session.user;
  back.menu = session.collection != nullptr ? session.collection->id.name + 2 : std::string();
}

std::string LN_UIService::MenuNameAt(const int menu_index) const
{
  if (menu_index < 0 || size_t(menu_index) >= m_menuStack.size()) {
    return std::string();
  }
  const auto session = m_sessions.find(m_menuStack[size_t(menu_index)]);
  return session != m_sessions.end() && session->second.collection != nullptr ?
             std::string(session->second.collection->id.name + 2) :
             std::string();
}

void LN_UIService::ProcessSemanticCancel()
{
  for (int32_t user = 1; user < LN_MAX_GAMEPADS; ++user) {
    ProcessPlayerBack(user);
  }
  const bool cancel_triggered = GetActionState(
      LN_UIAction::Cancel, LN_UIActionState::Triggered, 0, GetActiveInputContext());
  if (!cancel_triggered || m_cancelConsumed) {
    return;
  }
  /* Back while typing finishes the Text Field (ResolveTextEditing); the menu stays open. */
  if (m_editingText.IsValid()) {
    if (TryConsumeCancelToken()) {
      m_textEditCancelled = true;
    }
    return;
  }

  const size_t phase = PhaseIndex(m_event);
  bool has_capture = false;
  const auto pointer = m_pointerStates[phase].find(0);
  if (pointer != m_pointerStates[phase].end()) {
    has_capture = pointer->second.has_capture || pointer->second.drag_active;
  }
  if (has_capture) {
    if (TryConsumeCancelToken()) {
      CancelPointer(phase, 0, true);
    }
    return;
  }

  if (const int top = InputMenuIndex(0); top >= 0) {
    const auto session = m_sessions.find(m_menuStack[size_t(top)]);
    if (session == m_sessions.end()) {
      return;
    }
    /* Custom leaves the menu open and the press to the game (Input Action "Back"), unless it is
     * the press that opened the menu (as for players 1-7 in ProcessPlayerBack). */
    const bool opened_by_it = OpenedByThisBack(session->second);
    if (session->second.back_behavior == LN_UIMenuBackBehavior::Custom && !opened_by_it) {
      return;
    }
    if (!TryConsumeCancelToken() || opened_by_it) {
      return;
    }
    if (session->second.back_behavior == LN_UIMenuBackBehavior::CloseMenu) {
      NoteBackSound(session->second);
      CloseMenu(session->second.handle, LN_UIMenuCloseOperation::Specific);
    }
    return;
  }
  /* No menu is open: the press is the game's (Input Action "Back", the Exit Key). */
}

void LN_UIService::ResolveInteractions()
{
  if (m_resolving) {
    return;
  }
  m_resolving = true;
  UpdatePlayerPointers();
  ApplyPendingInitialFocus();
  /* Set by the pointer of user zero below. */
  m_pointerBlockedByUI = false;
  m_pointerOverBlockingUI = false;
  m_blockSurfaces.clear();
  m_previousPointerHovers = m_pointerHovers;
  m_pointerHovers.fill(LN_ObjectHandle());
  m_pointerGrabUsed = 0;
  /* How long player 0's keys have worked where they work now (m_mouseUsedForKeysIn). */
  const int64_t keys_place = PlayerZeroKeysPlace();
  m_keysPlaceAge = keys_place == m_keysPlace ? m_keysPlaceAge + m_frameDelta : 0.0;
  m_keysPlace = keys_place;
  m_pressedDisabledElement = false;
  m_disabledBlocker = LN_ObjectHandle();
  for (PlayerPointer &pointer : m_playerPointers) {
    pointer.aim_disabled_block = false;
  }
  /* Each player's pointer press this frame: player 0's mouse, or a Set Player Pointer's Accept. */
  std::array<bool, LN_MAX_GAMEPADS> pointer_pressed = {};
  for (int32_t user = 0; user < LN_MAX_GAMEPADS && m_input != nullptr; ++user) {
    pointer_pressed[size_t(user)] =
        VirtualPointerActive(user) ?
            !(user == 0 && m_editingText.IsValid()) &&
                PointerButtonState(user, LN_UIActionState::Triggered) :
            user == 0 &&
                m_input->GetStatus(SCA_IInputDevice::LEFTMOUSE, SCA_InputEvent::JUSTACTIVATED);
  }

  /* View Center must not reach through walls: one physics ray per player, cast only when a UI
   * Element is in its way. The objects the player's camera hangs from (their body) and the shown
   * pointer cursors don't count. */
  struct AimRay {
    bool cast = false;
    KX_GameObject *hit = nullptr;
    float fraction = 1.0f;
  };
  std::array<AimRay, LN_MAX_GAMEPADS> aim_rays;
  const auto aim_world_hit = [&](const int32_t user) -> const AimRay & {
    AimRay &ray = aim_rays[size_t(user)];
    if (ray.cast) {
      return ray;
    }
    ray.cast = true;
    const PlayerPointer &pointer = m_playerPointers[size_t(user)];
    const float length = (pointer.ray_to - pointer.ray_from).length();
    if (length <= MT_EPSILON) {
      return ray;
    }
    std::vector<KX_GameObject *> ignored = ShownPointerCursors();
    KX_Camera *view_camera = PlayerViewCamera(user);
    if (view_camera != nullptr) {
      KX_GameObject *root = view_camera;
      while (root->GetParent() != nullptr) {
        root = root->GetParent();
      }
      ignored.push_back(root);
    }
    PHY_IPhysicsEnvironment *environment = m_worldRayForTests ? nullptr :
                                                                m_scene.GetPhysicsEnvironment();
    if (!m_worldRayForTests && environment == nullptr) {
      return ray;
    }
    /* A shape the ray starts in (a body around a camera that only follows it) is hit at once,
     * convex shapes being solid: it hides nothing, cast again without it. */
    for (int attempt = 0; attempt < 4; ++attempt) {
      KX_GameObject *hit = nullptr;
      float fraction = 1.0f;
      if (m_worldRayForTests) {
        hit = m_worldRayForTests(pointer.ray_from, pointer.ray_to, ignored, fraction);
      }
      else {
        UIWorldRayCallback callback(m_scene);
        callback.m_ignored = ignored;
        callback.m_camera = view_camera;
        KX_RayCast::RayTest(environment, pointer.ray_from, pointer.ray_to, callback);
        hit = callback.m_object;
        fraction = hit != nullptr ? (callback.m_hitPoint - pointer.ray_from).length() / length :
                                    1.0f;
      }
      if (hit != nullptr && fraction * length <= 1.0e-4f) {
        ignored.push_back(hit);
        continue;
      }
      ray.hit = hit;
      ray.fraction = fraction;
      break;
    }
    return ray;
  };

  /* A Set Player Pointer points straight into its surface, from in front of the surface's
   * buttons, or from the player's camera (View Center): in the game world, not into menus (their
   * keys and gamepad drive those), nor at a view's scrollbar (it follows the mouse). */
  for (Registration &registration : m_registrations) {
    if (!VirtualPointerActive(registration.user)) {
      continue;
    }
    if (registration.interaction_domain != LN_InteractionDomain::Overlay &&
        !(registration.listener.tree != nullptr && registration.listener.element != nullptr))
    {
      const PlayerPointer &pointer = m_playerPointers[size_t(registration.user)];
      registration.ray_from = pointer.ray_from;
      registration.ray_to = pointer.ray_to;
      registration.has_precomputed_hit = false;
      if (pointer.aim) {
        float fraction = 1.0f;
        MT_Vector3 point;
        MT_Vector3 normal;
        /* Tested by the ray before it is marked as tested: RayIntersectsRegistration answers
         * from the stored result once has_precomputed_hit is set. */
        bool hit = RayIntersectsRegistration(registration, fraction, point, normal);
        if (hit) {
          const AimRay &world = aim_world_hit(registration.user);
          hit = world.hit == nullptr || world.hit == registration.object ||
                IsDescendantOf(world.hit, registration.object) ||
                world.fraction + 1.0e-4f >= fraction;
        }
        registration.has_precomputed_hit = true;
        registration.precomputed_hit = hit;
        registration.precomputed_fraction = fraction;
        registration.precomputed_point = point;
        registration.precomputed_normal = normal;
      }
    }
    else {
      /* Nothing else of that player's is hit, also not by the mouse ray a tree computed. */
      registration.has_precomputed_hit = true;
      registration.precomputed_hit = false;
    }
  }

  std::vector<Hit> hits;
  hits.reserve(m_registrations.size());
  std::unordered_set<int32_t> users;
  for (const auto &declaration : m_listenerDeclarations) {
    users.insert(declaration.second.user);
  }
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    if (PointerActive(user)) {
      users.insert(user);
    }
  }
  for (size_t index = 0; index < m_registrations.size(); ++index) {
    const Registration &registration = m_registrations[index];
    users.insert(registration.user);
    float fraction = 1.0f;
    MT_Vector3 point;
    MT_Vector3 normal;
    /* A press on a disabled UI Element does nothing: the Denied sound tells (not for one under a
     * modal menu, which the click could not reach). Player 0's mouse also keeps the game from
     * one with Block. */
    const bool mouse = registration.user == 0 && !VirtualPointerActive(0);
    const bool aim = AimPointerActive(registration.user);
    const bool pressed_here = registration.user >= 0 && registration.user < LN_MAX_GAMEPADS &&
                              pointer_pressed[size_t(registration.user)];
    if (!registration.enabled && registration.listener.element != nullptr &&
        (mouse || VirtualPointerActive(registration.user)) &&
        registration.propagation != LN_UIPropagation::Ignore &&
        (pressed_here ||
         ((mouse || aim) && registration.propagation == LN_UIPropagation::Block)) &&
        SessionAllowsInteraction(registration.object) &&
        ObjectPassesModalRouting(registration.object, registration.user) &&
        RayIntersectsRegistration(registration, fraction, point, normal))
    {
      if (pressed_here && !m_pressedDisabledElement) {
        m_pressedDisabledElement = true;
        m_pressedDisabled.sound = UISound::Denied;
        m_pressedDisabled.user = registration.user;
        m_pressedDisabled.object = m_manager.MakeObjectHandle(registration.object,
                                                              registration.object->GetName());
        const ElementRecord *record = FindElementRecord(registration.object);
        m_pressedDisabled.action = record != nullptr ? record->settings.action : std::string();
        m_pressedDisabled.menu = MenuNameAt(record != nullptr ? record->menu_index : -1);
      }
      /* Disabled, it still keeps the pointer from the game (a gun must not fire through it). */
      if (mouse && registration.propagation == LN_UIPropagation::Block) {
        m_disabledBlocker = m_manager.MakeObjectHandle(registration.object,
                                                       registration.object->GetName());
      }
      /* ...and View Center's press (Use must not open a door through a locked keypad). */
      if (aim && registration.propagation == LN_UIPropagation::Block) {
        m_playerPointers[size_t(registration.user)].aim_disabled_block = true;
      }
    }
    if (registration.enabled && registration.propagation != LN_UIPropagation::Ignore &&
        SessionAllowsInteraction(registration.object) &&
        RayIntersectsRegistration(registration, fraction, point, normal))
    {
      Hit hit;
      hit.registration = &registration;
      hit.fraction = fraction;
      hit.point = point;
      hit.normal = normal;
      hit.menu_index = MenuStackIndexForObject(registration.object);
      hit.domain_rank = registration.interaction_domain == LN_InteractionDomain::Overlay ? 1 : 0;
      hit.sequence = uint64_t(index);
      hits.push_back(hit);
    }
  }

  UpdateMenuSelectables();
  m_directedTargetUsers.clear();
  for (const Registration &registration : m_registrations) {
    if (registration.enabled && registration.focusable &&
        ObjectAcceptsDirectedInput(registration.object, registration.user))
    {
      m_directedTargetUsers.insert(registration.user);
    }
  }

  for (const int32_t user : users) {
    /* Decide with the input mode from before this frame's input, so the press that reveals a
     * hidden focus does not also move or press it. */
    const bool focus_shown = FocusShown(user);
    /* While a player's pointer is on, their stick moves it and Accept clicks under it: no
     * selection moves or is pressed. */
    const bool pointing = PointerActive(user);
    if (!pointing) {
      ResolveNavigationForUser(user, focus_shown);
    }
    ResolvePointerForUser(user, hits);
    if (!pointing && !ResolveCarryForUser(user, focus_shown)) {
      ResolveFocusedClickForUser(user, focus_shown, LN_UIAction::Accept);
      ResolveFocusedClickForUser(user, focus_shown, LN_UIAction::Secondary);
    }
  }
  /* Gamepad use makes its player a gamepad player even before any UI exists (Start opening the
   * pause menu), so the menu shows its selection at once. */
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    const int32_t gamepad_index = LN_InputMap::Get().GamepadOfUser(user);
    if (gamepad_index < 0) {
      continue;
    }
    bool stick_moved = false;
    for (size_t stick = 0; stick < 2; ++stick) {
      const StickDirection direction = m_stickDirection[size_t(gamepad_index)][stick];
      stick_moved = stick_moved ||
                    (direction != StickDirection::None &&
                     direction != m_previousStickDirection[size_t(gamepad_index)][stick]);
    }
    if (AnyGamepadButtonPressed(m_input, user) || stick_moved) {
      m_directedUsers.insert(user);
    }
  }
  /* The UI takes these whole presses: game logic sees no press, hold or release of them. Over a
   * UI Element with Block, every mouse button and the wheel; elsewhere the click that puts down
   * what a click picked up. */
  if (m_input != nullptr) {
    if (m_pointerOverBlockingUI) {
      ConsumePointerOverUI(*m_input);
    }
    else if (m_pointerBlockedByUI &&
             m_input->GetStatus(SCA_IInputDevice::LEFTMOUSE, SCA_InputEvent::JUSTACTIVATED))
    {
      m_manager.ConsumeInputStroke(SCA_IInputDevice::LEFTMOUSE);
    }
  }

  /* A listener has exactly one registration. */
  std::unordered_map<ListenerKey, const Registration *, ListenerKeyHash> first_registration;
  first_registration.reserve(m_registrations.size());
  for (const Registration &registration : m_registrations) {
    first_registration.try_emplace(registration.listener, &registration);
  }

  const size_t phase = PhaseIndex(m_event);
  std::unordered_set<ListenerKey, ListenerKeyHash> current_listeners;
  current_listeners.reserve(m_listenerDeclarations.size());
  for (const auto &declaration_item : m_listenerDeclarations) {
    const ListenerKey &listener = declaration_item.first;
    const ListenerDeclaration &declaration = declaration_item.second;
    current_listeners.insert(listener);
    InteractionState &state = m_currentStates[listener];

    const auto first = first_registration.find(listener);
    const Registration *listener_registration = first != first_registration.end() ?
                                                    first->second :
                                                    nullptr;
    const auto find_registration = [&](const KX_GameObject *object) -> const Registration * {
      return listener_registration != nullptr && listener_registration->object == object ?
                 listener_registration :
                 nullptr;
    };

    const Registration *selected = nullptr;
    KX_GameObject *state_object = m_manager.ResolveObjectHandle(state.object);
    if (state_object != nullptr) {
      selected = find_registration(state_object);
    }

    if (selected == nullptr && listener_registration != nullptr) {
      selected = listener_registration;
      state.object = m_manager.MakeObjectHandle(selected->object, selected->object->GetName());
      state_object = selected->object;
    }

    const auto focus = m_focusByUser.find(declaration.user);
    KX_GameObject *focused_object = focus != m_focusByUser.end() ?
                                        m_manager.ResolveObjectHandle(focus->second) :
                                        nullptr;

    if (selected != nullptr && selected->role == LN_UIInteractableRole::Control) {
      state.target_object = state.object;
    }
    state.focused = selected != nullptr && selected->enabled && selected->focusable &&
                    ObjectPassesModalRouting(selected->object, declaration.user) &&
                    focused_object == state_object;
    /* Shown only where keys and gamepad can drive it: not on a HUD button that kept a clicked
     * focus after a menu closed. */
    state.focus_visible = state.focused && FocusShown(declaration.user) &&
                          DirectedInputAllowed(declaration.user);
  }
  for (auto it = m_listenerStates[phase].begin(); it != m_listenerStates[phase].end();) {
    if (!current_listeners.contains(it->first)) {
      it = m_listenerStates[phase].erase(it);
    }
    else {
      ++it;
    }
  }
  /* A declaration can disappear without its tree or object being destroyed (for example, a
   * dynamic target or camera becomes invalid). Capture is phase-local, so cancel it immediately
   * instead of retaining a listener key that could become valid again and resume a stale press. */
  std::vector<int32_t> cancelled_users;
  for (const auto &pointer : m_pointerStates[phase]) {
    if (pointer.second.has_capture &&
        !current_listeners.contains(pointer.second.captured_listener))
    {
      cancelled_users.push_back(pointer.first);
    }
  }
  for (const int32_t user : cancelled_users) {
    CancelPointer(phase, user, true);
  }
  ProcessSemanticCancel();
  ResolveHighlights();
  BuildElementEvents();
  ResolveWidgets();
  m_resolving = false;
}

void LN_UIService::ResolveHighlights()
{
  m_highlightChanges.clear();
  const auto element_highlight = [&](KX_GameObject *object, const int32_t user, Highlight &r) {
    const ElementRecord *record = object != nullptr ? FindElementRecord(object) : nullptr;
    if (record == nullptr || (record->users & UserBit(user)) == 0 ||
        !object->GetUIInteractable() || IsBackgroundElement(record->settings))
    {
      return false;
    }
    r.object = record->handle;
    r.action = record->settings.action;
    r.menu_index = record->menu_index;
    r.domain_rank = record->domain_rank;
    r.priority = record->settings.priority;
    LN_Value item;
    int32_t index = -1;
    r.item_fingerprint = 0;
    if (GetBoundItem(object, item, index)) {
      r.item_fingerprint = ValueFingerprint(item);
      HashCombine(r.item_fingerprint, size_t(index) + 1);
    }
    return true;
  };
  std::unordered_map<int32_t, Highlight> current;
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    Highlight now;
    bool has = false;
    if (FocusShown(user) && DirectedInputAllowed(user)) {
      has = element_highlight(GetFocus(user, GetActiveInputContext(user)), user, now);
    }
    else if (user == 0 || PointerActive(user)) {
      /* What the player's pointer is over. While typing, the Text Field keeps the highlight
       * wherever the mouse is. */
      has = element_highlight(
          m_manager.ResolveObjectHandle(user == 0 && m_editingText.IsValid() ?
                                            m_editingText :
                                            m_pointerHovers[size_t(user)]),
          user,
          now);
    }
    /* Compared with what the events last named. */
    const auto previous = m_highlights.find(user);
    const bool had = previous != m_highlights.end() && previous->second.announced;
    if (has != had ||
        (has && (!SameObjectHandle(now.object, previous->second.object) ||
                 now.item_fingerprint != previous->second.item_fingerprint)))
    {
      HighlightChange &change = m_highlightChanges.emplace_back();
      change.user = user;
      change.had = had;
      change.has = has;
      if (had) {
        change.from = previous->second;
      }
      if (has && had && now.action != previous->second.action) {
        /* To another Action's element: its event comes next frame, after this frame's leave
         * event, so a text line both write ends with the new element whatever the node order. */
        change.has = false;
        now.announced = false;
      }
      else if (has) {
        change.to = now;
      }
    }
    if (has) {
      current.emplace(user, std::move(now));
    }
  }
  m_highlights = std::move(current);
}

void LN_UIService::CancelPointer(const size_t phase, const int32_t user, const bool emit_event)
{
  const auto pointer_it = m_pointerStates[phase].find(user);
  if (pointer_it == m_pointerStates[phase].end()) {
    return;
  }
  const PointerState pointer = pointer_it->second;
  if (pointer.has_capture) {
    InteractionState &source = m_currentStates[pointer.captured_listener];
    source.captured = false;
    source.dragging = false;
  }
  if (emit_event && pointer.drag_active) {
    InteractionState &source = m_currentStates[pointer.captured_listener];
    source.valid = true;
    source.drag_cancelled = true;
    source.drag_ended = true;
    source.dragging = false;
    source.captured = false;
    source.source_object = pointer.captured_object;
    source.drag_amount = pointer.drag_amount;
    if (pointer.has_drop_target) {
      if (!pointer.drop_refused) {
        source.target_object = pointer.drop_object;
      }
      InteractionState &target = m_currentStates[pointer.drop_listener];
      target.valid = true;
      target.drop_exited = true;
      target.drop_over = false;
      target.drop_refused = false;
      target.source_object = pointer.captured_object;
      target.drag_amount = pointer.drag_amount;
      target.target_object = pointer.drop_object;
    }
  }
  const auto source_state = m_listenerStates[phase].find(pointer.captured_listener);
  if (source_state != m_listenerStates[phase].end()) {
    source_state->second.pressed_inside = false;
    source_state->second.dragging = false;
  }
  m_pointerStates[phase].erase(pointer_it);
}

void LN_UIService::ResolvePointerForUser(const int32_t user, const std::vector<Hit> &all_hits)
{
  const size_t phase = PhaseIndex(m_event);
  /* The system mouse is user zero's. Other users point only with a Set Player Pointer cursor;
   * otherwise they keep independent focus/navigation through their gamepads without mirroring
   * mouse hover and click events into every player's UI state. Their Grab carries are not
   * pointer state. */
  const bool virtual_pointer = VirtualPointerActive(user);
  if (user != 0 && !virtual_pointer) {
    const auto carry = m_pointerStates[phase].find(user);
    if (carry != m_pointerStates[phase].end() && carry->second.carried) {
      return;
    }
    for (const auto &declaration : m_listenerDeclarations) {
      if (declaration.second.user != user) {
        continue;
      }
      ListenerPersistentState &persistent = m_listenerStates[phase][declaration.first];
      InteractionState &state = m_currentStates[declaration.first];
      if (persistent.over) {
        state.exited = true;
      }
      persistent.over = false;
      persistent.pressed_inside = false;
      persistent.dragging = false;
    }
    m_pointerStates[phase].erase(user);
    return;
  }
  /* A Grab carry is driven by keys and gamepad; a mouse press puts it back and takes over, and
   * moving the mouse hands it to the mouse. Until then the carried element is just an element
   * under the pointer, not a mouse drag. */
  /* A right click puts back what is carried or was picked up by a click, and clicks nothing
   * else. */
  bool carrying = false;
  bool right_click_used = false;
  /* The mouse's buttons, or a pointer cursor's Accept (its left button) and Secondary keys and
   * gamepad controls (its right button). */
  const BindingFilter button_filter = virtual_pointer ? BindingFilter::NotMouse :
                                                        BindingFilter::Mouse;
  /* View Center presses with its Press Action alone: no right click (a game's aim button). */
  const bool aim = AimPointerActive(user);
  /* While player 0 types into a Text Field, Enter and Space are the text's. */
  const bool typing = user == 0 && m_editingText.IsValid();
  const auto primary_state = [&](const LN_UIActionState state) {
    if (virtual_pointer) {
      return !typing && PointerButtonState(user, state);
    }
    const SCA_InputEvent::SCA_EnumInputs status = state == LN_UIActionState::Triggered ?
                                                      SCA_InputEvent::JUSTACTIVATED :
                                                  state == LN_UIActionState::Released ?
                                                      SCA_InputEvent::JUSTRELEASED :
                                                      SCA_InputEvent::ACTIVE;
    return m_input->GetStatus(SCA_IInputDevice::LEFTMOUSE, status);
  };
  if (const auto held = m_pointerStates[phase].find(user);
      held != m_pointerStates[phase].end() && (held->second.carried || held->second.picked_up))
  {
    const bool carried = held->second.carried;
    if (m_input != nullptr && !aim &&
        ActionBindingState(
            LN_UIAction::Secondary, LN_UIActionState::Triggered, user, nullptr, button_filter))
    {
      CancelPointer(phase, user, true);
      ConsumeBindingPresses(LN_UIAction::Secondary, user, *m_input, button_filter, true);
      right_click_used = true;
    }
    else if (carried) {
      /* Moved (or scrolled) the mouse: the mouse holds it now, as after a Grab with the mouse
       * over it. It would float over a selection the mouse hides, out of the mouse's reach. Only
       * while the UI has the mouse (not Mouse Look's turning) and the player is not on a gamepad
       * (a bump of the desk does not take a pad's carry). */
      const LN_MouseSnapshot *mouse = m_input != nullptr ? &m_input->GetMouse() : nullptr;
      const bool mouse_used = mouse != nullptr && !virtual_pointer && !aim &&
                              UIUsesPlayerInput(user) && !LN_InputMap::Get().LastUsedGamepad() &&
                              (mouse->wheel_delta != 0 ||
                               std::abs(mouse->delta_x) + std::abs(mouse->delta_y) > 1);
      if (m_input != nullptr && primary_state(LN_UIActionState::Triggered)) {
        CancelPointer(phase, user, true);
      }
      else if (!mouse_used || !MoveCarryToMouse(user, held->second)) {
        carrying = true;
      }
    }
  }
  std::vector<const Hit *> hits;
  for (const Hit &hit : all_hits) {
    if (hit.registration->user == user && MenuIndexPassesFloor(hit.menu_index, user)) {
      hits.push_back(&hit);
    }
  }
  std::sort(hits.begin(), hits.end(), [](const Hit *a, const Hit *b) {
    if (a->menu_index != b->menu_index) {
      return a->menu_index > b->menu_index;
    }
    if (a->domain_rank != b->domain_rank) {
      return a->domain_rank > b->domain_rank;
    }
    if (a->registration->priority != b->registration->priority) {
      return a->registration->priority > b->registration->priority;
    }
    if (a->fraction != b->fraction) {
      return a->fraction < b->fraction;
    }
    return a->sequence < b->sequence;
  });

  /* Capture and hover are independent during a drag. The capturer continues receiving the drag,
   * while controls underneath it remain eligible as drop targets. In particular, the captured
   * source's ordinary Block policy must not make every object behind it unobservable. */
  ListenerKey dragging_capture;
  bool has_dragging_capture = false;
  const auto existing_pointer = m_pointerStates[phase].find(user);
  if (existing_pointer != m_pointerStates[phase].end() && existing_pointer->second.has_capture &&
      !existing_pointer->second.carried)
  {
    const auto persistent = m_listenerStates[phase].find(
        existing_pointer->second.captured_listener);
    const auto registration = std::find_if(
        m_registrations.begin(), m_registrations.end(), [&](const Registration &item) {
          return item.listener == existing_pointer->second.captured_listener &&
                 item.object ==
                     m_manager.ResolveObjectHandle(existing_pointer->second.captured_object);
        });
    /* Capture alone does not change propagation: a blocking drag source must still prevent a
     * tap from pressing controls behind it. It stops occluding only after the drag threshold has
     * actually been crossed. The threshold-crossing tick uses a separate drop-candidate pass
     * below, so first-tick drop enter remains immediate without leaking ordinary press/clicks. */
    if (persistent != m_listenerStates[phase].end() && persistent->second.dragging &&
        existing_pointer->second.drag_active && registration != m_registrations.end() &&
        registration->role == LN_UIInteractableRole::DragSource)
    {
      dragging_capture = existing_pointer->second.captured_listener;
      has_dragging_capture = true;
    }
  }

  /* Where the pointer's buttons and wheel are the UI's, by the same rules as `blocked` below
   * (a disabled element with Block too): until the next frame, input that reaches Logic + Physics
   * steps first is judged against them where its pointer is (ConsumeInputBeforeResolve). A
   * pointer cursor's buttons are always its own while it is on. */
  for (const Registration &registration : m_registrations) {
    if (virtual_pointer || registration.user != user ||
        registration.propagation != LN_UIPropagation::Block ||
        (has_dragging_capture && registration.listener == dragging_capture))
    {
      continue;
    }
    const bool counts = registration.enabled ?
                            SessionAllowsInteraction(registration.object) &&
                                MenuIndexPassesFloor(
                                    MenuStackIndexForObject(registration.object), user) :
                            registration.listener.element != nullptr &&
                                ObjectPassesModalRouting(registration.object, user);
    if (counts) {
      m_blockSurfaces.push_back(MakePointerSurface(registration));
    }
  }

  std::vector<const Hit *> recipients;
  bool blocked = false;
  for (const Hit *hit : hits) {
    recipients.push_back(hit);
    const bool is_dragging_source = has_dragging_capture &&
                                    hit->registration->listener == dragging_capture;
    if (hit->registration->propagation == LN_UIPropagation::Block && !is_dragging_source) {
      blocked = true;
      break;
    }
  }
  /* The highlight under the mouse: the topmost UI Element, not the one being dragged or carried
   * (that one follows the pointer or the focus), nor a background (a panel with no Action that
   * takes no focus). A control held pressed (a Slider being dragged) keeps it. */
  KX_GameObject *carried_object =
      carrying && existing_pointer != m_pointerStates[phase].end() ?
          m_manager.ResolveObjectHandle(existing_pointer->second.captured_object) :
          nullptr;
  KX_GameObject *held_control =
      existing_pointer != m_pointerStates[phase].end() && existing_pointer->second.has_capture &&
              !existing_pointer->second.drag_active && !carrying ?
          m_manager.ResolveObjectHandle(existing_pointer->second.captured_object) :
          nullptr;
  if (const ElementRecord *held = held_control != nullptr ? FindElementRecord(held_control) :
                                                            nullptr;
      held != nullptr && !IsBackgroundElement(held->settings))
  {
    m_pointerHovers[size_t(user)] = held->handle;
  }
  else {
    for (const Hit *hit : recipients) {
      const Registration &registration = *hit->registration;
      if (registration.listener.element == nullptr ||
          (has_dragging_capture && registration.listener == dragging_capture) ||
          registration.object == carried_object)
      {
        continue;
      }
      const ElementRecord *record = FindElementRecord(registration.object);
      if (record != nullptr && IsBackgroundElement(record->settings)) {
        break;
      }
      m_pointerHovers[size_t(user)] = m_manager.MakeObjectHandle(
          registration.object, registration.object->GetName());
      break;
    }
  }
  /* A click there belongs to the UI, not to game logic (a gun must not fire through a HUD
   * button), and so do the other mouse buttons and the wheel (a camera must not zoom under a
   * menu). Pass Through elements let them reach the game; a full-screen backdrop element makes a
   * menu take them all. */
  /* What a click picked up owns the pointer: the next click puts it down, also over the game. */
  const bool picked_up = existing_pointer != m_pointerStates[phase].end() &&
                         existing_pointer->second.picked_up;
  if (!virtual_pointer) {
    const bool disabled_block = !blocked && !picked_up && m_disabledBlocker.IsValid();
    m_pointerBlockedByUI = blocked || picked_up || disabled_block;
    m_pointerOverBlockingUI = blocked || m_disabledBlocker.IsValid();
  }

  std::unordered_set<ListenerKey, ListenerKeyHash> hovered;
  for (const Hit *hit : recipients) {
    hovered.insert(hit->registration->listener);
    InteractionState &state = m_currentStates[hit->registration->listener];
    ListenerPersistentState &persistent = m_listenerStates[phase][hit->registration->listener];
    const LN_ObjectHandle hit_object = m_manager.MakeObjectHandle(
        hit->registration->object, hit->registration->object->GetName());
    const bool target_changed = persistent.over &&
                                !SameObjectHandle(persistent.object, hit_object);
    state.valid = true;
    state.over = true;
    state.entered = state.entered || !persistent.over || target_changed;
    if (target_changed) {
      state.exited = true;
    }
    state.object = hit_object;
    if (hit->registration->role == LN_UIInteractableRole::Control) {
      state.target_object = hit_object;
    }
    state.point = hit->point;
    state.normal = hit->normal;
    persistent.object = hit_object;
    persistent.over = true;
  }
  for (const auto &declaration : m_listenerDeclarations) {
    if (declaration.second.user != user) {
      continue;
    }
    ListenerPersistentState &persistent = m_listenerStates[phase][declaration.first];
    if (!hovered.contains(declaration.first)) {
      InteractionState &state = m_currentStates[declaration.first];
      if (persistent.over) {
        state.exited = true;
      }
      persistent.over = false;
    }
  }

  if (m_input == nullptr) {
    return;
  }
  bool pressed = primary_state(LN_UIActionState::Triggered);
  bool released = primary_state(LN_UIActionState::Released);
  const bool active = primary_state(LN_UIActionState::Active);
  MT_Vector3 pointer_position;
  if (virtual_pointer) {
    pointer_position = m_playerPointers[size_t(user)].position;
  }
  else {
    const LN_MouseSnapshot &mouse = m_input->GetMouse();
    pointer_position = MT_Vector3(float(mouse.x), float(mouse.y), 0.0f);
    /* Using the mouse hides the keyboard/gamepad focus look until a key or button is used again.
     * A pixel of jitter does not count. */
    if (pressed || mouse.wheel_delta != 0 ||
        std::abs(mouse.delta_x) + std::abs(mouse.delta_y) > 1)
    {
      m_directedUsers.erase(user);
      /* For the keys, only once the place they work in settled: not Mouse Look's movement, nor
       * the pointer report a menu that shows the cursor can bring. */
      if (user == 0 && m_keysPlace != 0 && m_keysPlaceAge >= kMouseForKeysSettle) {
        m_mouseUsedForKeysIn = m_keysPlace;
      }
    }
  }
  /* Secondary's mouse buttons (right click) click what the pointer is over; a pointer cursor's
   * Secondary keys and gamepad controls do. */
  const bool secondary = !right_click_used && !(virtual_pointer && typing) && !aim &&
                         ActionBindingState(LN_UIAction::Secondary,
                                            LN_UIActionState::Triggered,
                                            user,
                                            nullptr,
                                            button_filter);
  if (!recipients.empty() && secondary) {
    for (const Hit *hit : recipients) {
      InteractionState &state = m_currentStates[hit->registration->listener];
      state.valid = true;
      state.secondary_clicked = true;
    }
    if (m_pointerBlockedByUI && !virtual_pointer) {
      ConsumeBindingPresses(LN_UIAction::Secondary, user, *m_input, BindingFilter::Mouse, true);
    }
  }
  /* A pointer cursor's buttons are its own while it is on: game logic does not see them. View
   * Center's press is the UI's only while it aims at a UI Element with Block (or carries what a
   * click picked up); else the game gets it (Use opens a door). */
  if (aim) {
    PlayerPointer &aim_pointer = m_playerPointers[size_t(user)];
    aim_pointer.aim_blocked = blocked || picked_up || aim_pointer.aim_disabled_block;
    if (!recipients.empty()) {
      aim_pointer.point = recipients.front()->point;
    }
    if (aim_pointer.aim_blocked && !typing) {
      ConsumePointerButton(user, *m_input);
    }
  }
  else if (virtual_pointer && !typing) {
    ConsumePointerButton(user, *m_input);
    ConsumeBindingPresses(LN_UIAction::Secondary, user, *m_input, BindingFilter::NotMouse, true);
  }
  if (carrying) {
    return;
  }
  PointerState &pointer = m_pointerStates[phase][user];
  /* What the mouse carries since a click or Grab picked it up: Grab puts it down where the
   * pointer is, as the click does (and picks nothing up again in ResolveCarryForUser). */
  if (!virtual_pointer && !typing && pointer.has_capture && pointer.picked_up &&
      DirectedInputAllowed(user) &&
      ActionBindingState(
          LN_UIAction::Grab, LN_UIActionState::Triggered, user, nullptr, BindingFilter::NotMouse))
  {
    pressed = true;
    released = true;
    ConsumeBindingPresses(LN_UIAction::Grab, user, *m_input, BindingFilter::NotMouse);
    m_pointerGrabUsed |= UserBit(user);
  }
  auto cancel_pointer_state = [&]() {
    if (pointer.drag_active) {
      InteractionState &source = m_currentStates[pointer.captured_listener];
      source.valid = true;
      source.drag_cancelled = true;
      source.drag_ended = true;
      source.dragging = false;
      source.captured = false;
      source.source_object = pointer.captured_object;
      source.drag_amount = pointer.drag_amount;
      if (pointer.has_drop_target) {
        if (!pointer.drop_refused) {
          source.target_object = pointer.drop_object;
        }
        InteractionState &target = m_currentStates[pointer.drop_listener];
        target.valid = true;
        target.drop_exited = true;
        target.drop_over = false;
        target.drop_refused = false;
        target.source_object = pointer.captured_object;
        target.drag_amount = pointer.drag_amount;
        target.target_object = pointer.drop_object;
      }
    }
    pointer = PointerState();
  };

  /* A fixed-step tree can stop while a menu or the whole game is paused. Input capture continues
   * during that interval, but a release is intentionally not replayed as a stale click when the
   * fixed simulation resumes. Reconcile the persistent interaction state with the newest steady
   * device state so the old press cannot leave capture or pass-through recipients stuck forever.
   */
  bool has_pending_press = pointer.has_capture;
  for (const auto &declaration : m_listenerDeclarations) {
    if (declaration.second.user != user) {
      continue;
    }
    const auto persistent = m_listenerStates[phase].find(declaration.first);
    has_pending_press = has_pending_press || (persistent != m_listenerStates[phase].end() &&
                                              persistent->second.pressed_inside);
  }
  if (has_pending_press && !pressed && !released && !active && !pointer.picked_up) {
    cancel_pointer_state();
    for (const auto &declaration : m_listenerDeclarations) {
      if (declaration.second.user != user) {
        continue;
      }
      const auto persistent = m_listenerStates[phase].find(declaration.first);
      if (persistent != m_listenerStates[phase].end()) {
        persistent->second.pressed_inside = false;
        persistent->second.dragging = false;
      }
    }
  }

  const Registration *captured_registration = nullptr;
  /* A press meant to put down what a click picked up is not a press of anything else, also when
   * the pick-up ends right now. */
  const bool put_down_press = pressed && pointer.picked_up;
  if (pointer.has_capture) {
    const auto captured = std::find_if(
        m_registrations.begin(), m_registrations.end(), [&](const Registration &item) {
          return item.listener == pointer.captured_listener &&
                 item.object == m_manager.ResolveObjectHandle(pointer.captured_object);
        });
    const bool capture_is_valid = captured != m_registrations.end() && captured->user == user &&
                                  captured->enabled &&
                                  m_manager.ResolveObjectHandle(pointer.captured_object) ==
                                      captured->object &&
                                  pointer.captured_camera == captured->camera &&
                                  ObjectPassesModalRouting(captured->object, user);
    if (!capture_is_valid) {
      /* Disabling or retargeting a captured listener cancels the whole press sequence, including
       * pass-through recipients. Object, camera, or modal-domain changes also cancel capture:
       * silently transferring the same physical press to a different target or projection would
       * produce jumps and ghost clicks. */
      const auto captured_state = m_listenerStates[phase].find(pointer.captured_listener);
      if (captured_state != m_listenerStates[phase].end()) {
        captured_state->second.pressed_inside = false;
        captured_state->second.dragging = false;
      }
      for (const auto &declaration : m_listenerDeclarations) {
        if (declaration.second.user != user) {
          continue;
        }
        const auto persistent = m_listenerStates[phase].find(declaration.first);
        if (persistent != m_listenerStates[phase].end()) {
          persistent->second.pressed_inside = false;
          persistent->second.dragging = false;
        }
      }
      cancel_pointer_state();
    }
    else {
      captured_registration = &*captured;
    }
  }

  if (pressed && !put_down_press && captured_registration == nullptr && !recipients.empty()) {
    for (const Hit *hit : recipients) {
      const Registration &registration = *hit->registration;
      InteractionState &state = m_currentStates[registration.listener];
      ListenerPersistentState &persistent = m_listenerStates[phase][registration.listener];
      const LN_ObjectHandle target_object = m_manager.MakeObjectHandle(
          registration.object, registration.object->GetName());
      state.pressed = true;
      state.object = target_object;
      if (registration.role == LN_UIInteractableRole::Control) {
        state.target_object = target_object;
      }
      persistent.pressed_inside = true;
      persistent.dragging = false;
      if (registration.focus_on_press && registration.focusable) {
        m_focusByUser[user] = m_manager.MakeObjectHandle(registration.object,
                                                         registration.object->GetName());
      }
      if (registration.capture_on_press && !pointer.has_capture &&
          !HeldByOtherUser(registration.object, user))
      {
        pointer.has_capture = true;
        pointer.captured_listener = registration.listener;
        pointer.captured_object = m_manager.MakeObjectHandle(registration.object,
                                                             registration.object->GetName());
        pointer.captured_camera = registration.camera;
        pointer.press_position = pointer_position;
        pointer.capture_point = hit->point;
        pointer.virtual_pointer = virtual_pointer;
        if (virtual_pointer) {
          /* A cursor's ray keeps its direction: the press stays at its depth along it. */
          MT_Vector3 along = registration.ray_to - registration.ray_from;
          if (along.length2() > kScaleEpsilon) {
            along.normalize();
            pointer.capture_depth = along.dot(hit->point - registration.ray_from);
          }
        }
        else if (MT_Vector3 camera_forward =
                     -registration.camera->NodeGetWorldOrientation().getColumn(2);
                 IsFinite(camera_forward) && camera_forward.length2() > kScaleEpsilon)
        {
          camera_forward.normalize();
          pointer.capture_depth = camera_forward.dot(hit->point -
                                                     registration.camera->NodeGetWorldPosition());
        }
        pointer.capture_normal = hit->normal;
        captured_registration = &registration;
        state.captured = true;
      }
    }
  }

  if (captured_registration != nullptr) {
    InteractionState &state = m_currentStates[captured_registration->listener];
    ListenerPersistentState &persistent = m_listenerStates[phase][captured_registration->listener];
    state.valid = true;
    state.captured = true;
    state.object = pointer.captured_object;
    if (captured_registration->role == LN_UIInteractableRole::Control) {
      state.target_object = pointer.captured_object;
    }
    /* Preserve useful coordinates after the pointer leaves the source or another blocking
     * control becomes topmost. Intersect the current pointer ray with the camera-facing plane at
     * the original press depth. A simple ray fraction would curve toward the camera at wide
     * perspective angles; the plane is stable for both perspective and orthographic cameras. */
    MT_Vector3 camera_forward =
        -captured_registration->camera->NodeGetWorldOrientation().getColumn(2);
    state.point = pointer.capture_point;
    if (pointer.virtual_pointer) {
      MT_Vector3 along = captured_registration->ray_to - captured_registration->ray_from;
      if (along.length2() > kScaleEpsilon) {
        along.normalize();
        state.point = captured_registration->ray_from + along * pointer.capture_depth;
      }
    }
    else if (IsFinite(camera_forward) && camera_forward.length2() > kScaleEpsilon) {
      camera_forward.normalize();
      const MT_Vector3 ray = captured_registration->ray_to - captured_registration->ray_from;
      const float plane_denominator = camera_forward.dot(ray);
      if (std::abs(plane_denominator) > kScaleEpsilon) {
        const MT_Vector3 plane_point = captured_registration->camera->NodeGetWorldPosition() +
                                       camera_forward * pointer.capture_depth;
        const float ray_fraction = camera_forward.dot(plane_point -
                                                      captured_registration->ray_from) /
                                   plane_denominator;
        const MT_Vector3 projected_point = captured_registration->ray_from + ray * ray_fraction;
        if (IsFinite(projected_point)) {
          state.point = projected_point;
        }
      }
    }
    state.normal = pointer.capture_normal;
    /* Only a drag source can be dragged. A button keeps its press when the pointer moves, so it
     * still clicks when released over it. */
    const float threshold = std::max(0.0f, captured_registration->drag_threshold);
    const MT_Vector3 displacement = pointer_position - pointer.press_position;
    /* Click to Pick Up: a click on it picks it up, and the next click puts it down. With it,
     * a drag needs some movement even at Drag Threshold 0, so a click stays a click. */
    const bool moved = displacement.length2() >= threshold * threshold &&
                       (!captured_registration->click_pick_up || displacement.length2() > 0.0f);
    const bool pick_up = released && !persistent.dragging && !moved &&
                         captured_registration->click_pick_up &&
                         captured_registration->role == LN_UIInteractableRole::DragSource &&
                         persistent.pressed_inside && state.over;
    if (!persistent.dragging &&
        captured_registration->role == LN_UIInteractableRole::DragSource && (moved || pick_up))
    {
      persistent.dragging = true;
      state.drag_started = true;
      state.drag_origin = pointer.capture_point;
      pointer.drag_active = true;
      pointer.drop_group = captured_registration->drop_group;
      RememberDragItem(pointer, captured_registration->object, user);
    }
    state.dragging = persistent.dragging;
    /* The drop target under the pointer that takes what is dragged, or the one that refuses it
     * by its Accept Categories (letting go there puts it back). */
    const Hit *drop_hit = nullptr;
    const Hit *refused_hit = nullptr;
    if (pointer.drag_active) {
      state.source_object = pointer.captured_object;
      state.drag_amount = pointer.drag_amount;
      /* The ordinary recipient list was resolved before this tick could cross the drag
       * threshold. Re-evaluate only drop routing while skipping the captured source, preserving
       * every other control's Block policy. This makes a drop target available immediately but
       * never sends the source's initial press/click through to objects underneath it. */
      for (const Hit *hit : hits) {
        if (hit->registration->listener == captured_registration->listener) {
          continue;
        }
        const Registration &target_registration = *hit->registration;
        const bool groups_match = pointer.drop_group.empty() ||
                                  target_registration.drop_group.empty() ||
                                  pointer.drop_group == target_registration.drop_group;
        if (target_registration.drop_target && groups_match) {
          (AcceptsDrop(*captured_registration, target_registration, pointer) ? drop_hit :
                                                                                refused_hit) = hit;
          break;
        }
        if (target_registration.propagation == LN_UIPropagation::Block) {
          break;
        }
      }

      const Hit *over_hit = drop_hit != nullptr ? drop_hit : refused_hit;
      const bool target_changed = pointer.has_drop_target &&
                                  (over_hit == nullptr ||
                                   over_hit->registration->listener != pointer.drop_listener);
      if (target_changed) {
        InteractionState &old_target = m_currentStates[pointer.drop_listener];
        old_target.valid = true;
        old_target.drop_exited = true;
        old_target.source_object = pointer.captured_object;
        old_target.drag_amount = pointer.drag_amount;
        old_target.target_object = pointer.drop_object;
        pointer.has_drop_target = false;
        pointer.drop_refused = false;
        pointer.drop_listener = ListenerKey();
        pointer.drop_object = LN_ObjectHandle();
      }
      if (over_hit != nullptr) {
        InteractionState &target = m_currentStates[over_hit->registration->listener];
        const bool newly_entered = !pointer.has_drop_target;
        pointer.has_drop_target = true;
        pointer.drop_refused = over_hit == refused_hit;
        pointer.drop_listener = over_hit->registration->listener;
        pointer.drop_object = m_manager.MakeObjectHandle(
            over_hit->registration->object, over_hit->registration->object->GetName());
        target.valid = true;
        target.drop_entered = newly_entered;
        target.drop_over = true;
        target.drop_refused = pointer.drop_refused;
        target.point = over_hit->point;
        target.normal = over_hit->normal;
        target.source_object = pointer.captured_object;
        target.drag_amount = pointer.drag_amount;
        target.target_object = pointer.drop_object;
        if (drop_hit != nullptr) {
          state.target_object = pointer.drop_object;
        }
      }

      /* Back puts it back. Player 0's Back is shared with the menus; other players use their
       * own (a pointer cursor's drag). */
      if (GetActionState(
              LN_UIAction::Cancel, LN_UIActionState::Triggered, user, GetActiveInputContext(user)) &&
          (user != 0 || TryConsumeCancelToken()))
      {
        if (user != 0) {
          ConsumeBindingPresses(LN_UIAction::Cancel, user, *m_input, BindingFilter::NotMouse);
          m_playerBackUsed |= uint8_t(1u << user);
        }
        cancel_pointer_state();
        persistent.pressed_inside = false;
        persistent.dragging = false;
        return;
      }
    }
    /* Pass-through recipients can receive the same press before one listener captures the
     * pointer. Capture owns drag delivery, but every pressed recipient must still receive one
     * release and have its persistent press state cleared. Suppress all clicks once the pointer
     * became a drag so an underlying button cannot activate at the end of a drag operation. */
    auto release_other_recipients = [&](const bool pointer_was_dragging) {
      for (const auto &declaration : m_listenerDeclarations) {
        if (declaration.second.user != user ||
            declaration.first == captured_registration->listener)
        {
          continue;
        }
        ListenerPersistentState &other_persistent = m_listenerStates[phase][declaration.first];
        if (!other_persistent.pressed_inside) {
          continue;
        }
        InteractionState &other_state = m_currentStates[declaration.first];
        other_state.valid = true;
        other_state.released = true;
        other_state.clicked = other_state.over && !pointer_was_dragging;
        other_persistent.pressed_inside = false;
        other_persistent.dragging = false;
      }
    };
    if (pick_up) {
      /* The click ends without clicking: the object stays picked up until the next click. */
      pointer.picked_up = true;
      state.released = true;
      persistent.pressed_inside = false;
      release_other_recipients(true);
    }
    else if (released) {
      const bool pointer_was_dragging = persistent.dragging;
      state.released = !pointer.picked_up;
      state.clicked = persistent.pressed_inside && state.over && !pointer_was_dragging;
      state.drag_ended = pointer_was_dragging;
      if (pointer.drag_active) {
        state.source_object = pointer.captured_object;
        state.drag_amount = pointer.drag_amount;
        if (drop_hit != nullptr) {
          state.drop_accepted = true;
          state.target_object = pointer.drop_object;
          state.drop_point = drop_hit->point;
          state.drop_normal = drop_hit->normal;
          InteractionState &target = m_currentStates[drop_hit->registration->listener];
          target.valid = true;
          target.dropped = true;
          target.drop_over = true;
          target.source_object = pointer.captured_object;
          target.drag_amount = pointer.drag_amount;
          target.target_object = pointer.drop_object;
          target.point = drop_hit->point;
          target.normal = drop_hit->normal;
        }
        else if (refused_hit != nullptr) {
          /* The drop target under it refuses the item: it goes back, like a cancel. */
          state.drag_cancelled = true;
          InteractionState &target = m_currentStates[refused_hit->registration->listener];
          target.valid = true;
          target.drop_exited = true;
          target.drop_over = false;
          target.drop_refused = false;
          target.source_object = pointer.captured_object;
          target.drag_amount = pointer.drag_amount;
          target.target_object = pointer.drop_object;
        }
        else {
          state.drop_rejected = true;
        }
      }
      state.dragging = false;
      state.captured = false;
      persistent.pressed_inside = false;
      persistent.dragging = false;

      release_other_recipients(pointer_was_dragging);
      pointer = PointerState();
    }
  }
  else if (released) {
    std::unordered_set<ListenerKey, ListenerKeyHash> release_recipients;
    for (const Hit *hit : recipients) {
      release_recipients.insert(hit->registration->listener);
    }
    for (const auto &declaration : m_listenerDeclarations) {
      if (declaration.second.user != user) {
        continue;
      }
      ListenerPersistentState &persistent = m_listenerStates[phase][declaration.first];
      if (!persistent.pressed_inside) {
        continue;
      }
      InteractionState &state = m_currentStates[declaration.first];
      state.valid = true;
      state.released = true;
      state.clicked = release_recipients.contains(declaration.first) && state.over;
      persistent.pressed_inside = false;
      persistent.dragging = false;
    }
  }
}

void LN_UIService::ResolveFocusedClickForUser(const int32_t user,
                                              const bool focus_shown,
                                              const LN_UIAction action)
{
  /* Secondary's mouse buttons click what the pointer is over instead. */
  const BindingFilter filter = action == LN_UIAction::Secondary ? BindingFilter::NotMouse :
                                                                  BindingFilter::All;
  if (!DirectedInputAllowed(user) || m_input == nullptr ||
      !ActionBindingState(action, LN_UIActionState::Triggered, user, nullptr, filter))
  {
    return;
  }

  /* The mouse points at an element while the selection is hidden: the key presses that one (its
   * hover look shows it), not a selection the player cannot see. */
  bool shown = focus_shown;
  if (!shown) {
    if (KX_GameObject *pointed = PointedForKeys(user, true)) {
      m_focusByUser[user] = m_manager.MakeObjectHandle(pointed, pointed->GetName());
      shown = true;
    }
  }
  const Registration *recipient = DirectedRecipient(user,
                                                    GetFocus(user, GetActiveInputContext(user)));
  /* Only a control that can take the press shows the selection or uses the key. */
  if (recipient == nullptr) {
    return;
  }
  /* Selected this frame by Take UI Control: the press is the one that took the UI. */
  if (user >= 0 && user < LN_MAX_GAMEPADS && m_controlFocusFrame[size_t(user)] == m_frameIndex &&
      m_frameIndex != 0)
  {
    return;
  }
  m_directedUsers.insert(user);
  ConsumeBindingPresses(action, user, *m_input, filter);
  if (!shown) {
    /* Never press a button the player cannot see is selected: this press only shows it. */
    return;
  }
  {
    /* A focused control's semantic Accept action is its ordinary click. This matches established
     * UI toolkits and lets one On Click wire support pointer, keyboard, and gamepad activation.
     * Secondary is its secondary click (a right click). */
    InteractionState &state = m_currentStates[recipient->listener];
    state.valid = true;
    (action == LN_UIAction::Secondary ? state.secondary_clicked : state.clicked) = true;
    state.object = m_manager.MakeObjectHandle(recipient->object, recipient->object->GetName());
    if (recipient->role == LN_UIInteractableRole::Control) {
      state.target_object = state.object;
    }
  }
}

const LN_UIService::Registration *LN_UIService::DirectedRecipient(const int32_t user,
                                                                 KX_GameObject *object) const
{
  if (object == nullptr) {
    return nullptr;
  }
  /* Outside their menus, a player who took control of a UI presses only on it (not on a HUD
   * button a click selected meanwhile). */
  if (InputMenuIndex(user) < 0 && HoldsControl(user) && ControllerOf(object) != user) {
    return nullptr;
  }
  const Registration *recipient = nullptr;
  int recipient_menu_index = std::numeric_limits<int>::min();
  for (const Registration &registration : m_registrations) {
    if (registration.user != user || registration.object != object || !registration.enabled ||
        !registration.focusable || !ObjectAcceptsDirectedInput(registration.object, user))
    {
      continue;
    }
    const int menu_index = MenuStackIndexForObject(registration.object);
    if (!MenuIndexPassesFloor(menu_index, user)) {
      continue;
    }
    if (recipient == nullptr || menu_index > recipient_menu_index ||
        (menu_index == recipient_menu_index && registration.priority > recipient->priority))
    {
      recipient = &registration;
      recipient_menu_index = menu_index;
    }
  }
  return recipient;
}

int64_t LN_UIService::PlayerZeroKeysPlace() const
{
  const int index = InputMenuIndex(0);
  if (index >= 0) {
    return int64_t(m_menuStack[size_t(index)]);
  }
  return UIUsesPlayerInput(0) ? -1 : 0;
}

KX_GameObject *LN_UIService::PointedForKeys(const int32_t user, const bool this_frame) const
{
  /* Only player 0 has a mouse; a pointer cursor's own keys are its buttons. A mouse that only
   * rests over an element (a keyboard player's, or where a menu opened under it) points at
   * nothing for the keys: it must have been used in the menu the player is in. */
  if (user != 0 || m_mouseUsedForKeysIn == 0 || m_mouseUsedForKeysIn != PlayerZeroKeysPlace() ||
      FocusShown(user) || PointerActive(user))
  {
    return nullptr;
  }
  KX_GameObject *object = m_manager.ResolveObjectHandle(
      this_frame ? m_pointerHovers[size_t(user)] : m_previousPointerHovers[size_t(user)]);
  return DirectedRecipient(user, object) != nullptr ? object : nullptr;
}

void LN_UIService::StartPointerPickUp(const int32_t user, const Registration &source)
{
  const size_t phase = PhaseIndex(m_event);
  float fraction = 1.0f;
  MT_Vector3 point = source.world_position;
  MT_Vector3 normal(0.0f, 0.0f, 1.0f);
  if (!RayIntersectsRegistration(source, fraction, point, normal)) {
    point = source.world_position;
  }
  /* What a press and its release on a Click to Pick Up element leave behind (ResolvePointerForUser
   * then drags it with the button up and puts it down at the next click). */
  PointerState &pointer = m_pointerStates[phase][user];
  pointer = PointerState();
  pointer.has_capture = true;
  pointer.captured_listener = source.listener;
  pointer.captured_object = m_manager.MakeObjectHandle(source.object, source.object->GetName());
  pointer.captured_camera = source.camera;
  if (m_input != nullptr) {
    const LN_MouseSnapshot &mouse = m_input->GetMouse();
    pointer.press_position = MT_Vector3(float(mouse.x), float(mouse.y), 0.0f);
  }
  pointer.capture_point = point;
  pointer.capture_normal = normal;
  if (source.camera != nullptr) {
    MT_Vector3 camera_forward = -source.camera->NodeGetWorldOrientation().getColumn(2);
    if (IsFinite(camera_forward) && camera_forward.length2() > kScaleEpsilon) {
      camera_forward.normalize();
      pointer.capture_depth = camera_forward.dot(point - source.camera->NodeGetWorldPosition());
    }
  }
  pointer.drag_active = true;
  pointer.picked_up = true;
  pointer.drop_group = source.drop_group;
  RememberDragItem(pointer, source.object, user);
  ListenerPersistentState &persistent = m_listenerStates[phase][source.listener];
  persistent.pressed_inside = false;
  persistent.dragging = true;
  InteractionState &state = m_currentStates[source.listener];
  state.valid = true;
  state.captured = true;
  state.dragging = true;
  state.drag_started = true;
  state.object = pointer.captured_object;
  state.source_object = pointer.captured_object;
  state.drag_amount = pointer.drag_amount;
  state.drag_origin = point;
  state.point = point;
  state.normal = normal;
}

bool LN_UIService::MoveCarryToMouse(const int32_t user, PointerState &pointer)
{
  KX_GameObject *carried = m_manager.ResolveObjectHandle(pointer.captured_object);
  const auto source = std::find_if(
      m_registrations.begin(), m_registrations.end(), [&](const Registration &item) {
        return item.listener == pointer.captured_listener && item.object == carried;
      });
  if (carried == nullptr || source == m_registrations.end() || source->user != user ||
      source->camera == nullptr || source->camera != pointer.captured_camera || m_input == nullptr)
  {
    return false;
  }
  /* Held under the pointer at the depth of where it was picked up, as a click's pick-up is at
   * its press depth. Its drag offset stays the one the carry started with. */
  const LN_MouseSnapshot &mouse = m_input->GetMouse();
  const MT_Matrix3x3 &axes = source->camera->NodeGetWorldOrientation();
  pointer.carried = false;
  pointer.picked_up = true;
  pointer.virtual_pointer = false;
  pointer.press_position = MT_Vector3(float(mouse.x), float(mouse.y), 0.0f);
  pointer.capture_point = pointer.carry_home_world;
  pointer.capture_normal = axes.getColumn(2);
  MT_Vector3 camera_forward = -axes.getColumn(2);
  if (IsFinite(camera_forward) && camera_forward.length2() > kScaleEpsilon) {
    camera_forward.normalize();
    pointer.capture_depth = camera_forward.dot(pointer.carry_home_world -
                                               source->camera->NodeGetWorldPosition());
  }
  return true;
}

void LN_UIService::MovePickUpToKeys(const int32_t user, PointerState &pointer)
{
  KX_GameObject *object = m_manager.ResolveObjectHandle(pointer.captured_object);
  const auto source = std::find_if(
      m_registrations.begin(), m_registrations.end(), [&](const Registration &item) {
        return item.listener == pointer.captured_listener && item.object == object;
      });
  if (object == nullptr || source == m_registrations.end() || source->user != user) {
    return;
  }
  /* Its home is where it was picked up (its cell), not where the mouse holds it: navigation
   * from it starts there and it goes back there. */
  ElementRecord *record = FindElementRecord(object);
  const bool following = record != nullptr && record->following;
  pointer.carry_home_world = following ? record->drag_home_world : source->world_position;
  pointer.carry_home_screen = source->screen_position;
  if (source->camera != nullptr) {
    LN_ProjectWorldToScreen(source->camera, pointer.carry_home_world, pointer.carry_home_screen);
  }
  /* Held by the mouse, it kept where the pointer grabbed it; carried, it floats exactly where the
   * carry puts it, as a carry started by the keys does. */
  if (following) {
    record->drag_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  pointer.picked_up = false;
  pointer.carried = true;
}

void LN_UIService::HandPickUpToKeys(const int32_t user)
{
  const auto held = m_pointerStates[PhaseIndex(m_event)].find(user);
  if (held != m_pointerStates[PhaseIndex(m_event)].end() && held->second.has_capture &&
      held->second.picked_up && !held->second.virtual_pointer && !VirtualPointerActive(user))
  {
    MovePickUpToKeys(user, held->second);
  }
}

KX_GameObject *LN_UIService::CarriedObject(const int32_t user) const
{
  const auto &pointers = m_pointerStates[PhaseIndex(m_event)];
  const auto pointer = pointers.find(user);
  return pointer != pointers.end() && pointer->second.carried ?
             m_manager.ResolveObjectHandle(pointer->second.captured_object) :
             nullptr;
}

bool LN_UIService::HeldByOtherUser(const KX_GameObject *object, const int32_t user) const
{
  for (const auto &[other, pointer] : m_pointerStates[PhaseIndex(m_event)]) {
    if (other != user && pointer.has_capture &&
        m_manager.ResolveObjectHandle(pointer.captured_object) == object)
    {
      return true;
    }
  }
  return false;
}

bool LN_UIService::ResolveCarryForUser(const int32_t user, const bool focus_shown)
{
  const size_t phase = PhaseIndex(m_event);
  /* Without input (a rebind waits) a carried element still floats. */
  const auto pressed = [&](const LN_UIAction action) {
    return m_input != nullptr && DirectedInputAllowed(user) &&
           ActionBindingState(
               action, LN_UIActionState::Triggered, user, nullptr, BindingFilter::NotMouse);
  };
  /* A Grab that put down what the pointer carried this frame picks nothing up again. */
  const bool grab = pressed(LN_UIAction::Grab) && (m_pointerGrabUsed & UserBit(user)) == 0;
  const bool accept = pressed(LN_UIAction::Accept);
  const auto existing = m_pointerStates[phase].find(user);
  KX_GameObject *focused = GetFocus(user, GetActiveInputContext(user));

  if (existing == m_pointerStates[phase].end() || !existing->second.carried) {
    /* A pointer press or drag of this user keeps the pointer. While the mouse holds what a click
     * picked up, the keys that would pick up or click do nothing. */
    if (existing != m_pointerStates[phase].end() && existing->second.has_capture) {
      const bool secondary = pressed(LN_UIAction::Secondary);
      if (!existing->second.picked_up || (!grab && !accept && !secondary)) {
        return false;
      }
      for (const LN_UIAction action : {LN_UIAction::Grab, LN_UIAction::Accept, LN_UIAction::Secondary})
      {
        ConsumeBindingPresses(action, user, *m_input, BindingFilter::NotMouse);
      }
      return true;
    }
    if (!grab && !accept) {
      return false;
    }
    /* With the mouse, Grab (Accept with Click to Pick Up) picks up what the mouse points at into
     * the mouse, not the hidden selection: it follows the pointer, and a click or Grab puts it
     * down where the pointer is (right click or Back puts it back). */
    if (!focus_shown) {
      if (KX_GameObject *pointed = PointedForKeys(user, true)) {
        m_focusByUser[user] = m_manager.MakeObjectHandle(pointed, pointed->GetName());
        const Registration *pointed_source = DirectedRecipient(user, pointed);
        if (pointed_source == nullptr ||
            pointed_source->role != LN_UIInteractableRole::DragSource ||
            (!grab && !pointed_source->click_pick_up))
        {
          /* Accept then presses it (ResolveFocusedClickForUser); Grab has nothing to take. */
          return false;
        }
        if (grab) {
          ConsumeBindingPresses(LN_UIAction::Grab, user, *m_input, BindingFilter::NotMouse);
        }
        if (accept) {
          ConsumeBindingPresses(LN_UIAction::Accept, user, *m_input, BindingFilter::NotMouse);
        }
        if (!HeldByOtherUser(pointed, user)) {
          StartPointerPickUp(user, *pointed_source);
        }
        return true;
      }
    }
    /* Grab picks up any drag source; Accept, like a click, one with Click to Pick Up. */
    const Registration *source = DirectedRecipient(user, focused);
    if (source == nullptr || source->role != LN_UIInteractableRole::DragSource ||
        (!grab && !source->click_pick_up))
    {
      return false;
    }
    m_directedUsers.insert(user);
    if (grab) {
      ConsumeBindingPresses(LN_UIAction::Grab, user, *m_input, BindingFilter::NotMouse);
    }
    if (accept) {
      ConsumeBindingPresses(LN_UIAction::Accept, user, *m_input, BindingFilter::NotMouse);
    }
    /* Never pick up what the player cannot see is selected: this press only shows it. Another
     * player already holding it (an Any Player element): the press does nothing. */
    if (!focus_shown || HeldByOtherUser(source->object, user)) {
      return true;
    }
    PointerState &pointer = m_pointerStates[phase][user];
    pointer = PointerState();
    pointer.has_capture = true;
    pointer.drag_active = true;
    pointer.carried = true;
    pointer.captured_listener = source->listener;
    pointer.captured_object = m_manager.MakeObjectHandle(source->object,
                                                         source->object->GetName());
    pointer.captured_camera = source->camera;
    pointer.drop_group = source->drop_group;
    pointer.carry_home_screen = source->screen_position;
    pointer.carry_home_world = source->world_position;
    RememberDragItem(pointer, source->object, user);
    ListenerPersistentState &persistent = m_listenerStates[phase][source->listener];
    persistent.pressed_inside = false;
    persistent.dragging = true;
    InteractionState &state = m_currentStates[source->listener];
    state.valid = true;
    state.captured = true;
    state.dragging = true;
    state.drag_started = true;
    state.object = pointer.captured_object;
    state.source_object = pointer.captured_object;
    state.drag_amount = pointer.drag_amount;
    state.drag_origin = source->world_position;
    state.point = source->world_position;
    return true;
  }

  PointerState &pointer = existing->second;
  KX_GameObject *carried = m_manager.ResolveObjectHandle(pointer.captured_object);
  const Registration *source = nullptr;
  for (const Registration &registration : m_registrations) {
    if (registration.listener == pointer.captured_listener && registration.object == carried) {
      source = &registration;
      break;
    }
  }
  if (source == nullptr || carried == nullptr || source->user != user || !source->enabled ||
      !ObjectPassesModalRouting(carried, user))
  {
    CancelPointer(phase, user, true);
    /* A put-down press meant for the carried element clicks nothing else. */
    if (!grab && !accept) {
      return false;
    }
    m_directedUsers.insert(user);
    ConsumeBindingPresses(LN_UIAction::Grab, user, *m_input, BindingFilter::NotMouse);
    ConsumeBindingPresses(LN_UIAction::Accept, user, *m_input, BindingFilter::NotMouse);
    return true;
  }
  /* Back puts it back. Player 0's Back is shared with the menus; other players use their own. */
  if (GetActionState(
          LN_UIAction::Cancel, LN_UIActionState::Triggered, user, GetActiveInputContext(user)) &&
      (user != 0 || TryConsumeCancelToken()))
  {
    if (user != 0) {
      ConsumeBindingPresses(LN_UIAction::Cancel, user, *m_input, BindingFilter::NotMouse);
      m_playerBackUsed |= uint8_t(1u << user);
    }
    CancelPointer(phase, user, true);
    return true;
  }
  /* While carrying, Secondary does nothing, and is not the game's either. */
  if (pressed(LN_UIAction::Secondary)) {
    ConsumeBindingPresses(LN_UIAction::Secondary, user, *m_input, BindingFilter::NotMouse);
  }

  /* It floats over the focused element, a little toward the camera so it stays in front. While
   * the focus is still on it, that is over where it was picked up (it has moved since). */
  const Registration *target = focused != carried ? DirectedRecipient(user, focused) : nullptr;
  const MT_Vector3 over = target != nullptr ? target->world_position : pointer.carry_home_world;
  MT_Vector3 toward_camera(0.0f, 0.0f, 0.0f);
  if (source->camera != nullptr) {
    toward_camera = source->camera->NodeGetWorldPosition() - over;
    if (IsFinite(toward_camera) && toward_camera.length2() > kScaleEpsilon) {
      toward_camera.normalize();
    }
    else {
      toward_camera = MT_Vector3(0.0f, 0.0f, 0.0f);
    }
  }
  const MT_Vector3 size = (source->bounds_max - source->bounds_min) * source->world_scale;
  const float extent = std::max({std::abs(size.x()), std::abs(size.y()), std::abs(size.z())});
  const float lift = 0.2f * extent;
  /* Up and to the right on screen, so the element under it still shows. */
  MT_Vector3 aside(0.0f, 0.0f, 0.0f);
  if (source->camera != nullptr) {
    const MT_Matrix3x3 &axes = source->camera->NodeGetWorldOrientation();
    aside = (axes.getColumn(0) + axes.getColumn(1)) * (0.3f * extent);
    if (!IsFinite(aside)) {
      aside = MT_Vector3(0.0f, 0.0f, 0.0f);
    }
  }
  InteractionState &state = m_currentStates[source->listener];
  state.valid = true;
  state.captured = true;
  state.dragging = true;
  state.object = pointer.captured_object;
  state.source_object = pointer.captured_object;
  state.drag_amount = pointer.drag_amount;
  state.point = over + toward_camera * lift + aside;
  state.normal = toward_camera;

  /* A drop target of its group that refuses it by its Accept Categories is still over it (Drop
   * Enter tells), but putting it down there puts it back. */
  const bool over_target = target != nullptr && target->drop_target &&
                           (pointer.drop_group.empty() || target->drop_group.empty() ||
                            pointer.drop_group == target->drop_group);
  const bool accepts = over_target && AcceptsDrop(*source, *target, pointer);
  if (pointer.has_drop_target && (!over_target || target->listener != pointer.drop_listener)) {
    InteractionState &old_target = m_currentStates[pointer.drop_listener];
    old_target.valid = true;
    old_target.drop_exited = true;
    old_target.source_object = pointer.captured_object;
    old_target.drag_amount = pointer.drag_amount;
    old_target.target_object = pointer.drop_object;
    pointer.has_drop_target = false;
    pointer.drop_refused = false;
    pointer.drop_listener = ListenerKey();
    pointer.drop_object = LN_ObjectHandle();
  }
  if (over_target) {
    InteractionState &target_state = m_currentStates[target->listener];
    target_state.drop_entered = !pointer.has_drop_target;
    pointer.has_drop_target = true;
    pointer.drop_refused = !accepts;
    pointer.drop_listener = target->listener;
    pointer.drop_object = m_manager.MakeObjectHandle(target->object, target->object->GetName());
    target_state.valid = true;
    target_state.drop_over = true;
    target_state.drop_refused = !accepts;
    target_state.point = target->world_position;
    target_state.normal = toward_camera;
    target_state.source_object = pointer.captured_object;
    target_state.drag_amount = pointer.drag_amount;
    target_state.target_object = pointer.drop_object;
    if (accepts) {
      state.target_object = pointer.drop_object;
    }
  }

  if (!grab && !accept) {
    return true;
  }
  m_directedUsers.insert(user);
  if (grab) {
    ConsumeBindingPresses(LN_UIAction::Grab, user, *m_input, BindingFilter::NotMouse);
  }
  if (accept) {
    ConsumeBindingPresses(LN_UIAction::Accept, user, *m_input, BindingFilter::NotMouse);
  }
  if (!focus_shown) {
    return true;
  }
  if (!accepts) {
    /* Put down on itself or on something that does not take it (also a slot whose Accept
     * Categories refuse it): it goes back. */
    CancelPointer(phase, user, true);
    return true;
  }
  /* Put down: the same events as a pointer drop. */
  state.dragging = false;
  state.captured = false;
  state.drag_ended = true;
  state.drop_accepted = true;
  state.target_object = pointer.drop_object;
  state.drop_point = target->world_position;
  state.drop_normal = toward_camera;
  InteractionState &target_state = m_currentStates[target->listener];
  target_state.dropped = true;
  if (auto persistent = m_listenerStates[phase].find(pointer.captured_listener);
      persistent != m_listenerStates[phase].end())
  {
    persistent->second.pressed_inside = false;
    persistent->second.dragging = false;
  }
  m_pointerStates[phase].erase(existing);
  return true;
}

void LN_UIService::RememberDragItem(PointerState &pointer,
                                    KX_GameObject *object,
                                    const int32_t user) const
{
  const auto binding = m_slotBindings.find(object);
  pointer.has_drag_item = binding != m_slotBindings.end();
  pointer.drag_amount = 0;
  if (pointer.has_drag_item) {
    pointer.drag_item = binding->second.item;
    pointer.drag_item_index = binding->second.index;
    pointer.drag_amount = SplitAmount(GridOfSlot(object), pointer.drag_item, user);
  }
  pointer.drag_category.clear();
  pointer.drag_category_known = false;
}

int32_t LN_UIService::SplitAmount(const ViewRegistration *view,
                                  const LN_Value &item,
                                  const int32_t user) const
{
  std::string name;
  int32_t count = 0;
  if (view == nullptr || !view->inventory.handle_moves || view->inventory.split_action.empty() ||
      !LN_InventoryEntry(item, name, count) || count < 2)
  {
    return 0;
  }
  const int32_t action = GridAction(view->inventory.split_action, "Split Action");
  if (action < 0 ||
      !ActionIndexBindingState(action, LN_UIActionState::Active, user, nullptr, BindingFilter::All))
  {
    return 0;
  }
  const int32_t amount = view->inventory.split_amount > 0 ? view->inventory.split_amount :
                                                            (count + 1) / 2;
  return amount < count ? amount : 0;
}

int32_t LN_UIService::GridAction(const std::string &name, const char *setting) const
{
  const int32_t action = LN_InputMap::Get().Find(name);
  if (action < 0 && m_gridWarnings.insert(std::string(setting) + "\x1f" + name).second) {
    CM_Warning("Object Grid View: the Game Input Map has no action \"" << name << "\" for "
                                                                        << setting);
  }
  return action;
}

bool LN_UIService::SlotUsedBy(const ElementRecord &record, const int32_t user)
{
  const ViewRegistration *view = GridOfSlot(record.object);
  if (view == nullptr || m_input == nullptr || (view->use_action.empty() && view->use_keys.empty()))
  {
    return false;
  }
  LN_Value item;
  int32_t index = -1;
  if (!GetBoundItem(record.object, item, index) || !item.exists || index < 0 ||
      LN_InventoryIsEmptyPlace(item) || UserDrags(user) ||
      (user == 0 && m_editingText.IsValid()))
  {
    return false;
  }
  const auto pressed = [&](const std::string &name, const char *setting) {
    const int32_t action = GridAction(name, setting);
    if (action < 0 || !ActionIndexBindingState(
                          action, LN_UIActionState::Triggered, user, nullptr, BindingFilter::All))
    {
      return false;
    }
    ConsumeActionIndexPresses(action, user, *m_input, BindingFilter::All);
    return true;
  };
  if (!view->use_action.empty()) {
    const auto highlight = m_highlights.find(user);
    if (highlight != m_highlights.end() &&
        SameObjectHandle(highlight->second.object, record.handle) &&
        pressed(view->use_action, "Use Action"))
    {
      return true;
    }
  }
  /* A hotbar: the n-th key uses the n-th slot, while the player is in no menu but this one. */
  const std::vector<std::string> keys = CommaList(view->use_keys);
  const size_t place = size_t(std::find(view->slots.begin(), view->slots.end(), record.object) -
                              view->slots.begin());
  return place < keys.size() && !keys[place].empty() && UseKeysReach(user, record.menu_index) &&
         pressed(keys[place], "Use Keys");
}

bool LN_UIService::UseKeysReach(const int32_t user, const int menu_index) const
{
  const int input_menu = InputMenuIndex(user);
  return input_menu >= 0 ? input_menu == menu_index : !UIUsesPlayerInput(user);
}

std::string LN_UIService::ItemCategories(const LN_Value &item, const ListenerKey &view) const
{
  /* The grid's own Item Library, else the scene's. */
  const blender::Collection *library = nullptr;
  for (const ViewRegistration &registration : m_viewRegistrations) {
    if (registration.listener == view) {
      library = registration.icon_library;
      break;
    }
  }
  return ItemCategoriesIn(item, library);
}

std::string LN_UIService::ItemCategoriesIn(const LN_Value &item,
                                           const blender::Collection *library) const
{
  if (const LN_Value *own = OwnValue(item, "category")) {
    return NormalizedCategories(OwnValueText(*own));
  }
  /* Named like the slot's icon. An object item: its own category, else by its name. */
  std::string name;
  int32_t count = 0;
  if (item.exists && item.type == LN_ValueType::ObjectRef) {
    KX_GameObject *object = item.runtime_ref.shared_domain != 0 ?
                                m_manager.ResolveSharedObjectRef(item.runtime_ref) :
                                nullptr;
    if (object == nullptr && !item.reference_name.empty()) {
      if (EXP_ListValue<KX_GameObject> *objects = m_scene.GetObjectList()) {
        object = objects->FindValue(item.reference_name);
      }
    }
    if (EXP_Value *category = object != nullptr ? object->GetProperty("category") : nullptr) {
      return NormalizedCategories(category->GetText());
    }
    name = item.reference_name;
  }
  else if (!LN_InventoryShownEntry(item, name, count)) {
    return std::string();
  }
  KX_GameObject *source = FindLibraryItem(library, name);
  EXP_Value *category = source != nullptr ? source->GetProperty("category") : nullptr;
  return category != nullptr ? NormalizedCategories(category->GetText()) : std::string();
}

LN_InventoryRules LN_UIService::ListRules(const ViewRegistration &view)
{
  /* The library the grid shows: its list's rules', else the scene's (CollectUIViews). */
  LN_InventoryRules rules = view.rules;
  if (rules.library == nullptr) {
    rules.library = view.icon_library;
  }
  return rules;
}

LN_InventoryLookup LN_UIService::InventoryLookup() const
{
  LN_InventoryLookup lookup;
  lookup.scene = &m_scene;
  /* Categories as the UI reads them: also of object items, and through the tests' library. */
  lookup.categories = [](const LN_Value &item,
                         const blender::Collection *library,
                         const void *service) {
    return static_cast<const LN_UIService *>(service)->ItemCategoriesIn(item, library);
  };
  lookup.categories_data = this;
  return lookup;
}

std::string LN_UIService::ItemDisplayName(const ViewRegistration &view, const LN_Value &item) const
{
  if (const LN_Value *own = OwnValue(item, "name")) {
    return OwnValueText(*own);
  }
  std::string name;
  int32_t count = 0;
  if (item.exists && item.type == LN_ValueType::ObjectRef) {
    name = item.reference_name;
  }
  else if (!LN_InventoryShownEntry(item, name, count)) {
    return std::string();
  }
  KX_GameObject *source = FindLibraryItem(view.icon_library, name);
  EXP_Value *display = source != nullptr ? source->GetProperty("name") : nullptr;
  return display != nullptr && !display->GetText().empty() ? display->GetText() : name;
}

bool LN_UIService::FilterShows(const ViewRegistration &view,
                               const LN_Value &item,
                               const std::string &categories,
                               const std::string &search) const
{
  std::string entry;
  int32_t count = 0;
  if (item.exists && item.type == LN_ValueType::ObjectRef) {
    entry = item.reference_name;
  }
  else if (!LN_InventoryShownEntry(item, entry, count) || count <= 0) {
    return false;
  }
  if (!categories.empty() && !CategoriesOverlap(categories, ItemCategories(item, view.listener))) {
    return false;
  }
  return search.empty() ||
         LowerCase(ItemDisplayName(view, item)).find(search) != std::string::npos ||
         LowerCase(entry).find(search) != std::string::npos;
}

std::string LN_UIService::DraggedItemCategories(KX_GameObject *object,
                                                const PointerState &pointer) const
{
  if (pointer.has_drag_item) {
    const auto binding = m_slotBindings.find(object);
    return ItemCategories(pointer.drag_item,
                          binding != m_slotBindings.end() ? binding->second.view : ListenerKey());
  }
  if (object == nullptr) {
    return std::string();
  }
  /* A pickup or a card: its own category, else that of the library object its item names. */
  if (EXP_Value *category = object->GetProperty("category")) {
    return NormalizedCategories(category->GetText());
  }
  EXP_Value *item = object->GetProperty("item");
  if (item == nullptr) {
    return std::string();
  }
  LN_Value name;
  name.type = LN_ValueType::String;
  name.exists = true;
  name.string_value = item->GetText();
  return ItemCategories(name, ListenerKey());
}

bool LN_UIService::AcceptsDrop(const Registration &source,
                               const Registration &target,
                               PointerState &pointer) const
{
  if (!target.accept_categories.empty()) {
    if (!pointer.drag_category_known) {
      pointer.drag_category = DraggedItemCategories(
          m_manager.ResolveObjectHandle(pointer.captured_object), pointer);
      pointer.drag_category_known = true;
    }
    if (!CategoriesOverlap(target.accept_categories, pointer.drag_category)) {
      return false;
    }
  }
  /* A grid that moves items puts what comes from another list into its own: that list's
   * Inventory Rules must take it (their Accept Categories). */
  const ViewRegistration *into = target.object != nullptr ? GridOfSlot(target.object) : nullptr;
  const ViewRegistration *from = source.object != nullptr ? GridOfSlot(source.object) : nullptr;
  const bool between_lists = into != nullptr && from != nullptr &&
                             into->inventory.handle_moves && from->inventory.handle_moves &&
                             into->list_object != nullptr && from->list_object != nullptr &&
                             !SameGridList(*from, *into);
  const LN_InventoryLookup lookup = InventoryLookup();
  if (between_lists && pointer.has_drag_item &&
      !lookup.Accepts(ListRules(*into), pointer.drag_item))
  {
    return false;
  }
  /* Dropped on a filled slot, the two items may swap: a source slot that is a Drop Target with
   * Accept Categories must take the target's item too, so a weapon slot never gets an apple back,
   * and so must the source list's rules. Unless it goes into the target's list instead
   * (unequipping into a bag). */
  const auto binding = m_slotBindings.find(target.object);
  if (binding != m_slotBindings.end() && binding->second.item.exists &&
      (!SwapBackAllowed(source.drop_target,
                        source.accept_categories,
                        binding->second.item,
                        binding->second.view) ||
       (between_lists && !lookup.Accepts(ListRules(*from), binding->second.item))))
  {
    return pointer.has_drag_item &&
           DropGoesIntoList(source.object, target.object, pointer.drag_item);
  }
  return true;
}

bool LN_UIService::SwapBackAllowed(const bool drop_target,
                                   const std::string &accept_categories,
                                   const LN_Value &item,
                                   const ListenerKey &view) const
{
  return !drop_target || accept_categories.empty() ||
         CategoriesOverlap(accept_categories, ItemCategories(item, view));
}

bool LN_UIService::DropGoesIntoList(const KX_GameObject *source,
                                    const KX_GameObject *target,
                                    const LN_Value &item) const
{
  const ViewRegistration *from = source != nullptr ? GridOfSlot(source) : nullptr;
  const ViewRegistration *into = target != nullptr ? GridOfSlot(target) : nullptr;
  return from != nullptr && into != nullptr && from->inventory.handle_moves &&
         into->inventory.handle_moves && from->list_object != nullptr &&
         into->list_object != nullptr && !SameGridList(*from, *into) && !HoldsOneSlot(*into) &&
         GridListHasRoom(*into, item);
}

bool LN_UIService::GridListHasRoom(const ViewRegistration &view, const LN_Value &item) const
{
  return LN_InventoryHasRoom(view.items, item, ListRules(view), InventoryLookup());
}

bool LN_UIService::UserDrags(const int32_t user) const
{
  for (const auto &phase : m_pointerStates) {
    const auto pointer = phase.find(user);
    if (pointer != phase.end() && pointer->second.has_capture && pointer->second.drag_active) {
      return true;
    }
  }
  return false;
}

const LN_UIService::PointerState *LN_UIService::FrozenDragOf(KX_GameObject *object) const
{
  for (const auto &phase : m_pointerStates) {
    for (const auto &pointer : phase) {
      if (pointer.second.has_capture && pointer.second.drag_active &&
          pointer.second.has_drag_item &&
          m_manager.ResolveObjectHandle(pointer.second.captured_object) == object)
      {
        return &pointer.second;
      }
    }
  }
  return nullptr;
}

/** The screen rectangle an element covers, from its bounds. Only its centre when its screen
 * position does not come from projecting it through its camera. */
static void NavigationScreenRect(const LN_UIService::Registration &registration,
                                 MT_Vector3 &r_min,
                                 MT_Vector3 &r_max)
{
  r_min = r_max = registration.screen_position;
  MT_Vector3 centre;
  if (!LN_ProjectWorldToScreen(registration.camera, registration.world_position, centre) ||
      (centre - registration.screen_position).length() > 1.0e-4f)
  {
    return;
  }
  for (int corner = 0; corner < 8; ++corner) {
    const MT_Vector3 local(
        (corner & 1) != 0 ? registration.bounds_max.x() : registration.bounds_min.x(),
        (corner & 2) != 0 ? registration.bounds_max.y() : registration.bounds_min.y(),
        (corner & 4) != 0 ? registration.bounds_max.z() : registration.bounds_min.z());
    const MT_Vector3 world = registration.world_position +
                             registration.world_orientation *
                                 MT_Vector3(local.x() * registration.world_scale.x(),
                                            local.y() * registration.world_scale.y(),
                                            local.z() * registration.world_scale.z());
    MT_Vector3 screen;
    if (LN_ProjectWorldToScreen(registration.camera, world, screen) && screen.x() != FLT_MAX) {
      for (int axis = 0; axis < 2; ++axis) {
        r_min[axis] = std::min(r_min[axis], screen[axis]);
        r_max[axis] = std::max(r_max[axis], screen[axis]);
      }
    }
  }
}

void LN_UIService::ResolveNavigationForUser(const int32_t user, const bool focus_shown)
{
  if (m_viewNavigationHandledUsers.contains(user) || !DirectedInputAllowed(user)) {
    return;
  }
  LN_UIAction action = LN_UIAction::Next;
  bool from_press = false;
  if (!NavigationForUser(user, action, from_press)) {
    return;
  }
  /* What the mouse holds (a click or Grab picked it up): the keys carry it from now on. */
  HandPickUpToKeys(user);

  /* The mouse points at an element while the selection is hidden: the keys go on from it, as
   * they would from a shown selection (its hover look shows it). */
  bool shown = focus_shown;
  if (!shown) {
    if (KX_GameObject *pointed = PointedForKeys(user, false)) {
      m_focusByUser[user] = m_manager.MakeObjectHandle(pointed, pointed->GetName());
      shown = true;
    }
  }
  KX_GameObject *focused = GetFocus(user, GetActiveInputContext(user));
  /* A carried element floats over the focus: once the focus left it, it is no place to go. */
  KX_GameObject *carried = CarriedObject(user);
  /* Outside their menus, a player who took control of a UI stays on it. */
  const bool keep_inside = InputMenuIndex(user) < 0 && HoldsControl(user);
  std::vector<const Registration *> candidates;
  std::unordered_map<KX_GameObject *, size_t> candidate_index_by_object;
  for (const Registration &registration : m_registrations) {
    if (registration.user == user && registration.enabled && registration.focusable &&
        registration.navigable && ObjectAcceptsDirectedInput(registration.object, user) &&
        MenuIndexPassesFloor(MenuStackIndexForObject(registration.object), user) &&
        (registration.object != carried || carried == focused) &&
        (!keep_inside || ControllerOf(registration.object) == user))
    {
      const auto existing = candidate_index_by_object.find(registration.object);
      if (existing == candidate_index_by_object.end()) {
        candidate_index_by_object.emplace(registration.object, candidates.size());
        candidates.push_back(&registration);
        continue;
      }
      const Registration *existing_registration = candidates[existing->second];
      const int menu_index = MenuStackIndexForObject(registration.object);
      const int existing_menu_index = MenuStackIndexForObject(existing_registration->object);
      const int domain_rank = registration.interaction_domain == LN_InteractionDomain::Overlay ?
                                  1 :
                                  0;
      const int existing_domain_rank =
          existing_registration->interaction_domain == LN_InteractionDomain::Overlay ? 1 : 0;
      if (menu_index > existing_menu_index ||
          (menu_index == existing_menu_index && domain_rank > existing_domain_rank) ||
          (menu_index == existing_menu_index && domain_rank == existing_domain_rank &&
           registration.priority > existing_registration->priority))
      {
        candidates[existing->second] = &registration;
      }
    }
  }
  /* Navigation from a carried element starts where it was picked up, not where it floats. */
  Registration carried_at_home;
  if (carried != nullptr && carried == focused) {
    const auto entry = candidate_index_by_object.find(carried);
    const auto &pointers = m_pointerStates[PhaseIndex(m_event)];
    const auto pointer = pointers.find(user);
    if (entry != candidate_index_by_object.end() && pointer != pointers.end()) {
      carried_at_home = *candidates[entry->second];
      carried_at_home.screen_position = pointer->second.carry_home_screen;
      candidates[entry->second] = &carried_at_home;
    }
  }
  if (candidates.empty()) {
    if (carried != nullptr) {
      /* Only the carried element is left: keep the focus, it is where it goes down. */
      return;
    }
    if (keep_inside) {
      /* What the player took has nothing to select now (its buttons are off): its first one is
       * selected when there is one again. */
      for (UIControl &control : m_controls) {
        if (control.user == user) {
          control.focus_pending = true;
        }
      }
      return;
    }
    /* Nothing left to navigate: the UI Set UI Focus gave the keyboard to is gone. */
    m_focusByUser.erase(user);
    m_focusOutsideMenuUsers.erase(user);
    return;
  }
  std::stable_sort(
      candidates.begin(), candidates.end(), [](const Registration *a, const Registration *b) {
        return NavigationOrderLess(*a, *b);
      });

  auto current = std::find_if(candidates.begin(),
                              candidates.end(),
                              [focused](const Registration *r) { return r->object == focused; });
  m_directedUsers.insert(user);
  if (from_press) {
    ConsumeActionPress(action, user);
  }
  if (!shown && current != candidates.end()) {
    /* The selection was hidden while the mouse was used: this press only shows it. */
    return;
  }
  /* Left and Right change a focused Slider or Selector instead of leaving it. */
  if (current != candidates.end() && StepFocusedWidget((*current)->object, action, user)) {
    return;
  }
  const Registration *next = action == LN_UIAction::Previous ? candidates.back() :
                                                               candidates.front();
  if (action == LN_UIAction::Next || action == LN_UIAction::Previous) {
    if (current != candidates.end()) {
      const size_t index = size_t(current - candidates.begin());
      const size_t offset = action == LN_UIAction::Next ? 1u : candidates.size() - 1u;
      next = candidates[(index + offset) % candidates.size()];
    }
  }
  else if (current != candidates.end()) {
    const MT_Vector3 origin = (*current)->screen_position;
    float best_score = std::numeric_limits<float>::max();
    bool best_in_line = false;
    next = *current;
    const bool vertical = action == LN_UIAction::NavigateUp || action == LN_UIAction::NavigateDown;
    /* Sideways distance counts between the elements' edges: a checkbox under the left end of a
     * wide row is straight below it. One more sideways than forward is not that way. */
    const int side_axis = vertical ? 0 : 1;
    MT_Vector3 current_min;
    MT_Vector3 current_max;
    NavigationScreenRect(**current, current_min, current_max);
    /* Screen positions are fractions of the width and of the height: compare them in pixels. */
    float x_scale = 1.0f;
    KX_KetsjiEngine *engine = KX_GetActiveEngine();
    RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
    if (canvas != nullptr && canvas->GetWidth() > 0 && canvas->GetHeight() > 0) {
      x_scale = float(canvas->GetWidth()) / float(canvas->GetHeight());
    }
    const float primary_scale = vertical ? 1.0f : x_scale;
    const float side_scale = vertical ? x_scale : 1.0f;
    for (const Registration *candidate : candidates) {
      if (candidate == *current) {
        continue;
      }
      const MT_Vector3 delta = candidate->screen_position - origin;
      const bool in_direction = (action == LN_UIAction::NavigateUp && delta.y() < 0.0f) ||
                                (action == LN_UIAction::NavigateDown && delta.y() > 0.0f) ||
                                (action == LN_UIAction::NavigateLeft && delta.x() < 0.0f) ||
                                (action == LN_UIAction::NavigateRight && delta.x() > 0.0f);
      if (!in_direction) {
        continue;
      }
      MT_Vector3 candidate_min;
      MT_Vector3 candidate_max;
      NavigationScreenRect(*candidate, candidate_min, candidate_max);
      const float primary = (vertical ? std::abs(delta.y()) : std::abs(delta.x())) *
                            primary_scale;
      const float sideways = std::max(0.0f,
                                      std::max(candidate_min[side_axis], current_min[side_axis]) -
                                          std::min(candidate_max[side_axis],
                                                   current_max[side_axis])) *
                             side_scale;
      if (sideways > primary) {
        continue;
      }
      /* An element in line with it (overlapping it across the way the key goes) goes first,
       * however far: Right in a grid goes to the next cell, not to a tab just over it. */
      const bool in_line = sideways <= 0.0f;
      const float score = primary + sideways * 2.0f;
      if ((in_line && !best_in_line) || (in_line == best_in_line && score < best_score)) {
        best_in_line = in_line;
        best_score = score;
        next = candidate;
      }
    }
    /* The element's own neighbour that way wins (UI Element > Navigation). A grid slot keeps
     * moving inside its grid: its neighbour counts where the grid ends. */
    const ElementRecord *record = FindElementRecord((*current)->object);
    const blender::Object *neighbour =
        record == nullptr                   ? nullptr :
        action == LN_UIAction::NavigateUp   ? record->settings.navigation[0] :
        action == LN_UIAction::NavigateDown ? record->settings.navigation[1] :
        action == LN_UIAction::NavigateLeft ? record->settings.navigation[2] :
                                              record->settings.navigation[3];
    const auto same_grid = [&](const Registration *a, const Registration *b) {
      const auto binding_a = m_slotBindings.find(a->object);
      const auto binding_b = m_slotBindings.find(b->object);
      return binding_a != m_slotBindings.end() && binding_b != m_slotBindings.end() &&
             binding_a->second.view == binding_b->second.view;
    };
    if (neighbour != nullptr && (next == *current || !same_grid(*current, next))) {
      const std::string name = neighbour->id.name + 2;
      for (const Registration *candidate : candidates) {
        if (candidate != *current && candidate->object->GetName() == name) {
          next = candidate;
          break;
        }
      }
    }
  }
  m_focusByUser[user] = m_manager.MakeObjectHandle(next->object, next->object->GetName());
}

const LN_UIService::InteractionState &LN_UIService::GetInteractionState(
    const ListenerKey &listener) const
{
  const auto item = m_currentStates.find(listener);
  if (item == m_currentStates.end()) {
    return s_emptyInteractionState;
  }
  if (!m_passive) {
    return item->second;
  }
  /* Keep what is still true (hover, capture, focus, drag) and drop the frame's one-tick edges so
   * several Logic + Physics steps in one frame cannot repeat a click. */
  InteractionState &state = m_passiveInteractionState;
  state = item->second;
  state.entered = state.exited = state.pressed = state.released = state.clicked = false;
  state.secondary_clicked = false;
  state.drag_started = state.drag_ended = state.drop_accepted = state.drop_rejected = false;
  state.drag_cancelled = state.drop_entered = state.drop_exited = state.dropped = false;
  return state;
}

bool LN_UIService::GetActionState(const LN_UIAction action,
                                  const LN_UIActionState state,
                                  const int32_t user,
                                  const std::string &context) const
{
  if (m_input == nullptr || user < 0 || !ContextMatches(context, user)) {
    return false;
  }
  /* A Back the UI already used; each player has their own. */
  if (action == LN_UIAction::Cancel && state == LN_UIActionState::Triggered &&
      (user == 0 ? m_cancelConsumed :
                   user < LN_MAX_GAMEPADS && ((m_playerBackUsed >> user) & 1u) != 0))
  {
    return false;
  }
  return ActionBindingState(action, state, user, nullptr, BindingFilter::All);
}

bool LN_UIService::ActionBindingState(const LN_UIAction action,
                                      const LN_UIActionState state,
                                      const int32_t user,
                                      const LN_InputConsumption *consumed,
                                      const BindingFilter filter) const
{
  return ActionIndexBindingState(
      LN_InputMap::Get().FindBuiltin(action), state, user, consumed, filter);
}

bool LN_UIService::ActionIndexBindingState(const int32_t action_index,
                                           const LN_UIActionState state,
                                           const int32_t user,
                                           const LN_InputConsumption *consumed,
                                           const BindingFilter filter) const
{
  const LN_InputMap &map = LN_InputMap::Get();
  if (m_input == nullptr || user < 0 || action_index < 0 || size_t(action_index) >= map.Size()) {
    return false;
  }
  const LN_InputAction &definition = map.Action(action_index);
  if (!definition.context.empty() && definition.context != GetActiveInputContext(user)) {
    return false;
  }
  const uint8_t held = LN_InputModifiersHeld(*m_input);
  for (const LN_InputBinding &binding : definition.bindings) {
    const bool mouse = binding.device == LN_InputDevice::Mouse;
    if ((filter == BindingFilter::Mouse && !mouse) ||
        (filter == BindingFilter::NotMouse && mouse) ||
        (filter == BindingFilter::NotWheel && IsWheelBinding(binding)))
    {
      continue;
    }
    if (LN_InputBindingStick(binding) != 0) {
      if (StickBindingState(binding, state, user)) {
        return true;
      }
      continue;
    }
    const LN_InputBindingReading reading = LN_ReadInputBinding(binding, *m_input, user, held);
    switch (state) {
      case LN_UIActionState::Triggered:
        if (!reading.pressed) {
          break;
        }
        if (consumed == nullptr) {
          return true;
        }
        if (binding.device == LN_InputDevice::Gamepad) {
          if (!consumed->IsGamepadPressConsumed(LN_InputMap::Get().GamepadOfUser(user),
                                                binding.code))
          {
            return true;
          }
        }
        else if (IsWheelBinding(binding) ? !consumed->IsWheelConsumed() :
                                           !consumed->IsPressConsumed(binding.input) &&
                                               !consumed->IsStrokeConsumed(binding.input))
        {
          return true;
        }
        break;
      case LN_UIActionState::Active:
        if (reading.level) {
          return true;
        }
        break;
      case LN_UIActionState::Released:
        if (reading.released) {
          return true;
        }
        break;
    }
  }
  return false;
}

bool LN_UIService::StickBindingState(const LN_InputBinding &binding,
                                     const LN_UIActionState state,
                                     const int32_t user) const
{
  const int stick = LN_InputBindingStick(binding);
  if (stick == 0 || user < 0 || user >= LN_MAX_GAMEPADS) {
    return false;
  }
  /* A whole stick counts in any direction. */
  const bool whole = binding.code == GAME_INPUT_GAMEPAD_LEFT_STICK ||
                     binding.code == GAME_INPUT_GAMEPAD_RIGHT_STICK;
  const StickDirection wanted = whole ? StickDirection::None :
                                        StickDirection(1 + (binding.code -
                                                            GAME_INPUT_GAMEPAD_LEFT_STICK_UP) %
                                                               4);
  const auto matches = [&](const StickDirection direction) {
    return whole ? direction != StickDirection::None : direction == wanted;
  };
  /* Stick directions are kept per gamepad. */
  const int32_t gamepad_index = LN_InputMap::Get().GamepadOfUser(user);
  const bool now = gamepad_index >= 0 &&
                   matches(m_stickDirection[size_t(gamepad_index)][size_t(stick - 1)]);
  const bool before = gamepad_index >= 0 &&
                      matches(m_previousStickDirection[size_t(gamepad_index)][size_t(stick - 1)]);
  switch (state) {
    case LN_UIActionState::Active:
      return now;
    case LN_UIActionState::Triggered:
      /* Edges belong to the frame that saw them, not to Logic + Physics steps after it. */
      return !m_passive && now && !before;
    case LN_UIActionState::Released:
      return !m_passive && before && !now;
  }
  return false;
}

void LN_UIService::ConsumeBindingPresses(const LN_UIAction action,
                                         const int32_t user,
                                         const LN_InputSnapshot &input,
                                         const BindingFilter filter,
                                         const bool whole_stroke)
{
  ConsumeActionIndexPresses(
      LN_InputMap::Get().FindBuiltin(action), user, input, filter, whole_stroke);
}

void LN_UIService::ConsumeActionIndexPresses(const int32_t action_index,
                                             const int32_t user,
                                             const LN_InputSnapshot &input,
                                             const BindingFilter filter,
                                             const bool whole_stroke)
{
  const LN_InputMap &map = LN_InputMap::Get();
  if (action_index < 0 || size_t(action_index) >= map.Size()) {
    return;
  }
  const uint8_t held = LN_InputModifiersHeld(input);
  for (const LN_InputBinding &binding : map.Action(action_index).bindings) {
    const bool mouse = binding.device == LN_InputDevice::Mouse;
    if ((filter == BindingFilter::Mouse && !mouse) ||
        (filter == BindingFilter::NotMouse && mouse) ||
        (filter == BindingFilter::NotWheel && IsWheelBinding(binding)) ||
        LN_InputBindingStick(binding) != 0 ||
        !LN_ReadInputBinding(binding, input, user, held).pressed)
    {
      continue;
    }
    if (binding.device == LN_InputDevice::Gamepad) {
      if (binding.code <= GAME_INPUT_GAMEPAD_DPAD_RIGHT) {
        m_manager.ConsumeGamepadPress(LN_InputMap::Get().GamepadOfUser(user), binding.code);
      }
    }
    else if (IsWheelBinding(binding)) {
      m_manager.ConsumeWheel();
    }
    else if (whole_stroke) {
      m_manager.ConsumeInputStroke(binding.input);
    }
    else {
      m_manager.ConsumeInputPress(binding.input);
    }
  }
}

void LN_UIService::ConsumePointerOverUI(const LN_InputSnapshot &input)
{
  for (int32_t code = SCA_IInputDevice::LEFTMOUSE; code <= SCA_IInputDevice::BUTTON7MOUSE; ++code)
  {
    if (input.GetStatus(code, SCA_InputEvent::JUSTACTIVATED)) {
      m_manager.ConsumeInputStroke(code);
    }
  }
  /* Only when this input moved the wheel. */
  if (input.GetMouse().wheel_delta != 0 ||
      input.GetStatus(SCA_IInputDevice::WHEELUPMOUSE, SCA_InputEvent::JUSTACTIVATED) ||
      input.GetStatus(SCA_IInputDevice::WHEELDOWNMOUSE, SCA_InputEvent::JUSTACTIVATED))
  {
    m_manager.ConsumeWheel();
  }
}

void LN_UIService::ConsumeActionPress(const LN_UIAction action, const int32_t user)
{
  if (m_input != nullptr) {
    ConsumeBindingPresses(action, user, *m_input, BindingFilter::All);
  }
}

bool LN_UIService::NavigationForUser(const int32_t user,
                                     LN_UIAction &r_action,
                                     bool &r_from_press)
{
  const auto cached = m_frameNavigation.find(user);
  FrameNavigation &result = cached != m_frameNavigation.end() ? cached->second :
                                                                m_frameNavigation[user];
  if (cached == m_frameNavigation.end()) {
    const LN_InputMap &map = LN_InputMap::Get();
    const std::string &context = GetActiveInputContext(user);
    NavigationRepeat &repeat = m_navigationRepeat[user];
    if (repeat.frame + 1 != m_frameIndex) {
      /* The hold was not watched every frame (no UI to navigate meanwhile): start over. */
      repeat.active = false;
    }
    repeat.frame = m_frameIndex;
    for (const LN_UIAction candidate : kNavigationActions) {
      if (GetActionState(candidate, LN_UIActionState::Triggered, user, context)) {
        result = {candidate, true, true};
        repeat = {candidate, true, 0.0, map.RepeatDelay(), m_frameIndex};
        break;
      }
    }
    if (!result.has_action) {
      if (repeat.active && GetActionState(repeat.action, LN_UIActionState::Active, user, context))
      {
        repeat.held += m_frameDelta;
        if (repeat.held + kRepeatEpsilon >= repeat.next_step) {
          /* One step per frame, even after a long frame. */
          repeat.next_step = std::max(repeat.next_step, repeat.held) + map.RepeatInterval();
          result = {repeat.action, true, false};
        }
      }
      else {
        repeat.active = false;
      }
    }
  }
  r_action = result.action;
  r_from_press = result.from_press;
  return result.has_action;
}

int32_t LN_UIService::ScrollStepForUser(const int32_t user)
{
  const auto cached = m_frameScroll.find(user);
  if (cached != m_frameScroll.end()) {
    return cached->second;
  }
  const LN_InputMap &map = LN_InputMap::Get();
  int32_t step = 0;
  NavigationRepeat &repeat = m_scrollRepeat[user];
  if (repeat.frame + 1 != m_frameIndex) {
    repeat.active = false;
  }
  repeat.frame = m_frameIndex;
  for (const LN_UIAction action : {LN_UIAction::ScrollUp, LN_UIAction::ScrollDown}) {
    if (ActionBindingState(
            action, LN_UIActionState::Triggered, user, nullptr, BindingFilter::NotWheel))
    {
      step = action == LN_UIAction::ScrollUp ? 1 : -1;
      repeat = {action, true, 0.0, map.RepeatDelay(), m_frameIndex};
      break;
    }
  }
  if (step == 0) {
    if (repeat.active &&
        ActionBindingState(
            repeat.action, LN_UIActionState::Active, user, nullptr, BindingFilter::NotWheel))
    {
      repeat.held += m_frameDelta;
      if (repeat.held + kRepeatEpsilon >= repeat.next_step) {
        repeat.next_step = std::max(repeat.next_step, repeat.held) + map.RepeatInterval();
        step = repeat.action == LN_UIAction::ScrollUp ? 1 : -1;
      }
    }
    else {
      repeat.active = false;
    }
  }
  m_frameScroll[user] = step;
  return step;
}

int32_t LN_UIService::ScrollWheelSteps(const LN_InputSnapshot *input) const
{
  const int32_t wheel = input != nullptr ? input->GetMouse().wheel_delta : 0;
  if (wheel == 0) {
    return 0;
  }
  const LN_InputMap &map = LN_InputMap::Get();
  const uint8_t held = LN_InputModifiersHeld(*input);
  /* The wheel bindings of Scroll Up and Scroll Down: swapping them inverts the wheel. */
  int32_t steps = 0;
  for (const LN_UIAction action : {LN_UIAction::ScrollUp, LN_UIAction::ScrollDown}) {
    const LN_InputAction &definition = map.Action(map.FindBuiltin(action));
    if (!definition.context.empty() && definition.context != GetActiveInputContext()) {
      continue;
    }
    for (const LN_InputBinding &binding : definition.bindings) {
      if (IsWheelBinding(binding) && LN_ReadInputBinding(binding, *input, 0, held).level) {
        steps += action == LN_UIAction::ScrollUp ? std::abs(wheel) : -std::abs(wheel);
        break;
      }
    }
  }
  return steps;
}

void LN_UIService::ConsumeInputBeforeResolve(const LN_InputSnapshot &sample)
{
  /* While player 0 types into a Text Field, every key is the text's, as in ResolveTextEditing:
   * a letter must not reach the game first (M typed into a name cycled the map). */
  if (m_editingText.IsValid()) {
    for (int32_t code = SCA_IInputDevice::BEGINKEY + 1; code < SCA_IInputDevice::ENDKEY; ++code) {
      if (sample.GetStatus(code, SCA_InputEvent::JUSTACTIVATED)) {
        m_manager.ConsumeInputStroke(code);
      }
    }
  }
  /* What a click picked up owns the mouse buttons and Back until it is put down, wherever the
   * pointer is. */
  bool picked_up = false;
  for (const auto &pointers : m_pointerStates) {
    const auto pointer = pointers.find(0);
    picked_up = picked_up || (pointer != pointers.end() && pointer->second.picked_up);
  }
  if (picked_up) {
    if (sample.GetStatus(SCA_IInputDevice::LEFTMOUSE, SCA_InputEvent::JUSTACTIVATED)) {
      m_manager.ConsumeInputStroke(SCA_IInputDevice::LEFTMOUSE);
    }
    ConsumeBindingPresses(LN_UIAction::Secondary, 0, sample, BindingFilter::Mouse, true);
    ConsumeBindingPresses(LN_UIAction::Cancel, 0, sample, BindingFilter::All);
  }
  /* Mouse buttons and the wheel over the UI: the UI the last frame found, tested where this
   * input's pointer is and where that UI is now (both may have moved since). */
  bool pointer_input = sample.GetMouse().wheel_delta != 0 ||
                       sample.GetStatus(SCA_IInputDevice::WHEELUPMOUSE,
                                        SCA_InputEvent::JUSTACTIVATED) ||
                       sample.GetStatus(SCA_IInputDevice::WHEELDOWNMOUSE,
                                        SCA_InputEvent::JUSTACTIVATED);
  for (int32_t code = SCA_IInputDevice::LEFTMOUSE;
       code <= SCA_IInputDevice::BUTTON7MOUSE && !pointer_input;
       ++code)
  {
    pointer_input = sample.GetStatus(code, SCA_InputEvent::JUSTACTIVATED);
  }
  if (pointer_input) {
    if (PointerOverSurface(m_blockSurfaces, sample.GetMouse())) {
      ConsumePointerOverUI(sample);
    }
    /* The wheel scrolls the view under the pointer, not the game camera as well. */
    else if (ScrollWheelSteps(&sample) != 0 &&
             PointerOverSurface(m_wheelSurfaces, sample.GetMouse()))
    {
      m_manager.ConsumeWheel();
    }
  }
  /* Back that an open menu will use at its next frame (to go back) is not for game logic either.
   * Only the press: a release still reaches Keyboard Key nodes. A HUD opened as a menu, and a
   * menu whose Back is Custom, leave it to the game. */
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    const int top_index = InputMenuIndex(user);
    if (top_index < 0) {
      continue;
    }
    const auto top = m_sessions.find(m_menuStack[size_t(top_index)]);
    if (top != m_sessions.end() && top->second.back_behavior == LN_UIMenuBackBehavior::CloseMenu)
    {
      ConsumeBindingPresses(LN_UIAction::Cancel,
                            user,
                            sample,
                            user == 0 ? BindingFilter::All : BindingFilter::NotMouse);
    }
  }
  /* A pointer cursor's buttons are its own while it is on; View Center's press only while it
   * aimed at a UI Element with Block last frame. */
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    if (user == 0 && m_editingText.IsValid()) {
      continue;
    }
    if (!VirtualPointerActive(user)) {
      /* Player 0's cursor that follows the mouse: its Press Action (Enter, A) clicks under it at
       * the next UI frame (UpdatePlayerPointers), so it is not the game's either. */
      if (PointerActive(user) && !AimPointerActive(user)) {
        ConsumePointerButton(user, sample);
      }
      continue;
    }
    if (AimPointerActive(user)) {
      if (m_playerPointers[size_t(user)].aim_blocked) {
        ConsumePointerButton(user, sample);
      }
      continue;
    }
    ConsumePointerButton(user, sample);
    ConsumeBindingPresses(LN_UIAction::Secondary, user, sample, BindingFilter::NotMouse, true);
  }
  /* Grid View Use: as SlotUsedBy will take them at the next frame, from last frame's slots. */
  const auto holds_item = [&](KX_GameObject *slot) {
    LN_Value item;
    int32_t index = -1;
    return slot != nullptr && GetBoundItem(slot, item, index) && item.exists &&
           !LN_InventoryIsEmptyPlace(item);
  };
  const auto claim = [&](const std::string &name, const int32_t user) {
    const int32_t action = name.empty() ? -1 : LN_InputMap::Get().Find(name);
    if (action >= 0) {
      ConsumeActionIndexPresses(action, user, sample, BindingFilter::All);
    }
  };
  for (const auto &[user, highlight] : m_highlights) {
    KX_GameObject *slot = m_manager.ResolveObjectHandle(highlight.object);
    const ViewRegistration *view = slot != nullptr ? GridOfSlot(slot) : nullptr;
    if (view != nullptr && holds_item(slot)) {
      claim(view->use_action, user);
    }
  }
  for (const ViewRegistration &view : m_viewRegistrations) {
    if (view.kind != ViewKind::Grid || view.use_keys.empty()) {
      continue;
    }
    const std::vector<std::string> keys = CommaList(view.use_keys);
    for (size_t place = 0; place < keys.size() && place < view.slots.size(); ++place) {
      KX_GameObject *slot = view.slots[place];
      if (holds_item(slot) && UseKeysReach(view.user, MenuStackIndexForObject(slot))) {
        claim(keys[place], view.user);
      }
    }
  }
  /* Only for a player who had a control keys and gamepad can reach in the last frame. */
  for (const int32_t user : m_directedTargetUsers) {
    if (!DirectedInputAllowed(user)) {
      continue;
    }
    for (const LN_UIAction action : {LN_UIAction::NavigateUp,
                                     LN_UIAction::NavigateDown,
                                     LN_UIAction::NavigateLeft,
                                     LN_UIAction::NavigateRight,
                                     LN_UIAction::Accept,
                                     LN_UIAction::Next,
                                     LN_UIAction::Previous})
    {
      ConsumeBindingPresses(action, user, sample, BindingFilter::All);
    }
    ConsumeBindingPresses(LN_UIAction::Secondary, user, sample, BindingFilter::NotMouse);
    /* Grab only where it picks up or puts down: elsewhere its keys stay with the game. It puts
     * down what the mouse holds, and with the mouse it picks up what the mouse points at (the
     * hover is still last frame's here, as in ResolveInteractions until it is resolved). */
    bool carrying = false;
    bool held_by_mouse = false;
    for (const auto &pointers : m_pointerStates) {
      const auto pointer = pointers.find(user);
      carrying = carrying || (pointer != pointers.end() && pointer->second.carried);
      held_by_mouse = held_by_mouse || (pointer != pointers.end() && pointer->second.picked_up);
    }
    KX_GameObject *pointed = PointedForKeys(user, true);
    const Registration *focused = DirectedRecipient(
        user, pointed != nullptr ? pointed : GetFocus(user, GetActiveInputContext(user)));
    if (carrying || held_by_mouse ||
        (focused != nullptr && focused->role == LN_UIInteractableRole::DragSource))
    {
      ConsumeBindingPresses(LN_UIAction::Grab, user, sample, BindingFilter::NotMouse);
    }
    /* Back puts a carried element back, also outside menus. */
    if (carrying) {
      ConsumeBindingPresses(LN_UIAction::Cancel, user, sample, BindingFilter::NotMouse);
    }
  }
}

LN_UIResult LN_UIService::SetFocus(const int32_t user,
                                   KX_GameObject *object,
                                   const std::string &context)
{
  if (user < 0 || !ContextMatches(context, user)) {
    return LN_UIResult::InvalidTarget;
  }
  if (object == nullptr) {
    m_focusByUser.erase(user);
    m_focusOutsideMenuUsers.erase(user);
    return LN_UIResult::Success;
  }
  if (object->GetScene() != &m_scene) {
    return LN_UIResult::WrongScene;
  }
  if (m_manager.IsGameObjectQueuedForRemoval(object)) {
    return LN_UIResult::InvalidTarget;
  }
  if (!IsRegisteredFocusable(user, object) || !ObjectPassesModalRouting(object, user)) {
    return LN_UIResult::InvalidTarget;
  }
  m_focusByUser[user] = m_manager.MakeObjectHandle(object, object->GetName());
  /* A UI built without Open Object Menu (a title screen) asks for keyboard and gamepad control
   * this way. Menus have it anyway (a HUD opened as a menu does not). */
  if (InputMenuIndex(user) < 0) {
    m_focusOutsideMenuUsers.insert(user);
  }
  return LN_UIResult::Success;
}

KX_GameObject *LN_UIService::GetFocus(const int32_t user, const std::string &context) const
{
  if (user < 0 || !ContextMatches(context, user)) {
    return nullptr;
  }
  const auto item = m_focusByUser.find(user);
  KX_GameObject *object = item != m_focusByUser.end() ?
                              m_manager.ResolveObjectHandle(item->second) :
                              nullptr;
  if (!IsRegisteredFocusable(user, object) || !ObjectPassesModalRouting(object, user)) {
    return nullptr;
  }
  return object;
}

const std::string &LN_UIService::GetActiveInputContext(const int32_t user) const
{
  for (auto it = m_menuStack.rbegin(); it != m_menuStack.rend(); ++it) {
    const auto session = m_sessions.find(*it);
    /* A HUD opened as a menu is not where the player's input goes (InputMenuIndex). */
    if (session != m_sessions.end() && session->second.user == user &&
        !session->second.input_context.empty() && !IsHudLayer(session->second))
    {
      return session->second.input_context;
    }
  }
  return s_emptyContext;
}

void LN_UIService::ApplyPendingInitialFocus()
{
  /* Each player's top menu focuses its first control once. */
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    const int top = InputMenuIndex(user);
    if (top < 0) {
      continue;
    }
    const auto session_it = m_sessions.find(m_menuStack[size_t(top)]);
    if (session_it == m_sessions.end() || !session_it->second.needs_initial_focus) {
      continue;
    }
    MenuSession &session = session_it->second;
    /* Start where Tab navigation starts: the lowest Tab Order, then priority, then reading
     * order. The flag stays set until a focusable control exists, because the menu's controls
     * may only register on a later tick. */
    std::unordered_map<int32_t, const Registration *> first_by_user;
    for (const Registration &registration : m_registrations) {
      /* Only the menu's own player: a listener left at another player's User in it does not
       * pull that player's focus in. The menu index is cached, so a menu without controls does
       * not search itself for each registration every frame. */
      if (!registration.enabled || !registration.focusable || !registration.navigable ||
          registration.user != session.user ||
          MenuStackIndexForObject(registration.object) != top ||
          !ObjectAcceptsDirectedInput(registration.object, registration.user))
      {
        continue;
      }
      const Registration *&first = first_by_user[registration.user];
      if (first == nullptr || NavigationOrderLess(registration, *first)) {
        first = &registration;
      }
    }
    if (first_by_user.empty()) {
      continue;
    }
    session.needs_initial_focus = false;
    for (const auto &[focus_user, registration] : first_by_user) {
      const auto existing = m_focusByUser.find(focus_user);
      if (existing != m_focusByUser.end()) {
        KX_GameObject *current = m_manager.ResolveObjectHandle(existing->second);
        if (SessionContainsObject(session, current) &&
            ObjectAcceptsDirectedInput(current, focus_user))
        {
          continue;
        }
      }
      /* Shown at once for keyboard and gamepad players; a mouse player sees it after the first
       * key or button press. */
      m_focusByUser[focus_user] = m_manager.MakeObjectHandle(registration->object,
                                                             registration->object->GetName());
    }
  }
  /* Take UI Control selects the first element of what the player took, once (outside their
   * menus: those keep the selection while open). */
  for (UIControl &control : m_controls) {
    KX_GameObject *root = m_manager.ResolveObjectHandle(control.root);
    if (!control.focus_pending || root == nullptr || InputMenuIndex(control.user) >= 0) {
      continue;
    }
    const auto existing = m_focusByUser.find(control.user);
    if (existing != m_focusByUser.end() &&
        IsDescendantOf(m_manager.ResolveObjectHandle(existing->second), root))
    {
      control.focus_pending = false;
      continue;
    }
    const Registration *first = nullptr;
    for (const Registration &registration : m_registrations) {
      if (registration.user == control.user && registration.enabled && registration.focusable &&
          registration.navigable && MenuStackIndexForObject(registration.object) < 0 &&
          IsDescendantOf(registration.object, root) &&
          (first == nullptr || NavigationOrderLess(registration, *first)))
      {
        first = &registration;
      }
    }
    if (first != nullptr) {
      m_focusByUser[control.user] = m_manager.MakeObjectHandle(first->object,
                                                               first->object->GetName());
      control.focus_pending = false;
      if (control.user >= 0 && control.user < LN_MAX_GAMEPADS) {
        m_controlFocusFrame[size_t(control.user)] = m_frameIndex;
      }
    }
  }
}

void LN_UIService::RestoreFocusAfterMenuClose(const MenuSession &session)
{
  for (auto it = m_focusByUser.begin(); it != m_focusByUser.end();) {
    if (!SessionContainsHandle(session, it->second)) {
      ++it;
      continue;
    }
    if (OtherSessionContainsHandle(session.handle, it->second)) {
      ++it;
      continue;
    }
    const auto previous = session.previous_focus.find(it->first);
    if (previous != session.previous_focus.end() &&
        !SessionContainsHandle(session, previous->second) &&
        m_manager.ResolveObjectHandle(previous->second) != nullptr)
    {
      it->second = previous->second;
      ++it;
    }
    else {
      m_focusOutsideMenuUsers.erase(it->first);
      it = m_focusByUser.erase(it);
    }
  }
}

void LN_UIService::RebaseFocusHistoryAfterMenuClose(const MenuSession &session)
{
  /* Handles allow menus to close in any order. A newer menu may have remembered focus inside an
   * older menu; if that older menu closes first, splice its predecessor into the newer history so
   * closing the newer menu later cannot restore a dead object. */
  const auto closed = std::find(m_menuStack.begin(), m_menuStack.end(), session.handle);
  if (closed == m_menuStack.end()) {
    return;
  }
  for (auto stack_item = std::next(closed); stack_item != m_menuStack.end(); ++stack_item) {
    const auto menu = m_sessions.find(*stack_item);
    if (menu == m_sessions.end()) {
      continue;
    }
    for (auto previous = menu->second.previous_focus.begin();
         previous != menu->second.previous_focus.end();)
    {
      if (!SessionContainsHandle(session, previous->second)) {
        ++previous;
        continue;
      }
      const auto replacement = session.previous_focus.find(previous->first);
      if (replacement != session.previous_focus.end() && replacement->second.IsValid() &&
          !SessionContainsHandle(session, replacement->second))
      {
        previous->second = replacement->second;
        ++previous;
      }
      else {
        previous = menu->second.previous_focus.erase(previous);
      }
    }
  }
}

void LN_UIService::ReleaseListener(const ListenerKey &listener, const bool synthesize_exit)
{
  for (size_t phase = 0; phase < 3; ++phase) {
    std::vector<int32_t> cancelled_users;
    for (auto &pointer : m_pointerStates[phase]) {
      if (pointer.second.has_capture && pointer.second.captured_listener == listener) {
        cancelled_users.push_back(pointer.first);
      }
      else if (pointer.second.has_drop_target && pointer.second.drop_listener == listener) {
        pointer.second.has_drop_target = false;
        pointer.second.drop_listener = ListenerKey();
        pointer.second.drop_object = LN_ObjectHandle();
      }
    }
    for (const int32_t user : cancelled_users) {
      CancelPointer(phase, user, true);
    }
    auto state = m_listenerStates[phase].find(listener);
    if (state != m_listenerStates[phase].end()) {
      if (synthesize_exit && phase == PhaseIndex(m_event) && state->second.over) {
        m_currentStates[listener].exited = true;
      }
      m_listenerStates[phase].erase(state);
    }
  }
  std::erase_if(m_registrations,
                [&](const Registration &registration) { return registration.listener == listener; });
  m_registeredListeners.erase(listener);
  std::erase_if(m_registeredTargets,
                [&](const RegistrationKey &key) { return key.listener == listener; });
  m_listenerDeclarations.erase(listener);
}

void LN_UIService::ReleaseObjectState(KX_GameObject *object)
{
  if (object == nullptr) {
    return;
  }
  const LN_ObjectHandle removed_object = m_manager.MakeObjectHandle(object, object->GetName());
  const KX_Camera *removed_camera = dynamic_cast<KX_Camera *>(object);
  for (size_t phase = 0; phase < 3; ++phase) {
    std::vector<int32_t> cancelled_users;
    for (const auto &pointer : m_pointerStates[phase]) {
      if (pointer.second.has_capture &&
          (SameObjectHandle(pointer.second.captured_object, removed_object) ||
           (removed_camera != nullptr && pointer.second.captured_camera == removed_camera)))
      {
        cancelled_users.push_back(pointer.first);
      }
    }
    for (const int32_t user : cancelled_users) {
      CancelPointer(phase, user, true);
    }
  }
  for (size_t phase = 0; phase < 3; ++phase) {
    std::vector<ListenerKey> listeners;
    for (const auto &item : m_listenerStates[phase]) {
      if (m_manager.ResolveObjectHandle(item.second.object) == object) {
        listeners.push_back(item.first);
      }
    }
    for (const ListenerKey &listener : listeners) {
      ReleaseListener(listener, false);
    }
  }
  std::unordered_set<RegistrationKey, RegistrationKeyHash> removed_registrations;
  std::unordered_set<ListenerKey, ListenerKeyHash> removed_camera_listeners;
  if (removed_camera != nullptr) {
    for (const Registration &registration : m_registrations) {
      if (registration.camera == removed_camera) {
        removed_registrations.insert({registration.listener, registration.object});
        removed_camera_listeners.insert(registration.listener);
      }
    }
  }
  std::erase_if(m_registrations, [&](const Registration &registration) {
    return registration.object == object ||
           (removed_camera != nullptr && registration.camera == removed_camera);
  });
  std::erase_if(m_registeredTargets,
                [&](const RegistrationKey &key) {
                  return key.object == object || removed_registrations.contains(key);
                });
  for (const ListenerKey &listener : removed_camera_listeners) {
    const bool listener_still_registered = std::any_of(
        m_registrations.begin(), m_registrations.end(), [&](const Registration &candidate) {
          return candidate.listener == listener;
        });
    if (!listener_still_registered) {
      m_registeredListeners.erase(listener);
    }
  }
  RemoveElementRecord(object);
  for (auto focus = m_focusByUser.begin(); focus != m_focusByUser.end();) {
    if (m_manager.ResolveObjectHandle(focus->second) == object) {
      /* The UI Set UI Focus gave the keyboard to went away with its focused object. */
      m_focusOutsideMenuUsers.erase(focus->first);
      focus = m_focusByUser.erase(focus);
    }
    else {
      ++focus;
    }
  }

  std::unordered_set<ListenerKey, ListenerKeyHash> removed_views;
  for (const ViewRegistration &view : m_viewRegistrations) {
    if (view.viewport == object || view.content == object || view.camera == object ||
        std::find(view.slots.begin(), view.slots.end(), object) != view.slots.end())
    {
      removed_views.insert(view.listener);
    }
  }
  for (const ListenerKey &listener : removed_views) {
    ReleaseView(listener);
    m_currentViewStates.erase(listener);
  }
  std::erase_if(m_viewRegistrations, [&](const ViewRegistration &view) {
    return view.viewport == object || view.content == object || view.camera == object ||
           std::find(view.slots.begin(), view.slots.end(), object) != view.slots.end();
  });
}

void LN_UIService::NotifyObjectRemoved(KX_GameObject *object)
{
  /* The address of a removed object can be reused by the next one created. */
  m_menuIndexByObject.erase(object);
  /* A split drag's copy goes with its slot; a copy removed otherwise (its menu closed) is
   * forgotten. */
  for (auto ghost = m_dragGhosts.begin(); ghost != m_dragGhosts.end();) {
    const bool slot_gone = ghost->first == object;
    const bool copy_gone = std::find(ghost->second.members.begin(),
                                     ghost->second.members.end(),
                                     object) != ghost->second.members.end();
    if (!slot_gone && !copy_gone) {
      ++ghost;
      continue;
    }
    for (const KX_GameObject *member : ghost->second.members) {
      m_dragGhostObjects.erase(member);
    }
    if (slot_gone) {
      RemoveDragGhost(ghost->second.ghost);
    }
    ghost = m_dragGhosts.erase(ghost);
  }
  m_dragGhostObjects.erase(object);
  std::vector<MenuHandle> camera_sessions;
  const KX_Camera *removed_camera = dynamic_cast<KX_Camera *>(object);
  const bool removes_overlay_view = removed_camera != nullptr && !m_sessions.empty() &&
                                    m_scene.OverlayCameraMatches(removed_camera);
  for (const auto &item : m_sessions) {
    if (item.second.camera == object || removes_overlay_view) {
      camera_sessions.push_back(item.first);
    }
  }
  std::sort(camera_sessions.begin(),
            camera_sessions.end(),
            [this](const MenuHandle a, const MenuHandle b) {
              return std::find(m_menuStack.begin(), m_menuStack.end(), a) >
                     std::find(m_menuStack.begin(), m_menuStack.end(), b);
            });
  for (const MenuHandle handle : camera_sessions) {
    CloseMenu(handle);
  }
  CloseMenusOwnedBy(object);
  ReleaseObjectState(object);
  for (auto &view_state : m_viewStates) {
    std::erase_if(view_state.second.managed_objects, [&](const auto &managed) {
      return m_manager.ResolveObjectHandle(managed.object) == object;
    });
  }
  m_viewVisibilityStates.erase(object);
  m_slotBindings.erase(object);
  /* What a player took control of is free again; a pointer whose surface goes stops next frame
   * (its handle no longer resolves). */
  std::erase_if(m_controls, [&](const UIControl &control) {
    return m_manager.ResolveObjectHandle(control.root) == object;
  });
}

void LN_UIService::NotifyRuntimeTreeRemoved(const LN_RuntimeTree *tree)
{
  if (tree == nullptr) {
    return;
  }
  std::vector<MenuHandle> owned_sessions;
  for (auto it = m_menuStack.rbegin(); it != m_menuStack.rend(); ++it) {
    const auto session = m_sessions.find(*it);
    if (session != m_sessions.end() && session->second.owner_tree == tree) {
      owned_sessions.push_back(*it);
    }
  }
  for (const MenuHandle handle : owned_sessions) {
    CloseMenu(handle);
  }
  std::unordered_set<ListenerKey, ListenerKeyHash> listener_set;
  for (const auto &declaration : m_listenerDeclarations) {
    if (declaration.first.tree == tree) {
      listener_set.insert(declaration.first);
    }
  }
  for (const Registration &registration : m_registrations) {
    if (registration.listener.tree == tree) {
      listener_set.insert(registration.listener);
    }
  }
  for (size_t phase = 0; phase < 3; ++phase) {
    for (const auto &item : m_listenerStates[phase]) {
      if (item.first.tree == tree) {
        listener_set.insert(item.first);
      }
    }
  }
  for (const ListenerKey &listener : listener_set) {
    ReleaseListener(listener, false);
  }
  std::unordered_set<ListenerKey, ListenerKeyHash> view_set;
  for (const auto &item : m_viewStates) {
    if (item.first.tree == tree) {
      view_set.insert(item.first);
    }
  }
  for (const ViewRegistration &view : m_viewRegistrations) {
    if (view.listener.tree == tree) {
      view_set.insert(view.listener);
    }
  }
  for (const ListenerKey &listener : view_set) {
    ReleaseView(listener);
    m_currentViewStates.erase(listener);
  }
  std::erase_if(m_viewRegistrations,
                [&](const ViewRegistration &view) { return view.listener.tree == tree; });
}

void LN_UIService::NotifyOverlayCollectionRemoved(blender::Collection *collection)
{
  if (collection == nullptr) {
    return;
  }
  std::vector<MenuHandle> removed_handles;
  std::vector<int32_t> removed_users;
  std::unordered_set<KX_GameObject *> removed_objects;
  std::vector<LN_ObjectHandle> removed_object_handles;
  for (auto it = m_menuStack.rbegin(); it != m_menuStack.rend(); ++it) {
    const auto session = m_sessions.find(*it);
    if (session != m_sessions.end() && session->second.collection == collection) {
      removed_handles.push_back(*it);
      for (const LN_ObjectHandle &object_handle : session->second.objects) {
        removed_object_handles.push_back(object_handle);
        if (KX_GameObject *object = m_manager.ResolveObjectHandle(object_handle)) {
          removed_objects.insert(object);
        }
      }
    }
  }
  for (const MenuHandle handle : removed_handles) {
    if (m_closingMenus.contains(handle)) {
      continue;
    }
    const auto session = m_sessions.find(handle);
    if (session == m_sessions.end()) {
      continue;
    }
    m_closingMenus.insert(handle);
    RestoreFocusAfterMenuClose(session->second);
    RebaseFocusHistoryAfterMenuClose(session->second);
    m_menuStack.erase(std::remove(m_menuStack.begin(), m_menuStack.end(), handle),
                      m_menuStack.end());
    removed_users.push_back(session->second.user);
    m_sessions.erase(session);
    m_closingMenus.erase(handle);
  }
  for (const int32_t user : removed_users) {
    NoteMenuStackChanged(user);
  }
  if (!removed_handles.empty()) {
    RecomputeCoverage();
    UpdateMenuPause();
  }
  for (size_t phase = 0; phase < 3; ++phase) {
    std::vector<int32_t> cancelled_users;
    for (const auto &pointer : m_pointerStates[phase]) {
      const bool captured_object_removed = pointer.second.has_capture &&
                                           std::any_of(removed_object_handles.begin(),
                                                       removed_object_handles.end(),
                                                       [&](const LN_ObjectHandle &removed) {
                                                         return SameObjectHandle(
                                                             removed, pointer.second.captured_object);
                                                       });
      if (captured_object_removed) {
        cancelled_users.push_back(pointer.first);
      }
    }
    for (const int32_t user : cancelled_users) {
      CancelPointer(phase, user, true);
    }
  }
  for (KX_GameObject *object : removed_objects) {
    ReleaseObjectState(object);
  }
}

void LN_UIService::Clear()
{
  /* The scene frees split drag copies with the template copies. */
  m_dragGhosts.clear();
  m_dragGhostObjects.clear();
  m_cellsShown.clear();
  m_messages.clear();
  std::vector<ListenerKey> views;
  views.reserve(m_viewStates.size());
  for (auto &item : m_viewStates) {
    /* The scene frees the template copies with everything else. */
    item.second.template_copies.clear();
    views.push_back(item.first);
  }
  for (const ListenerKey &listener : views) {
    ReleaseView(listener);
  }
  m_sessions.clear();
  ReleaseMenuPause(true);
  m_menuStack.clear();
  m_closingMenus.clear();
  m_registrations.clear();
  m_listenerDeclarations.clear();
  m_registeredListeners.clear();
  m_registeredTargets.clear();
  m_viewRegistrations.clear();
  m_viewSlotsVersion++;
  m_currentStates.clear();
  m_currentViewStates.clear();
  m_viewVisibilityStates.clear();
  m_slotBindings.clear();
  m_barShapes.clear();
  m_barWarnings.clear();
  m_tooltips.clear();
  m_hints.clear();
  m_gridWarnings.clear();
  for (size_t phase = 0; phase < 3; ++phase) {
    m_listenerStates[phase].clear();
    m_pointerStates[phase].clear();
  }
  m_focusByUser.clear();
  m_directedUsers.clear();
  m_focusOutsideMenuUsers.clear();
  m_menuIndexByObject.clear();
  m_menuIndexValid = false;
  m_stickDirection = {};
  m_previousStickDirection = {};
  m_stickWaitsForCentre = {};
  m_navigationRepeat.clear();
  m_frameNavigation.clear();
  m_scrollRepeat.clear();
  m_frameScroll.clear();
  m_pointerBlockedByUI = false;
  m_pointerOverBlockingUI = false;
  m_blockSurfaces.clear();
  m_wheelSurfaces.clear();
  m_directedTargetUsers.clear();
  m_viewNavigationHandledUsers.clear();
  m_elements.clear();
  m_elementIndex.clear();
  m_elementsGeneration = UINT64_MAX;
  m_elementEvents.clear();
  m_soundEvents.clear();
  m_backSounds.clear();
  m_input = nullptr;
  m_tickIndex = UINT64_MAX;
  m_passive = false;
  m_resolving = false;
  m_cancelConsumed = false;
  m_playerBackUsed = 0;
  m_templateCopiesToRemove.clear();
  m_viewScrollRequests.clear();
  m_pointerHovers.fill(LN_ObjectHandle());
  m_previousPointerHovers.fill(LN_ObjectHandle());
  m_pointerGrabUsed = 0;
  m_mouseUsedForKeysIn = 0;
  m_keysPlace = 0;
  m_keysPlaceAge = 0.0;
  m_playerPointers = {};
  m_pointersFrame = UINT64_MAX;
  m_controls.clear();
  m_controlFocusFrame = {};
}

/* -------------------------------------------------------------------- */
/** \name UI Elements
 *
 * Objects whose UI Element panel is enabled. The registry is rebuilt only when the scene's object
 * list changes and settings are read once per object, so a frame only hit-tests and routes.
 * \{ */

namespace {

std::string FixedString(const char *value, const size_t capacity)
{
  return std::string(value, strnlen(value, capacity));
}


LN_UIService::ElementSettings ElementSettingsFromObject(const blender::Object &object)
{
  const blender::ObjectGameUI &ui = object.game_ui;
  LN_UIService::ElementSettings settings;
  settings.action = FixedString(ui.action, sizeof(ui.action));
  settings.drop_group = FixedString(ui.drop_group, sizeof(ui.drop_group));
  settings.accept_categories = NormalizedCategories(
      FixedString(ui.accept_categories, sizeof(ui.accept_categories)));
  settings.pointer = LN_UIPropagation(std::clamp(int(ui.pointer_mode), 0, 2));
  settings.focus = LN_UIElementFocus(std::clamp(int(ui.focus_mode), 0, 2));
  settings.anchor = LN_UIElementAnchor(std::clamp(int(ui.anchor), 0, 9));
  settings.player = std::clamp(int32_t(ui.player), 0, LN_MAX_GAMEPADS - 1);
  settings.any_player = (ui.flag & blender::OB_UI_ANY_PLAYER) != 0;
  settings.tab_order = ui.tab_order;
  settings.priority = ui.priority;
  settings.padding = std::max(0.0f, ui.padding);
  settings.drag_threshold = std::max(0.0f, ui.drag_threshold);
  settings.hover_scale = std::max(0.01f, ui.hover_scale);
  settings.press_scale = std::max(0.01f, ui.press_scale);
  settings.look_time = std::max(0.0f, ui.look_time);
  settings.draggable = (ui.flag & blender::OB_UI_DRAGGABLE) != 0;
  settings.follow_cursor = (ui.flag & blender::OB_UI_DRAG_NO_FOLLOW) == 0;
  settings.drag_item_only = ui.drag_look == blender::OB_UI_DRAG_LOOK_ITEM;
  settings.drop_target = (ui.flag & blender::OB_UI_DROP_TARGET) != 0;
  settings.click_pick_up = (ui.flag & blender::OB_UI_CLICK_PICK_UP) != 0;
  settings.highlight = ui.highlight;
  settings.refused = ui.refused;
  settings.tooltip = ui.tooltip;
  settings.tooltip_text = FixedString(ui.tooltip_text, sizeof(ui.tooltip_text));
  if (settings.tooltip_text.empty()) {
    settings.tooltip_text = "{name}\n{description}";
  }
  else {
    /* A text field takes no line breaks: "\n" stands for one. */
    for (size_t at = settings.tooltip_text.find("\\n"); at != std::string::npos;
         at = settings.tooltip_text.find("\\n", at + 1))
    {
      settings.tooltip_text.replace(at, 2, "\n");
    }
  }
  settings.tooltip_mode = LN_UITooltipMode(
      std::clamp(int(ui.tooltip_mode), 0, int(LN_UITooltipMode::TextOnly)));
  settings.tooltip_offset = MT_Vector3(
      ui.tooltip_offset[0], ui.tooltip_offset[1], ui.tooltip_offset[2]);
  settings.widget = LN_UIWidget(std::clamp(int(ui.widget), 0, int(LN_UIWidget::Bar)));
  settings.navigation = {ui.nav_up, ui.nav_down, ui.nav_left, ui.nav_right};
  settings.widget_object = ui.widget_object;
  settings.widget_part = ui.widget_part;
  settings.widget_property = FixedString(ui.widget_property, sizeof(ui.widget_property));
  settings.widget_max_property = FixedString(ui.widget_max_property,
                                             sizeof(ui.widget_max_property));
  settings.widget_min = ui.widget_min;
  settings.widget_max = ui.widget_max;
  settings.widget_step = std::max(0.0f, ui.widget_step);
  return settings;
}

bool ObjectBounds(KX_GameObject *object, MT_Vector3 &r_min, MT_Vector3 &r_max)
{
  blender::Object *blender_object = object != nullptr ? object->GetBlenderObject() : nullptr;
  const std::optional<blender::Bounds<blender::float3>> bounds =
      blender_object != nullptr ? BKE_object_boundbox_eval_cached_get(blender_object) :
                                  std::nullopt;
  if (!bounds.has_value()) {
    return false;
  }
  r_min = MT_Vector3(bounds->min.x, bounds->min.y, bounds->min.z);
  r_max = MT_Vector3(bounds->max.x, bounds->max.y, bounds->max.z);
  return true;
}

bool FramesMatch(const CameraFrame &frame, const float applied[4])
{
  const float values[4] = {frame.left, frame.right, frame.bottom, frame.top};
  for (int index = 0; index < 4; ++index) {
    const float tolerance = 1.0e-5f * std::max(1.0f, std::abs(values[index]));
    if (std::abs(values[index] - applied[index]) > tolerance) {
      return false;
    }
  }
  return true;
}

void StoreFrame(const CameraFrame &frame, float r_applied[4])
{
  r_applied[0] = frame.left;
  r_applied[1] = frame.right;
  r_applied[2] = frame.bottom;
  r_applied[3] = frame.top;
}

}  // namespace

void LN_UIService::SetObjectBoundsForTests(BoundsFunction function)
{
  m_objectBoundsForTests = std::move(function);
}

void LN_UIService::SetPlayerCameraForTests(PlayerCameraFunction function)
{
  m_playerCameraForTests = std::move(function);
}

void LN_UIService::SetWorldRayForTests(WorldRayFunction function)
{
  m_worldRayForTests = std::move(function);
}

void LN_UIService::SetWorldDropForTests(WorldDropFunction function)
{
  m_worldDropForTests = std::move(function);
}

void LN_UIService::SetDragGhostForTests(DragGhostMakeFunction make,
                                        DragGhostRemoveFunction remove)
{
  m_makeDragGhostForTests = std::move(make);
  m_removeDragGhostForTests = std::move(remove);
}

void LN_UIService::SetItemLibraryForTests(ItemLibraryFunction function)
{
  m_itemLibraryForTests = std::move(function);
}

bool LN_UIService::LocalBounds(KX_GameObject *object, MT_Vector3 &r_min, MT_Vector3 &r_max) const
{
  return m_objectBoundsForTests ? m_objectBoundsForTests(object, r_min, r_max) :
                                  ObjectBounds(object, r_min, r_max);
}

KX_Camera *LN_UIService::PlayerViewCamera(const int32_t user) const
{
  if (m_playerCameraForTests) {
    return m_playerCameraForTests(user);
  }
  KX_Camera *active_camera = m_scene.GetActiveCamera();
  std::array<int, KX_PlayerViews::MaxPlayers> players;
  std::array<KX_Camera *, KX_PlayerViews::MaxPlayers> cameras;
  const int count = m_scene.GetPlayerViews().CollectViews(active_camera, players, cameras);
  for (int index = 0; index < count; ++index) {
    if (players[size_t(index)] == user) {
      return cameras[size_t(index)];
    }
  }
  /* No view of their own (one shared screen): the game camera. */
  return active_camera;
}

void LN_UIService::SetPlayerPointer(const int32_t user,
                                    const bool enabled,
                                    KX_GameObject *surface,
                                    KX_GameObject *cursor,
                                    const std::string &move_action,
                                    const float speed,
                                    const std::string &press_action)
{
  if (user < 0 || user >= LN_MAX_GAMEPADS) {
    return;
  }
  PlayerPointer &pointer = m_playerPointers[size_t(user)];
  /* Off: the next UI frame hides the cursor and cancels what the pointer held. Only a surface
   * pointer: one node may turn it off while another turns View Center on, in either order. */
  if (!enabled || surface == nullptr) {
    if (!pointer.aim) {
      pointer.enabled = false;
    }
    return;
  }
  /* Another cursor: the old one hides, the new one shows. */
  KX_GameObject *old_cursor = m_manager.ResolveObjectHandle(pointer.cursor);
  if (old_cursor != cursor) {
    if (old_cursor != nullptr && pointer.cursor_shown) {
      old_cursor->SetVisible(false, true);
    }
    pointer.cursor_shown = false;
  }
  if (!pointer.enabled || pointer.aim || m_manager.ResolveObjectHandle(pointer.surface) != surface)
  {
    /* A new surface: start in its middle, player 0 with the mouse. */
    pointer.placed = false;
    pointer.mouse_mode = user == 0;
  }
  pointer.mode_changed = pointer.mode_changed || pointer.aim;
  pointer.enabled = true;
  pointer.aim = false;
  pointer.aim_blocked = false;
  pointer.surface = m_manager.MakeObjectHandle(surface, surface->GetName());
  pointer.cursor = cursor != nullptr ? m_manager.MakeObjectHandle(cursor, cursor->GetName()) :
                                       LN_ObjectHandle();
  if (pointer.move_action != move_action) {
    pointer.warned_action = false;
  }
  pointer.move_action = move_action;
  pointer.speed = std::isfinite(speed) ? std::max(0.0f, speed) : 1.0f;
  if (pointer.press_action != press_action) {
    pointer.warned_press_action = false;
  }
  pointer.press_action = press_action;
}

void LN_UIService::SetPlayerAimPointer(const int32_t user,
                                       const bool enabled,
                                       const float reach,
                                       const std::string &press_action)
{
  if (user < 0 || user >= LN_MAX_GAMEPADS) {
    return;
  }
  PlayerPointer &pointer = m_playerPointers[size_t(user)];
  /* Off: only a View Center pointer (see SetPlayerPointer). */
  if (!enabled) {
    if (pointer.aim) {
      pointer.enabled = false;
    }
    return;
  }
  /* From a surface (also one turned off in this flush): its cursor hides and the next UI frame
   * lets go of what it held. */
  if (!pointer.aim) {
    if (KX_GameObject *old_cursor = m_manager.ResolveObjectHandle(pointer.cursor)) {
      if (pointer.cursor_shown) {
        old_cursor->SetVisible(false, true);
      }
    }
    pointer.cursor_shown = false;
    pointer.placed = false;
    pointer.aim_blocked = false;
    pointer.mode_changed = true;
  }
  pointer.enabled = true;
  pointer.aim = true;
  pointer.mouse_mode = false;
  pointer.surface = LN_ObjectHandle();
  pointer.cursor = LN_ObjectHandle();
  pointer.reach = std::isfinite(reach) ? std::max(0.0f, reach) : 3.0f;
  if (pointer.press_action != press_action) {
    pointer.warned_press_action = false;
  }
  pointer.press_action = press_action;
}

int32_t LN_UIService::PointerButtonAction(const int32_t user)
{
  const LN_InputMap &map = LN_InputMap::Get();
  if (user < 0 || user >= LN_MAX_GAMEPADS) {
    return -1;
  }
  PlayerPointer &pointer = m_playerPointers[size_t(user)];
  if (pointer.press_action.empty()) {
    return map.FindBuiltin(LN_UIAction::Accept);
  }
  const int32_t action = map.Find(pointer.press_action);
  if (action < 0 && !pointer.warned_press_action) {
    pointer.warned_press_action = true;
    CM_Warning("Set Player Pointer: the Game Input Map has no action \""
               << pointer.press_action << "\" to press with the pointer of player " << user);
  }
  return action;
}

bool LN_UIService::PointerButtonState(const int32_t user, const LN_UIActionState state)
{
  /* View Center does not point with the mouse: its mouse buttons count too. */
  const BindingFilter filter = AimPointerActive(user) ? BindingFilter::All :
                                                        BindingFilter::NotMouse;
  return ActionIndexBindingState(PointerButtonAction(user), state, user, nullptr, filter);
}

void LN_UIService::ConsumePointerButton(const int32_t user, const LN_InputSnapshot &input)
{
  const BindingFilter filter = AimPointerActive(user) ? BindingFilter::All :
                                                        BindingFilter::NotMouse;
  ConsumeActionIndexPresses(PointerButtonAction(user), user, input, filter, true);
}

bool LN_UIService::UIUsesPlayerInput(const int32_t user) const
{
  if (user < 0 || user >= LN_MAX_GAMEPADS) {
    return false;
  }
  return InputMenuIndex(user) >= 0 || HoldsControl(user) ||
         (PointerActive(user) && !m_playerPointers[size_t(user)].aim);
}

void LN_UIService::SetCursorOnlyInMenus(const bool on)
{
  m_cursorOnlyInMenus = on;
  m_menuCursorShown = -1;
  UpdateMenuCursor();
}

void LN_UIService::UpdateMenuCursor()
{
  if (!m_cursorOnlyInMenus) {
    return;
  }
  /* Player 0's mouse is the UI's in a menu that takes their input (a HUD does not), and at a UI
   * in the game world they took (Take UI Control) unless a Set Player Pointer cursor there shows
   * where they point. */
  const bool shown = InputMenuIndex(0) >= 0 || (HoldsControl(0) && !PointerActive(0));
  if (int(shown) == m_menuCursorShown) {
    return;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_ICanvas *canvas = engine != nullptr ? engine->GetCanvas() : nullptr;
  if (canvas == nullptr) {
    return;
  }
  canvas->SetMouseState(shown ? RAS_ICanvas::MOUSE_NORMAL : RAS_ICanvas::MOUSE_INVISIBLE);
  m_menuCursorShown = int(shown);
}

std::vector<KX_GameObject *> LN_UIService::ShownPointerCursors() const
{
  std::vector<KX_GameObject *> cursors;
  for (const PlayerPointer &pointer : m_playerPointers) {
    if (pointer.cursor_shown) {
      if (KX_GameObject *cursor = m_manager.ResolveObjectHandle(pointer.cursor)) {
        cursors.push_back(cursor);
      }
    }
  }
  return cursors;
}

bool LN_UIService::GetPlayerPointerPoint(const int32_t user, MT_Vector3 &r_point) const
{
  if (user < 0 || user >= LN_MAX_GAMEPADS) {
    return false;
  }
  const PlayerPointer &pointer = m_playerPointers[size_t(user)];
  if (!pointer.enabled || !pointer.placed ||
      (!pointer.aim && m_manager.ResolveObjectHandle(pointer.surface) == nullptr))
  {
    return false;
  }
  r_point = pointer.point;
  return true;
}

MT_Vector3 LN_UIService::PointerMove(const int32_t user, PlayerPointer &pointer)
{
  MT_Vector3 move(0.0f, 0.0f, 0.0f);
  if (!pointer.move_action.empty()) {
    const int32_t action = LN_InputMap::Get().Find(pointer.move_action);
    if (action < 0) {
      if (!pointer.warned_action) {
        pointer.warned_action = true;
        CM_Warning("Set Player Pointer: the Game Input Map has no action \""
                   << pointer.move_action << "\" to move the pointer of player " << user);
      }
      return move;
    }
    const LN_InputActionTracker *actions = m_manager.FrameInputActions();
    if (const LN_InputActionState *state = actions != nullptr ? actions->Find(action, user) :
                                                                nullptr)
    {
      move = state->vector;
    }
  }
  else if (m_input != nullptr) {
    const LN_GamepadSnapshot *gamepad = m_input->GetGamepad(
        LN_InputMap::Get().GamepadOfUser(user));
    if (gamepad != nullptr && gamepad->connected) {
      /* The left stick, without the drift of a stick at rest. */
      constexpr float dead_zone = 0.15f;
      move = LN_GamepadStickPosition(*gamepad, 1);
      const float length = move.length();
      move = length > dead_zone ? move * ((std::min(length, 1.0f) - dead_zone) /
                                          ((1.0f - dead_zone) * length)) :
                                  MT_Vector3(0.0f, 0.0f, 0.0f);
    }
  }
  move.setValue(move.x(), move.y(), 0.0f);
  const float length = move.length();
  if (!IsFinite(move) || length <= 0.0f) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  /* Slow near the middle for aiming at small buttons, full speed at full tilt. */
  return move / length * std::min(length * length, 1.0f);
}

void LN_UIService::UpdatePlayerPointers()
{
  if (m_pointersFrame == m_frameIndex) {
    return;
  }
  m_pointersFrame = m_frameIndex;
  const size_t phase = PhaseIndex(m_event);
  const float delta_time = float(std::clamp(m_frameDelta, 0.0, 0.1));
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    PlayerPointer &pointer = m_playerPointers[size_t(user)];
    const bool was_active = pointer.active && !pointer.mode_changed;
    if (pointer.mode_changed && pointer.active) {
      /* From a surface to View Center or back: what it held is let go, as a cancel. */
      CancelPointer(phase, user, true);
    }
    pointer.mode_changed = false;
    pointer.active = false;
    if (pointer.aim) {
      /* View Center: from the player's camera, straight ahead as far as Reach. A menu that takes
       * the player's keys and gamepad has them, and a paused game world takes no presses: it
       * waits. */
      KX_Camera *camera = pointer.enabled ? PlayerViewCamera(user) : nullptr;
      if (camera != nullptr && camera->GetScene() == &m_scene &&
          !m_manager.IsGameObjectQueuedForRemoval(camera))
      {
        const MT_Vector3 from = camera->NodeGetWorldPosition();
        MT_Vector3 forward = -camera->NodeGetWorldOrientation().getColumn(2);
        if (IsFinite(from) && IsFinite(forward) && forward.length2() > kScaleEpsilon) {
          forward.normalize();
          pointer.ray_from = from;
          pointer.ray_to = from + forward * pointer.reach;
          pointer.point = pointer.ray_to;
          pointer.placed = true;
          /* It never moves on a surface: a press never becomes a drag. */
          pointer.position = MT_Vector3(0.0f, 0.0f, 0.0f);
          pointer.delta = MT_Vector3(0.0f, 0.0f, 0.0f);
          const KX_KetsjiEngine *engine = KX_GetActiveEngine();
          pointer.active = InputMenuIndex(user) < 0 &&
                           !(engine != nullptr && engine->IsGamePaused());
        }
      }
      if (!pointer.active) {
        pointer.aim_blocked = false;
        if (!pointer.enabled) {
          pointer.placed = false;
        }
      }
      if (was_active && !pointer.active) {
        CancelPointer(phase, user, true);
      }
      else if (!was_active && pointer.active) {
        /* A Grab carry of the player's keys and gamepad ends: the pointer takes their buttons. */
        const auto carry = m_pointerStates[phase].find(user);
        if (carry != m_pointerStates[phase].end() && carry->second.carried) {
          CancelPointer(phase, user, true);
        }
      }
      continue;
    }
    KX_GameObject *cursor = m_manager.ResolveObjectHandle(pointer.cursor);
    KX_GameObject *surface = pointer.enabled ? m_manager.ResolveObjectHandle(pointer.surface) :
                                               nullptr;
    KX_Camera *camera = surface != nullptr ? PlayerViewCamera(user) : nullptr;
    MT_Vector3 bounds_min;
    MT_Vector3 bounds_max;
    bool valid = surface != nullptr && camera != nullptr && surface->GetScene() == &m_scene &&
                 !m_manager.IsGameObjectQueuedForRemoval(surface) &&
                 LocalBounds(surface, bounds_min, bounds_max);

    /* The surface: its thinnest side is the one it faces; the face toward the player's camera is
     * the front, the other two axes the plane the pointer moves on. */
    const MT_Vector3 origin = valid ? surface->NodeGetWorldPosition() : MT_Vector3(0, 0, 0);
    const MT_Matrix3x3 orientation = valid ? surface->NodeGetWorldOrientation() :
                                             MT_Matrix3x3::Identity();
    const MT_Vector3 scale = valid ? surface->NodeGetWorldScaling() : MT_Vector3(1, 1, 1);
    const auto local_to_world = [&](const MT_Vector3 &local) {
      return origin + orientation * MT_Vector3(local.x() * scale.x(),
                                               local.y() * scale.y(),
                                               local.z() * scale.z());
    };
    int normal_axis = 2;
    int u_axis = 0;
    int v_axis = 1;
    MT_Vector3 front(0.0f, 0.0f, 1.0f);
    float size = 0.0f;
    if (valid) {
      const MT_Vector3 center = local_to_world((bounds_min + bounds_max) * 0.5f);
      MT_Vector3 to_camera = camera->NodeGetWorldPosition() - center;
      if (to_camera.length2() > kScaleEpsilon) {
        to_camera.normalize();
      }
      MT_Vector3 extent;
      float largest = 0.0f;
      for (int axis = 0; axis < 3; ++axis) {
        extent[axis] = (bounds_max[axis] - bounds_min[axis]) * std::abs(scale[axis]);
        largest = std::max(largest, float(extent[axis]));
      }
      const auto facing = [&](const int axis) {
        return std::abs(orientation.getColumn(axis).dot(to_camera));
      };
      normal_axis = 0;
      for (int axis = 1; axis < 3; ++axis) {
        const float difference = extent[axis] - extent[normal_axis];
        const float tie = largest * 1.0e-3f;
        if (difference < -tie || (std::abs(difference) <= tie && facing(axis) > facing(normal_axis)))
        {
          normal_axis = axis;
        }
      }
      u_axis = normal_axis == 0 ? 1 : 0;
      v_axis = normal_axis == 2 ? 1 : 2;
      size = std::max(extent[u_axis], extent[v_axis]);
      valid = IsFinite(extent) && extent[u_axis] > kScaleEpsilon &&
              extent[v_axis] > kScaleEpsilon && std::abs(scale[u_axis]) > kScaleEpsilon &&
              std::abs(scale[v_axis]) > kScaleEpsilon;
      MT_Vector3 normal_direction = orientation.getColumn(normal_axis) *
                                    (scale[normal_axis] < 0.0f ? -1.0f : 1.0f);
      if (normal_direction.length2() > kScaleEpsilon) {
        normal_direction.normalize();
      }
      front = normal_direction.dot(to_camera) >= 0.0f ? normal_direction : -normal_direction;
    }

    if (!valid) {
      if (cursor != nullptr && pointer.cursor_shown) {
        cursor->SetVisible(false, true);
        pointer.cursor_shown = false;
      }
      if (!pointer.enabled) {
        pointer.placed = false;
      }
    }
    else {
      const bool front_is_max = orientation.getColumn(normal_axis).dot(front) *
                                    (scale[normal_axis] < 0.0f ? -1.0f : 1.0f) >=
                                0.0f;
      const float face = front_is_max ? bounds_max[normal_axis] : bounds_min[normal_axis];
      if (!pointer.placed) {
        pointer.local_u = (bounds_min[u_axis] + bounds_max[u_axis]) * 0.5f;
        pointer.local_v = (bounds_min[v_axis] + bounds_max[v_axis]) * 0.5f;
        pointer.placed = true;
      }
      /* A menu that takes the player's keys and gamepad has them; the pointer waits where it
       * is. */
      const bool in_menu = InputMenuIndex(user) >= 0;
      const MT_Vector3 move = in_menu ? MT_Vector3(0.0f, 0.0f, 0.0f) : PointerMove(user, pointer);
      /* Player 0: the mouse points until the stick moves the pointer, and the other way around
       * (never in the middle of a press). A press of its keys or gamepad (Enter, A) clicks under
       * the cursor where the mouse left it. */
      const auto held = m_pointerStates[phase].find(user);
      const bool pressing = held != m_pointerStates[phase].end() && held->second.has_capture;
      const LN_MouseSnapshot *mouse = user == 0 && m_input != nullptr && !in_menu ?
                                          &m_input->GetMouse() :
                                          nullptr;
      if (mouse != nullptr && !pressing) {
        if (move.length2() > 0.0f ||
            (!m_editingText.IsValid() && PointerButtonState(user, LN_UIActionState::Triggered)))
        {
          pointer.mouse_mode = false;
        }
        else if (mouse->has_position &&
                 (std::abs(mouse->delta_x) + std::abs(mouse->delta_y) > 1 ||
                  mouse->wheel_delta != 0 ||
                  m_input->GetStatus(SCA_IInputDevice::LEFTMOUSE, SCA_InputEvent::JUSTACTIVATED) ||
                  m_input->GetStatus(SCA_IInputDevice::RIGHTMOUSE, SCA_InputEvent::JUSTACTIVATED)))
        {
          pointer.mouse_mode = true;
        }
      }
      if (user != 0) {
        pointer.mouse_mode = false;
      }
      if (pointer.mouse_mode && !in_menu) {
        /* Where the mouse points on the surface's plane. */
        KX_Camera *mouse_camera = nullptr;
        MT_Vector3 from;
        MT_Vector3 to;
        bool has_ray = false;
        if (mouse != nullptr && mouse->has_position) {
          const float x = float(mouse->x);
          const float y = float(mouse->y);
          if (m_pointerRayForTests) {
            mouse_camera = camera;
            has_ray = m_pointerRayForTests(mouse_camera, x, y, from, to);
          }
          else {
            mouse_camera = WorldPointerCamera(x, y);
            has_ray = mouse_camera != nullptr && mouse_camera->GetScene() == &m_scene &&
                      LN_ComputeCameraPointerRay(mouse_camera,
                                                 x,
                                                 y,
                                                 std::max(mouse_camera->GetCameraFar(), 0.0f),
                                                 from,
                                                 to,
                                                 HoldsPointerInView(mouse_camera));
          }
        }
        MT_Vector3 face_point = (bounds_min + bounds_max) * 0.5f;
        face_point[normal_axis] = face;
        const MT_Vector3 plane_point = local_to_world(face_point);
        const MT_Vector3 direction = to - from;
        const float denominator = direction.dot(front);
        if (has_ray && std::abs(denominator) > kScaleEpsilon) {
          const float t = (plane_point - from).dot(front) / denominator;
          const MT_Vector3 hit = from + direction * t;
          if (t >= 0.0f && IsFinite(hit)) {
            const MT_Vector3 local = orientation.transposed() * (hit - origin);
            pointer.local_u = local[u_axis] / scale[u_axis];
            pointer.local_v = local[v_axis] / scale[v_axis];
          }
        }
      }
      else if (move.length2() > 0.0f) {
        /* The stick moves it as the player sees the surface: right is their camera's right. */
        const MT_Matrix3x3 view = camera->NodeGetWorldOrientation();
        MT_Vector3 world_delta = (view.getColumn(0) * move.x() + view.getColumn(1) * move.y()) *
                                 (pointer.speed * size * delta_time);
        world_delta -= front * world_delta.dot(front);
        pointer.local_u += world_delta.dot(orientation.getColumn(u_axis)) / scale[u_axis];
        pointer.local_v += world_delta.dot(orientation.getColumn(v_axis)) / scale[v_axis];
      }
      pointer.local_u = std::clamp(
          pointer.local_u, float(bounds_min[u_axis]), float(bounds_max[u_axis]));
      pointer.local_v = std::clamp(
          pointer.local_v, float(bounds_min[v_axis]), float(bounds_max[v_axis]));
      if (!std::isfinite(pointer.local_u) || !std::isfinite(pointer.local_v)) {
        pointer.placed = false;
        pointer.local_u = (bounds_min[u_axis] + bounds_max[u_axis]) * 0.5f;
        pointer.local_v = (bounds_min[v_axis] + bounds_max[v_axis]) * 0.5f;
      }
      MT_Vector3 local_point;
      local_point[normal_axis] = face;
      local_point[u_axis] = pointer.local_u;
      local_point[v_axis] = pointer.local_v;
      pointer.point = local_to_world(local_point);

      /* The cursor floats just in front of everything shown on the surface (its buttons and
       * labels), and the pointer's ray starts in front of the cursor. */
      const float margin = std::max(size * 0.002f, 1.0e-4f);
      float lift = 0.0f;
      /* All descendants: a child with a parent inverse hangs from a scene graph node of its
       * own (GetChildrenRecursive walks through it). */
      for (KX_GameObject *child : surface->GetChildrenRecursive()) {
        MT_Vector3 child_min;
        MT_Vector3 child_max;
        if (child == nullptr || child == cursor || IsDescendantOf(child, cursor) ||
            !child->GetVisible() || !LocalBounds(child, child_min, child_max))
        {
          continue;
        }
        const MT_Vector3 child_origin = child->NodeGetWorldPosition();
        const MT_Matrix3x3 child_orientation = child->NodeGetWorldOrientation();
        const MT_Vector3 child_scale = child->NodeGetWorldScaling();
        for (int corner = 0; corner < 8; ++corner) {
          const MT_Vector3 local((corner & 1) ? child_max.x() : child_min.x(),
                                 (corner & 2) ? child_max.y() : child_min.y(),
                                 (corner & 4) ? child_max.z() : child_min.z());
          const MT_Vector3 world = child_origin +
                                   child_orientation * MT_Vector3(local.x() * child_scale.x(),
                                                                  local.y() * child_scale.y(),
                                                                  local.z() * child_scale.z());
          if (IsFinite(world)) {
            lift = std::max(lift, float((world - pointer.point).dot(front)));
          }
        }
      }
      lift = std::min(lift, size) + margin;
      /* While a press is held its depth stays: a dragged element (a child of the surface) or a
       * press look must not move the pointer toward the camera. */
      if (pressing && pointer.lift > 0.0f) {
        lift = pointer.lift;
      }
      pointer.lift = lift;
      pointer.ray_from = pointer.point + front * (lift + margin);
      pointer.ray_to = pointer.point - front * margin;
      if (cursor != nullptr && cursor->GetScene() == &m_scene &&
          !m_manager.IsGameObjectQueuedForRemoval(cursor))
      {
        cursor->NodeSetWorldPosition(pointer.point + front * lift);
        if (!pointer.cursor_shown) {
          cursor->SetVisible(true, true);
          pointer.cursor_shown = true;
        }
      }
      /* The drag threshold's pixels: 1000 across the surface's width. */
      const MT_Vector3 position((pointer.local_u - bounds_min[u_axis]) *
                                    std::abs(scale[u_axis]) / size * 1000.0f,
                                (bounds_max[v_axis] - pointer.local_v) *
                                    std::abs(scale[v_axis]) / size * 1000.0f,
                                0.0f);
      pointer.delta = was_active ? position - pointer.position : MT_Vector3(0.0f, 0.0f, 0.0f);
      pointer.position = position;
      pointer.active = !in_menu;
    }

    if (was_active && !pointer.active) {
      /* Off, gone or behind a menu: what it held is let go, as a cancel. */
      CancelPointer(phase, user, true);
    }
    else if (!was_active && pointer.active) {
      /* A Grab carry of the player's keys and gamepad ends: the pointer takes their buttons. */
      const auto carry = m_pointerStates[phase].find(user);
      if (carry != m_pointerStates[phase].end() && carry->second.carried) {
        CancelPointer(phase, user, true);
      }
    }
  }
}

std::vector<KX_GameObject *> LN_UIService::TemplateSlots(const ViewRegistration &view)
{
  /* Keeps a typo in Rows or Columns from freezing the game. */
  constexpr int64_t max_slots = 1024;
  KX_GameObject *slot_template = view.slot_template;
  const int32_t columns = std::max(1, view.columns);
  const size_t count = size_t(std::min<int64_t>(int64_t(view.rows) * columns, max_slots));
  ViewPersistentState &persistent = m_viewStates[view.listener];
  std::vector<KX_GameObject *> slots{slot_template};
  slots.reserve(count);
  bool reuse = m_manager.ResolveObjectHandle(persistent.slot_template) == slot_template &&
               persistent.template_copies.size() + 1 == count &&
               persistent.template_columns == columns &&
               persistent.template_rows == view.rows &&
               persistent.template_horizontal == view.horizontal &&
               persistent.template_spacing == view.spacing &&
               persistent.template_overlay == m_scene.IsOverlayRuntimeObject(slot_template);
  for (size_t index = 0; reuse && index < persistent.template_copies.size(); ++index) {
    KX_GameObject *copy = m_manager.ResolveObjectHandle(persistent.template_copies[index]);
    reuse = copy != nullptr && !m_manager.IsGameObjectQueuedForRemoval(copy);
    slots.push_back(copy);
  }
  if (reuse) {
    return slots;
  }
  RemoveTemplateCopies(persistent);
  slots.resize(1);

  /* Copies go right along the template's X axis and down its Y axis, a template size apart with a
   * small gap unless Spacing says otherwise. Slot order is item order: row by row, or column by
   * column for a horizontal grid. */
  const size_t rows = size_t(std::max(1, view.rows));
  MT_Vector3 size(1.0f, 1.0f, 1.0f);
  MT_Vector3 bounds_min;
  MT_Vector3 bounds_max;
  if (ObjectBounds(slot_template, bounds_min, bounds_max)) {
    size = bounds_max - bounds_min;
  }
  /* In world space: a template's own transform is measured from Blender's parent inverse (a hidden
   * node between it and its parent), which its copies, parented straight to that parent, do not
   * have. Spacing is in the parents' units, so a menu scaled up spreads its grid with it; the
   * automatic gap is the template's own size. Without a parent, both are the same numbers. */
  const MT_Vector3 local_scale = slot_template->NodeGetLocalScaling();
  const MT_Vector3 scale = slot_template->NodeGetWorldScaling();
  const MT_Vector3 position = slot_template->NodeGetWorldPosition();
  const MT_Matrix3x3 orientation = slot_template->NodeGetWorldOrientation();
  const auto step = [&](const int axis, const float spacing) {
    if (spacing == 0.0f) {
      return orientation.getColumn(axis) * (std::abs(size[axis] * scale[axis]) * 1.1f);
    }
    const float parents = std::abs(local_scale[axis]) > kScaleEpsilon ?
                              std::abs(scale[axis] / local_scale[axis]) :
                              1.0f;
    return orientation.getColumn(axis) * (spacing * parents);
  };
  const MT_Vector3 step_x = step(0, view.spacing.x());
  const MT_Vector3 step_y = step(1, view.spacing.y());
  KX_GameObject *parent = slot_template->GetParent();
  for (size_t index = 1; index < count; ++index) {
    std::vector<KX_GameObject *> members;
    std::unordered_map<KX_GameObject *, KX_GameObject *> mapping;
    KX_GameObject *copy = m_scene.AddReplicaObject(
        slot_template, slot_template, 0.0f, &members, nullptr, &mapping);
    if (copy == nullptr) {
      break;
    }
    /* A copy of a slot in an open menu belongs to that menu. */
    m_scene.AdoptOverlayReplica(copy, members, mapping);
    if (parent != nullptr) {
      copy->SetParent(parent, false, false);
    }
    copy->NodeSetWorldScale(scale);
    copy->NodeSetGlobalOrientation(orientation);
    const size_t column = view.horizontal ? index / rows : index % size_t(columns);
    const size_t row = view.horizontal ? index % rows : index / size_t(columns);
    copy->NodeSetWorldPosition(position + step_x * float(column) - step_y * float(row));
    copy->NodeUpdateGS(0.0);
    persistent.template_copies.push_back(m_manager.MakeObjectHandle(copy, copy->GetName()));
    slots.push_back(copy);
    /* AddReplicaObject's reference: the scene owns the copy. */
    copy->Release();
  }
  persistent.slot_template = m_manager.MakeObjectHandle(slot_template, slot_template->GetName());
  persistent.template_columns = columns;
  persistent.template_rows = view.rows;
  persistent.template_horizontal = view.horizontal;
  persistent.template_spacing = view.spacing;
  persistent.template_overlay = m_scene.IsOverlayRuntimeObject(slot_template);
  /* From this frame on the copies are UI Elements and members of the template's menu. */
  RefreshElementRegistry();
  for (auto &item : m_sessions) {
    RefreshSessionMembership(item.second);
  }
  InvalidateMenuIndex();
  return slots;
}

void LN_UIService::RemoveTemplateCopies(ViewPersistentState &persistent)
{
  /* Only queued: removing an object notifies this service, which can release views again. */
  m_templateCopiesToRemove.insert(m_templateCopiesToRemove.end(),
                                  persistent.template_copies.begin(),
                                  persistent.template_copies.end());
  persistent.template_copies.clear();
  persistent.slot_template = LN_ObjectHandle();
}

bool LN_UIService::IsSlotPart(const ElementRecord *record,
                              KX_GameObject *slot,
                              KX_GameObject *child)
{
  if (record == nullptr) {
    return false;
  }
  const ElementSettings &settings = record->settings;
  for (KX_GameObject *item = child; item != nullptr && item != slot; item = item->GetParent()) {
    for (const blender::Object *part : {settings.highlight,
                                        settings.refused,
                                        settings.tooltip,
                                        settings.widget_part,
                                        settings.widget_object})
    {
      if (part != nullptr && item->GetName() == part->id.name + 2) {
        return true;
      }
    }
  }
  return false;
}

void LN_UIService::SlotParts(KX_GameObject *slot,
                             KX_GameObject *&r_icon,
                             KX_GameObject *&r_text,
                             std::set<KX_GameObject *> *r_picture)
{
  r_icon = nullptr;
  r_text = nullptr;
  if (r_picture != nullptr) {
    r_picture->insert(slot);
  }
  const ElementRecord *record = FindElementRecord(slot);
  /* A UI Element inside the slot (a button on it) is no part of its picture either. */
  const auto inside_element = [&](KX_GameObject *child) {
    for (KX_GameObject *item = child; item != nullptr && item != slot; item = item->GetParent()) {
      const blender::Object *source = item->GetBlenderObject();
      if (source != nullptr && (source->game_ui.flag & blender::OB_UI_ELEMENT) != 0) {
        return true;
      }
    }
    return false;
  };
  for (KX_GameObject *child : slot->GetChildrenRecursive()) {
    if (child == nullptr || m_manager.IsGameObjectQueuedForRemoval(child)) {
      continue;
    }
    const bool part = IsSlotPart(record, slot, child);
    if (r_picture != nullptr && !part && !inside_element(child)) {
      r_picture->insert(child);
    }
    if ((r_text == nullptr || r_icon == nullptr) && part) {
      continue;
    }
    if (child->GetGameObjectType() == SCA_IObject::OBJ_TEXT) {
      r_text = r_text ? r_text : child;
    }
    else if (r_icon == nullptr && child->GetMeshCount() > 0) {
      r_icon = child;
    }
  }
}

void LN_UIService::StartDragGhost(KX_GameObject *slot)
{
  const PointerState *drag = FrozenDragOf(slot);
  const ViewRegistration *view = GridOfSlot(slot);
  const ElementRecord *record = FindElementRecord(slot);
  if (m_dragGhosts.contains(slot) || drag == nullptr || drag->drag_amount <= 0 ||
      view == nullptr || record == nullptr)
  {
    return;
  }
  const auto place = std::find(view->slots.begin(), view->slots.end(), slot);
  if (place == view->slots.end()) {
    return;
  }
  KX_GameObject *icon = nullptr;
  KX_GameObject *text = nullptr;
  std::set<KX_GameObject *> picture;
  SlotParts(slot, icon, text, &picture);
  std::vector<KX_GameObject *> members;
  std::unordered_map<KX_GameObject *, KX_GameObject *> mapping;
  KX_GameObject *copy = m_makeDragGhostForTests ?
                            m_makeDragGhostForTests(slot) :
                            m_scene.AddReplicaObject(
                                slot, slot, 0.0f, &members, &picture, &mapping);
  if (copy == nullptr) {
    return;
  }
  if (!m_makeDragGhostForTests) {
    /* A copy of a slot of an open menu belongs to that menu (and that player's copy of it). */
    m_scene.AdoptOverlayReplica(copy, members, mapping);
    if (KX_GameObject *parent = slot->GetParent()) {
      copy->SetParent(parent, false, false);
    }
  }
  /* Where the slot is now, before it follows the pointer, without its hover look. */
  const float look = record->look_factor > kScaleEpsilon ? record->look_factor : 1.0f;
  copy->NodeSetWorldScale(slot->NodeGetWorldScaling() / look);
  copy->NodeSetGlobalOrientation(slot->NodeGetWorldOrientation());
  copy->NodeSetWorldPosition(slot->NodeGetWorldPosition());
  copy->NodeUpdateGS(0.0);
  DragGhost &ghost = m_dragGhosts[slot];
  ghost.slot = m_manager.MakeObjectHandle(slot, slot->GetName());
  ghost.ghost = m_manager.MakeObjectHandle(copy, copy->GetName());
  ghost.view = view->listener;
  ghost.slot_index = size_t(place - view->slots.begin());
  for (KX_GameObject *member : members) {
    ghost.members.push_back(member);
    m_dragGhostObjects.insert(member);
  }
  if (std::find(members.begin(), members.end(), copy) == members.end()) {
    ghost.members.push_back(copy);
    m_dragGhostObjects.insert(copy);
  }
  const auto copied = [&](KX_GameObject *object) -> KX_GameObject * {
    const auto item = object != nullptr ? mapping.find(object) : mapping.end();
    return item != mapping.end() ? item->second : nullptr;
  };
  if (KX_GameObject *copy_icon = copied(icon)) {
    ghost.icon = m_manager.MakeObjectHandle(copy_icon, copy_icon->GetName());
  }
  if (KX_GameObject *copy_text = copied(text)) {
    ghost.text = m_manager.MakeObjectHandle(copy_text, copy_text->GetName());
  }
  if (!m_makeDragGhostForTests) {
    /* AddReplicaObject's reference: the scene owns the copy. */
    copy->Release();
  }
  /* This frame already: the cell keeps what stays, the slot shows what it carries. */
  const auto binding = m_slotBindings.find(slot);
  if (binding != m_slotBindings.end()) {
    UpdateDragGhost(*view, ghost.slot_index, slot, binding->second.item, true, true);
    ShowSlotItem(*view,
                 m_viewStates[view->listener],
                 ghost.slot_index,
                 slot,
                 LN_InventoryEntryWithCount(drag->drag_item, drag->drag_amount),
                 icon,
                 text);
  }
}

bool LN_UIService::StartDragPicture(KX_GameObject *slot)
{
  if (const auto existing = m_dragGhosts.find(slot); existing != m_dragGhosts.end()) {
    return existing->second.picture;
  }
  const PointerState *drag = FrozenDragOf(slot);
  const ViewRegistration *view = GridOfSlot(slot);
  const ElementRecord *record = FindElementRecord(slot);
  if (drag == nullptr || view == nullptr || record == nullptr) {
    return false;
  }
  const auto place = std::find(view->slots.begin(), view->slots.end(), slot);
  if (place == view->slots.end()) {
    return false;
  }
  KX_GameObject *icon = nullptr;
  KX_GameObject *text = nullptr;
  SlotParts(slot, icon, text, nullptr);
  if (view->icon_library == nullptr) {
    icon = nullptr;
  }
  if (!view->show_counts && !view->show_names) {
    text = nullptr;
  }
  /* Nothing of the item to show: the whole slot follows. (Unit tests make the copy with their
   * hook: their slots have no icon or text.) */
  if (icon == nullptr && text == nullptr && !m_makeDragGhostForTests) {
    return false;
  }
  /* The slot carrying its icon and count, and what holds them between (the copy hides those). */
  std::set<KX_GameObject *> picture = {slot};
  for (KX_GameObject *part : {icon, text}) {
    for (KX_GameObject *item = part; item != nullptr && item != slot; item = item->GetParent()) {
      picture.insert(item);
    }
  }
  std::vector<KX_GameObject *> members;
  std::unordered_map<KX_GameObject *, KX_GameObject *> mapping;
  KX_GameObject *copy = m_makeDragGhostForTests ?
                            m_makeDragGhostForTests(slot) :
                            m_scene.AddReplicaObject(
                                slot, slot, 0.0f, &members, &picture, &mapping);
  if (copy == nullptr) {
    return false;
  }
  if (!m_makeDragGhostForTests) {
    /* A copy of a slot of an open menu belongs to that menu (and that player's copy of it). */
    m_scene.AdoptOverlayReplica(copy, members, mapping);
    if (KX_GameObject *parent = slot->GetParent()) {
      copy->SetParent(parent, false, false);
    }
  }
  /* Where the slot is now, at its hovered size (it is pressed now): the item lifts off there. */
  const float look = record->look_factor > kScaleEpsilon ? record->look_factor : 1.0f;
  copy->NodeSetWorldScale(slot->NodeGetWorldScaling() / look * record->settings.hover_scale);
  copy->NodeSetGlobalOrientation(slot->NodeGetWorldOrientation());
  copy->NodeSetWorldPosition(slot->NodeGetWorldPosition());
  copy->NodeUpdateGS(0.0);
  DragGhost &ghost = m_dragGhosts[slot];
  ghost.picture = true;
  ghost.slot = m_manager.MakeObjectHandle(slot, slot->GetName());
  ghost.ghost = m_manager.MakeObjectHandle(copy, copy->GetName());
  ghost.view = view->listener;
  ghost.slot_index = size_t(place - view->slots.begin());
  if (drag->captured_camera != nullptr) {
    ghost.camera = m_manager.MakeObjectHandle(drag->captured_camera,
                                              drag->captured_camera->GetName());
  }
  /* The slot's size: in front of any icon a cell holds (a round 3D icon stands out of its cell),
   * not far beyond. */
  MT_Vector3 bounds_min;
  MT_Vector3 bounds_max;
  if (ObjectBounds(slot, bounds_min, bounds_max)) {
    const MT_Vector3 size = (bounds_max - bounds_min) * slot->NodeGetWorldScaling();
    ghost.lift = std::max({std::abs(size.x()), std::abs(size.y()), std::abs(size.z())});
  }
  for (KX_GameObject *member : members) {
    ghost.members.push_back(member);
    m_dragGhostObjects.insert(member);
  }
  if (std::find(members.begin(), members.end(), copy) == members.end()) {
    ghost.members.push_back(copy);
    m_dragGhostObjects.insert(copy);
  }
  const auto copied = [&](KX_GameObject *object) -> KX_GameObject * {
    const auto item = object != nullptr ? mapping.find(object) : mapping.end();
    return item != mapping.end() ? item->second : nullptr;
  };
  KX_GameObject *copy_icon = copied(icon);
  KX_GameObject *copy_text = copied(text);
  if (copy_icon != nullptr) {
    ghost.icon = m_manager.MakeObjectHandle(copy_icon, copy_icon->GetName());
  }
  if (copy_text != nullptr) {
    ghost.text = m_manager.MakeObjectHandle(copy_text, copy_text->GetName());
  }
  if (!m_makeDragGhostForTests) {
    /* Only the item shows: the cell's copy (and what holds the icon and count) is hidden. */
    for (const auto &[source, made] : mapping) {
      if (source != icon && source != text && made != nullptr) {
        made->SetVisible(false, false);
      }
    }
    /* AddReplicaObject's reference: the scene owns the copy. */
    copy->Release();
  }
  /* It shows what it carries: part of a split stack, else all of it. */
  const LN_Value carried = drag->drag_amount > 0 ? LN_InventoryEntryWithCount(drag->drag_item,
                                                                              drag->drag_amount) :
                                                   drag->drag_item;
  if (KX_GameObject *hidden = ShowItemOn(*view, ghost.visual, copy, carried, copy_icon, copy_text))
  {
    hidden->SetVisible(false, true);
  }
  ghost.shown = carried;
  /* This frame already, the cell shows what is left; the grid keeps it so until the drag ends. */
  if (KX_GameObject *hidden = ShowSlotItem(*view,
                                           m_viewStates[view->listener],
                                           ghost.slot_index,
                                           slot,
                                           ItemLeftInCell(*drag),
                                           icon,
                                           text))
  {
    hidden->SetViewClipVisible(false);
  }
  return true;
}

KX_GameObject *LN_UIService::DragPictureOf(const KX_GameObject *slot) const
{
  const auto entry = m_dragGhosts.find(slot);
  if (entry == m_dragGhosts.end() || !entry->second.picture) {
    return nullptr;
  }
  KX_GameObject *copy = m_manager.ResolveObjectHandle(entry->second.ghost);
  return copy != nullptr && !m_manager.IsGameObjectQueuedForRemoval(copy) ? copy : nullptr;
}

void LN_UIService::PlaceDragPicture(const KX_GameObject *slot, const MT_Vector3 &position)
{
  KX_GameObject *copy = DragPictureOf(slot);
  if (copy == nullptr) {
    return;
  }
  const DragGhost &ghost = m_dragGhosts.at(slot);
  MT_Vector3 placed = position;
  KX_GameObject *camera_object = m_manager.ResolveObjectHandle(ghost.camera);
  KX_Camera *camera = dynamic_cast<KX_Camera *>(camera_object);
  if (camera != nullptr && camera->GetCameraData() != nullptr && ghost.lift > 0.0f) {
    /* Along the line it is seen on, so it stays under the pointer: to the eye in perspective,
     * back along the view axis in orthographic views. */
    MT_Vector3 toward = camera->GetCameraData()->m_perspective ?
                            camera->NodeGetWorldPosition() - position :
                            camera->NodeGetWorldOrientation().getColumn(2);
    if (IsFinite(toward) && toward.length2() > kScaleEpsilon) {
      placed += toward.normalized() * ghost.lift;
    }
  }
  copy->NodeSetWorldPosition(placed);
  copy->NodeUpdateGS(0.0f);
}

LN_Value LN_UIService::ItemLeftInCell(const PointerState &drag)
{
  std::string name;
  int32_t count = 0;
  if (drag.drag_amount > 0 && LN_InventoryEntry(drag.drag_item, name, count) &&
      count > drag.drag_amount)
  {
    return LN_InventoryEntryWithCount(drag.drag_item, count - drag.drag_amount);
  }
  return LN_Value();
}

void LN_UIService::UpdateDragGhost(const ViewRegistration &view,
                                   const size_t slot,
                                   KX_GameObject *object,
                                   const LN_Value &item,
                                   const bool dragged_cell,
                                   const bool active_cell)
{
  const auto entry = m_dragGhosts.find(object);
  if (entry == m_dragGhosts.end()) {
    return;
  }
  DragGhost &ghost = entry->second;
  KX_GameObject *copy = m_manager.ResolveObjectHandle(ghost.ghost);
  if (copy == nullptr || m_manager.IsGameObjectQueuedForRemoval(copy)) {
    return;
  }
  const PointerState *drag = FrozenDragOf(object);
  /* The cell of the dragged item keeps the rest of the stack; another cell (the grid scrolled
   * meanwhile) shows its own item. */
  LN_Value shown = item;
  std::string name;
  int32_t count = 0;
  if (dragged_cell && drag != nullptr && drag->drag_amount > 0 &&
      LN_InventoryEntry(item, name, count))
  {
    shown = LN_InventoryEntryWithCount(item, std::max(0, count - drag->drag_amount));
  }
  ghost.shown = shown;
  ghost.slot_index = slot;
  /* The slot's color (Object Info in its material) as it is now: an action the game plays on the
   * slot (a highlight fading out as the drag starts) shows on the cell too, instead of the copy
   * keeping the color the slot had when it was made. */
  copy->SetObjectColor(object->GetObjectColor());
  const bool visible = active_cell &&
                       (!view.cull_outside || PointInsideView(view, copy->NodeGetWorldPosition()));
  if (visible != ghost.visible) {
    copy->SetVisible(visible, true);
    ghost.visible = visible;
    ghost.icon_hidden = !visible;
  }
  KX_GameObject *icon = m_manager.ResolveObjectHandle(ghost.icon);
  KX_GameObject *text = m_manager.ResolveObjectHandle(ghost.text);
  KX_GameObject *hidden = ShowItemOn(view, ghost.visual, copy, shown, icon, text);
  /* An icon without a library mesh (an empty cell) hides; it shows again with an item. */
  if (icon != nullptr && visible && (hidden == icon) != ghost.icon_hidden) {
    icon->SetVisible(hidden != icon, true);
    ghost.icon_hidden = hidden == icon;
  }
}

void LN_UIService::EndDragGhost(KX_GameObject *slot)
{
  const auto entry = m_dragGhosts.find(slot);
  if (entry == m_dragGhosts.end()) {
    return;
  }
  DragGhost ghost = std::move(entry->second);
  m_dragGhosts.erase(entry);
  for (const KX_GameObject *member : ghost.members) {
    m_dragGhostObjects.erase(member);
  }
  /* Back in its cell, the slot shows what the copy showed until the grid binds it again. */
  const ViewRegistration *view = GridOfSlot(slot);
  if (view != nullptr && view->listener == ghost.view) {
    KX_GameObject *icon = nullptr;
    KX_GameObject *text = nullptr;
    SlotParts(slot, icon, text, nullptr);
    ShowSlotItem(*view, m_viewStates[view->listener], ghost.slot_index, slot, ghost.shown, icon, text);
  }
  RemoveDragGhost(ghost.ghost);
}

void LN_UIService::RemoveDragGhost(const LN_ObjectHandle &copy)
{
  if (m_removeDragGhostForTests) {
    if (KX_GameObject *object = m_manager.ResolveObjectHandle(copy)) {
      m_removeDragGhostForTests(object);
    }
    return;
  }
  m_templateCopiesToRemove.push_back(copy);
}

void LN_UIService::SweepDragGhosts()
{
  std::vector<KX_GameObject *> ended;
  for (const auto &[slot, ghost] : m_dragGhosts) {
    KX_GameObject *object = m_manager.ResolveObjectHandle(ghost.slot);
    const PointerState *drag = object == slot ? FrozenDragOf(object) : nullptr;
    /* A cell's copy is only for a split stack; the item's copy (Item Only) for any drag. */
    if (drag == nullptr || (!ghost.picture && drag->drag_amount <= 0)) {
      ended.push_back(const_cast<KX_GameObject *>(slot));
    }
  }
  for (KX_GameObject *slot : ended) {
    const auto entry = m_dragGhosts.find(slot);
    for (const KX_GameObject *member : entry->second.members) {
      m_dragGhostObjects.erase(member);
    }
    RemoveDragGhost(entry->second.ghost);
    m_dragGhosts.erase(entry);
  }
}

void LN_UIService::RemoveQueuedTemplateCopies()
{
  while (!m_templateCopiesToRemove.empty()) {
    for (const LN_ObjectHandle &handle : std::exchange(m_templateCopiesToRemove, {})) {
      KX_GameObject *copy = m_manager.ResolveObjectHandle(handle);
      if (copy != nullptr && !m_manager.IsGameObjectQueuedForRemoval(copy)) {
        m_scene.DelayedRemoveObject(copy);
      }
    }
  }
}

KX_GameObject *LN_UIService::ShowSlotItem(const ViewRegistration &view,
                                          ViewPersistentState &persistent,
                                          const size_t slot,
                                          KX_GameObject *object,
                                          const LN_Value &item,
                                          KX_GameObject *icon,
                                          KX_GameObject *text)
{
  if ((view.icon_library == nullptr || icon == nullptr) &&
      ((!view.show_counts && !view.show_names) || text == nullptr))
  {
    return nullptr;
  }
  if (persistent.slot_visuals.size() <= slot) {
    persistent.slot_visuals.resize(slot + 1);
  }
  return ShowItemOn(view, persistent.slot_visuals[slot], object, item, icon, text);
}

KX_GameObject *LN_UIService::ShowItemOn(const ViewRegistration &view,
                                        SlotVisual &shown,
                                        KX_GameObject *object,
                                        const LN_Value &item,
                                        KX_GameObject *icon,
                                        KX_GameObject *text)
{
  if ((view.icon_library == nullptr || icon == nullptr) &&
      ((!view.show_counts && !view.show_names) || text == nullptr))
  {
    return nullptr;
  }
  std::string name;
  int32_t count = 0;
  if (item.exists && item.type == LN_ValueType::ObjectRef) {
    name = item.reference_name;
    count = 1;
  }
  else if (!LN_InventoryShownEntry(item, name, count)) {
    name.clear();
    count = 0;
  }
  const uint8_t text_mode = uint8_t((view.show_counts ? 1 : 0) | (view.show_names ? 2 : 0));
  if (m_manager.ResolveObjectHandle(shown.slot) != object || shown.item != name ||
      shown.count != count || shown.text_mode != text_mode)
  {
    shown.slot = m_manager.MakeObjectHandle(object, object->GetName());
    shown.item = name;
    shown.count = count;
    shown.text_mode = text_mode;
    shown.icon_found = false;
    if (view.icon_library != nullptr && icon != nullptr) {
      KX_GameObject *source = LN_FindLibraryItem(m_scene, view.icon_library, name);
      RAS_MeshObject *mesh = source != nullptr && source->GetMeshCount() > 0 ? source->GetMesh(0) :
                                                                                nullptr;
      shown.icon_found = mesh != nullptr;
      if (mesh != nullptr && icon->GetMesh(0) != mesh) {
        m_scene.ReplaceMesh(icon, mesh, true, false);
      }
    }
    if ((view.show_counts || view.show_names) && text != nullptr) {
      std::string shown_text;
      if (view.show_names) {
        /* The name players see: the library object's "name" property, else the item itself (a
         * quest line, a dialogue choice). */
        KX_GameObject *source = view.icon_library != nullptr ?
                                    LN_FindLibraryItem(m_scene, view.icon_library, name) :
                                    nullptr;
        EXP_Value *display = source != nullptr ? source->GetProperty("name") : nullptr;
        shown_text = display != nullptr && !display->GetText().empty() ? display->GetText() : name;
        if (view.show_counts && count > 1) {
          shown_text += " x" + std::to_string(count);
        }
      }
      else if (count > 1) {
        shown_text = std::to_string(count);
      }
      /* Text objects show their "Text" property, as written: an item name is data, not a
       * text with {values}. */
      if (text->GetGameObjectType() == SCA_IObject::OBJ_TEXT) {
        static_cast<KX_FontObject *>(text)->SetLiteralText(shown_text);
      }
      EXP_StringValue *value = new EXP_StringValue(shown_text, "Text");
      text->SetProperty("Text", value);
      value->Release();
    }
  }
  return view.icon_library != nullptr && icon != nullptr && !shown.icon_found ? icon : nullptr;
}

bool LN_UIService::HasElements() const
{
  return !m_elements.empty();
}

LN_UIService::ElementRecord *LN_UIService::FindElementRecord(const KX_GameObject *object)
{
  const auto item = m_elementIndex.find(object);
  if (item == m_elementIndex.end()) {
    return nullptr;
  }
  ElementRecord &record = m_elements[item->second];
  return m_manager.ResolveObjectHandle(record.handle) == object ? &record : nullptr;
}

void LN_UIService::RemoveElementRecord(const KX_GameObject *object)
{
  const auto item = m_elementIndex.find(object);
  if (item == m_elementIndex.end()) {
    return;
  }
  const size_t index = item->second;
  /* A shown Highlight Object that outlives the element (shared, not a child) is hidden; another
   * element using it shows it again if that one is highlighted. */
  if (m_elements[index].highlight_shown) {
    KX_GameObject *frame = m_manager.ResolveObjectHandle(
        m_elements[index].highlight_object.handle);
    if (frame != nullptr && frame != object && !m_manager.IsGameObjectQueuedForRemoval(frame)) {
      frame->SetVisible(false, true);
    }
  }
  m_elementIndex.erase(item);
  if (index + 1 != m_elements.size()) {
    m_elements[index] = std::move(m_elements.back());
    m_elementIndex[m_elements[index].object] = index;
  }
  m_elements.pop_back();
}

void LN_UIService::RefreshElementRegistry()
{
  const uint64_t generation = m_scene.GetObjectListGeneration();
  if (generation == m_elementsGeneration) {
    return;
  }
  m_elementsGeneration = generation;

  std::vector<ElementRecord> records;
  EXP_ListValue<KX_GameObject> *objects = m_scene.GetObjectList();
  if (objects != nullptr) {
    for (KX_GameObject *object : *objects) {
      const blender::Object *blender_object = object != nullptr ? object->GetBlenderObject() :
                                                                  nullptr;
      /* A split drag's copy of a slot is only a picture of it (StartDragGhost). */
      if (blender_object == nullptr ||
          (blender_object->game_ui.flag & blender::OB_UI_ELEMENT) == 0 ||
          object->GetScene() != &m_scene || m_manager.IsGameObjectQueuedForRemoval(object) ||
          m_dragGhostObjects.contains(object))
      {
        continue;
      }
      if (ElementRecord *existing = FindElementRecord(object)) {
        records.push_back(std::move(*existing));
        continue;
      }
      ElementRecord record;
      record.object = object;
      record.handle = m_manager.MakeObjectHandle(object, object->GetName());
      record.settings = ElementSettingsFromObject(*blender_object);
      records.push_back(std::move(record));
    }
  }
  m_elements = std::move(records);
  m_elementIndex.clear();
  m_elementIndex.reserve(m_elements.size());
  for (size_t index = 0; index < m_elements.size(); ++index) {
    m_elementIndex.emplace(m_elements[index].object, index);
  }
}

/** The menu camera's frame over the whole window and over `user`'s split screen view; false
 * while the window is not split (a menu in a player's view then stays as authored). */
static bool player_view_frames(KX_Scene &scene,
                               const int32_t user,
                               KX_Camera *camera,
                               CameraFrame &r_window,
                               CameraFrame &r_view)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
  RAS_Rasterizer *rasterizer = engine ? engine->GetRasterizer() : nullptr;
  if (canvas == nullptr || rasterizer == nullptr || camera == nullptr) {
    return false;
  }
  const KX_PlayerViews &views = scene.GetPlayerViews();
  KX_Camera *active_camera = scene.GetActiveCamera();
  const RAS_Rect display_area = rasterizer->GetRenderArea(canvas,
                                                          RAS_Rasterizer::RAS_STEREO_LEFTEYE);
  /* Before the first frame the views have no window yet: the canvas is it. */
  const RAS_Rect window = views.GetWindow().GetWidth() > 0 ? views.GetWindow() : display_area;
  KX_PlayerViews::Area area;
  if (views.GetViewCount(active_camera) < 2 ||
      !views.GetPlayerArea(user, active_camera, window, area))
  {
    return false;
  }
  RAS_Rect camera_area;
  RAS_Rect viewport;
  engine->GetSceneViewport(&scene, camera, display_area, camera_area, viewport);
  const float zoom = camera->GetViewport() ? 1.0f : engine->GetCameraZoom(camera);
  if (!ComputeCameraFrame(
          scene.GetFramingType(), camera, camera_area, viewport, zoom, r_window))
  {
    return false;
  }
  const float width = r_window.right - r_window.left;
  const float height = r_window.top - r_window.bottom;
  if (!(width > 0.0f) || !(height > 0.0f)) {
    return false;
  }
  r_view = r_window;
  r_view.left = r_window.left + area.left * width;
  r_view.right = r_view.left + area.width * width;
  r_view.top = r_window.top - area.top * height;
  r_view.bottom = r_view.top - area.height * height;
  return true;
}

void LN_UIService::PlaceMenusInPlayerViews()
{
  for (auto &item : m_sessions) {
    if (item.second.in_player_view) {
      PlaceMenuInPlayerView(item.second);
    }
  }
}

void LN_UIService::PlaceMenuInPlayerView(MenuSession &session, const bool to_window)
{
  KX_Camera *camera = m_scene.GetOverlayCamera();
  if (!session.in_player_view || camera == nullptr) {
    return;
  }
  /* The whole window's frame maps into the player's view, scaled to fit and centred (identity
   * while the window is not split, or when the menu goes back to the window). */
  float target[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  CameraFrame window;
  CameraFrame view;
  if (!to_window && player_view_frames(m_scene, session.user, camera, window, view)) {
    target[0] = std::min((view.right - view.left) / (window.right - window.left),
                         (view.top - view.bottom) / (window.top - window.bottom));
    target[1] = 0.5f * (window.left + window.right);
    target[2] = 0.5f * (window.bottom + window.top);
    target[3] = 0.5f * (view.left + view.right);
    target[4] = 0.5f * (view.bottom + view.top);
  }
  const float *before = session.placement;
  const bool same = std::equal(target, target + 5, before, [](const float a, const float b) {
    return std::abs(a - b) <= 1.0e-6f;
  });
  if (same || !(target[0] > 0.0f) || !(before[0] > 0.0f)) {
    return;
  }
  /* Move the menu's top objects (children follow) by the change: whatever logic moved since the
   * last placement stays. Anchored UI Elements are placed by their anchor (ApplyElementAnchor). */
  std::unordered_set<KX_GameObject *> objects;
  for (const LN_ObjectHandle &handle : session.objects) {
    if (KX_GameObject *object = m_manager.ResolveObjectHandle(handle)) {
      objects.insert(object);
    }
  }
  /* A menu that just opened: its UI Elements are known from now on. */
  RefreshElementRegistry();
  /* Anchored UI Elements are placed by their anchor; one inside another anchored one is carried
   * by it (as in CollectElements). */
  std::vector<ElementRecord *> anchored_records;
  std::unordered_set<const KX_GameObject *> anchored;
  for (ElementRecord &record : m_elements) {
    if (record.settings.anchor != LN_UIElementAnchor::None && objects.contains(record.object) &&
        !HasAnchoredAncestor(record.object))
    {
      anchored_records.push_back(&record);
      anchored.insert(record.object);
    }
  }
  /* Anchored objects below a moved top object stay where they are (their size follows it):
   * their anchor places them, from where they were. */
  std::vector<std::pair<KX_GameObject *, MT_Vector3>> kept;
  for (KX_GameObject *object : objects) {
    if (anchored.contains(object) && object->GetParent() != nullptr &&
        objects.contains(object->GetParent()))
    {
      kept.emplace_back(object, object->NodeGetWorldPosition());
    }
  }
  const bool perspective = camera->GetCameraData()->m_perspective;
  const MT_Matrix4x4 world_to_camera(camera->GetWorldToCamera());
  const MT_Matrix4x4 camera_to_world(camera->GetCameraToWorld());
  /* Back to where a point was authored, then into its new place. */
  const auto place = [&](const MT_Vector3 &position, MT_Vector3 &r_position) {
    const MT_Vector4 local = world_to_camera *
                             MT_Vector4(position.x(), position.y(), position.z(), 1.0f);
    const float depth = perspective ? -float(local[2]) : 1.0f;
    if (!(depth > 1.0e-6f)) {
      return false;
    }
    const float authored_x = before[1] + (float(local[0]) / depth - before[3]) / before[0];
    const float authored_y = before[2] + (float(local[1]) / depth - before[4]) / before[0];
    const float x = target[3] + (authored_x - target[1]) * target[0];
    const float y = target[4] + (authored_y - target[2]) * target[0];
    const MT_Vector4 world = camera_to_world *
                             MT_Vector4(x * depth, y * depth, float(local[2]), 1.0f);
    if (std::fabs(world[3]) <= MT_EPSILON) {
      return false;
    }
    r_position = MT_Vector3(world[0] / world[3], world[1] / world[3], world[2] / world[3]);
    return IsFinite(r_position);
  };
  std::unordered_set<KX_GameObject *> moved;
  for (KX_GameObject *object : objects) {
    if ((object->GetParent() != nullptr && objects.contains(object->GetParent())) ||
        anchored.contains(object) || m_manager.IsGameObjectQueuedForRemoval(object))
    {
      continue;
    }
    MT_Vector3 position;
    if (!place(object->NodeGetWorldPosition(), position)) {
      continue;
    }
    object->NodeSetWorldScale(object->NodeGetWorldScaling() * (target[0] / before[0]));
    object->NodeSetWorldPosition(position);
    object->NodeUpdateGS(0.0);
    moved.insert(object);
  }
  for (const auto &[object, position] : kept) {
    object->NodeSetWorldPosition(position);
    object->NodeUpdateGS(0.0);
  }
  /* A Scroll View puts its content at its first place plus the scroll every frame: a content
   * without a parent moved here gets that place moved too (with a parent it is local). */
  for (auto &item : m_viewStates) {
    ViewPersistentState &state = item.second;
    KX_GameObject *content = state.initialized ? m_manager.ResolveObjectHandle(state.content) :
                                                 nullptr;
    MT_Vector3 base;
    if (content != nullptr && content->GetParent() == nullptr && moved.contains(content) &&
        place(state.base_local_position, base))
    {
      state.base_local_position = base;
    }
  }
  std::copy(target, target + 5, session.placement);
  /* Anchored now, so they never show a frame in the old place. */
  for (ElementRecord *record : anchored_records) {
    ApplyElementAnchor(*record, camera);
  }
}

void LN_UIService::ApplyElementAnchor(ElementRecord &record, KX_Camera *camera)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_ICanvas *canvas = engine ? engine->GetCanvas() : nullptr;
  RAS_Rasterizer *rasterizer = engine ? engine->GetRasterizer() : nullptr;
  if (canvas == nullptr || rasterizer == nullptr || camera == nullptr) {
    return;
  }
  const RAS_Rect display_area = rasterizer->GetRenderArea(canvas,
                                                          RAS_Rasterizer::RAS_STEREO_LEFTEYE);
  RAS_Rect camera_area;
  RAS_Rect viewport;
  engine->GetSceneViewport(&m_scene, camera, display_area, camera_area, viewport);
  /* Same zoom rule as KX_KetsjiEngine::GetCameraProjectionMatrix. */
  const float zoom = camera->GetViewport() ? 1.0f : engine->GetCameraZoom(camera);
  const RAS_FrameSettings &framing = m_scene.GetFramingType();
  CameraFrame current;
  if (!ComputeCameraFrame(framing, camera, camera_area, viewport, zoom, current)) {
    return;
  }

  KX_GameObject *object = record.object;
  const LN_UIElementAnchor anchor = record.settings.anchor;
  /* In a menu opened in its player's view (split screen): the anchor's frame is that view, and a
   * menu's top object is resized with it, like the rest of the menu (PlaceMenuInPlayerView). */
  const MenuSession *placed_menu = nullptr;
  bool placed_root = false;
  /* The whole window's frame height (its anchors measure in it, as in any menu). */
  float window_height = current.top - current.bottom;
  if (const int menu_index = MenuStackIndexForObject(object);
      menu_index >= 0 && size_t(menu_index) < m_menuStack.size())
  {
    const auto session = m_sessions.find(m_menuStack[size_t(menu_index)]);
    if (session != m_sessions.end() && session->second.in_player_view) {
      placed_menu = &session->second;
      placed_root = object->GetParent() == nullptr ||
                    MenuStackIndexForObject(object->GetParent()) != menu_index;
      CameraFrame window;
      CameraFrame view;
      if (player_view_frames(m_scene, placed_menu->user, camera, window, view)) {
        current = view;
        window_height = window.top - window.bottom;
      }
    }
  }
  if (!record.anchor_initialized) {
    /* The authored position belongs to the frame the author saw: the camera view at the scene's
     * render aspect. The render size is also the design aspect here, since embedded play designs
     * the scene framing around the editor canvas instead. */
    const blender::Scene *blender_scene = m_scene.GetBlenderScene();
    const int reference_width = blender_scene != nullptr ?
                                    int(std::lround(blender_scene->r.xsch *
                                                    blender_scene->r.xasp)) :
                                    viewport.GetWidth();
    const int reference_height = blender_scene != nullptr ?
                                     int(std::lround(blender_scene->r.ysch *
                                                     blender_scene->r.yasp)) :
                                     viewport.GetHeight();
    const RAS_Rect reference(std::max(reference_width, 1), std::max(reference_height, 1));
    const RAS_FrameSettings authored_framing(framing.FrameType(),
                                             framing.BarRed(),
                                             framing.BarGreen(),
                                             framing.BarBlue(),
                                             reference.GetWidth(),
                                             reference.GetHeight());
    CameraFrame authored;
    if (!ComputeCameraFrame(authored_framing, camera, reference, reference, 1.0f, authored)) {
      return;
    }
    StoreFrame(authored, record.anchor_applied_frame);
    record.anchor_perspective = authored.perspective;
    record.anchor_authored_height = authored.top - authored.bottom;
    record.anchor_initialized = true;
  }
  /* In a placed menu: the menu's size (its placement scale) and its anchored offsets (that size
   * times the window frame's height, relative to the authored frame's). */
  const float placement = placed_menu != nullptr ? placed_menu->placement[0] : 1.0f;
  const float offset_scale = record.anchor_authored_height > 0.0f ?
                                 placement * window_height / record.anchor_authored_height :
                                 placement;
  if (current.perspective != record.anchor_perspective) {
    /* Switching between perspective and orthographic changes the frame's units: keep the element
     * where it is and place it from there in later frames. */
    StoreFrame(current, record.anchor_applied_frame);
    record.anchor_perspective = current.perspective;
    record.anchor_applied_scale = placement;
    record.anchor_applied_offset = offset_scale;
    return;
  }
  if (FramesMatch(current, record.anchor_applied_frame)) {
    return;
  }
  /* Measure the current position (wherever game logic moved it) from the anchor of the frame it
   * was placed for, in frame heights, and place it the same way in the new frame. */
  CameraFrame previous = current;
  previous.left = record.anchor_applied_frame[0];
  previous.right = record.anchor_applied_frame[1];
  previous.bottom = record.anchor_applied_frame[2];
  previous.top = record.anchor_applied_frame[3];
  const MT_Vector3 position_now = object->NodeGetWorldPosition();
  const MT_Vector4 local = MT_Matrix4x4(camera->GetWorldToCamera()) *
                           MT_Vector4(
                               position_now.x(), position_now.y(), position_now.z(), 1.0f);
  const float depth = current.perspective ? -float(local[2]) : 1.0f;
  if (!(depth > 1.0e-6f)) {
    return;
  }
  float previous_x;
  float previous_y;
  float previous_height;
  AnchorPointInFrame(previous, anchor, depth, previous_x, previous_y, previous_height);
  float anchor_x;
  float anchor_y;
  float height;
  AnchorPointInFrame(current, anchor, depth, anchor_x, anchor_y, height);
  float scale = height / previous_height;
  float size_scale = 1.0f;
  if (placed_menu != nullptr) {
    /* Scaled as the menu is (PlaceMenuInPlayerView), so it keeps its place in the menu: a part
     * below the menu's top object already got that size from it. */
    if (!(placement > 0.0f) || !(offset_scale > 0.0f) || !(record.anchor_applied_scale > 0.0f) ||
        !(record.anchor_applied_offset > 0.0f))
    {
      return;
    }
    scale = offset_scale / record.anchor_applied_offset;
    size_scale = placement / record.anchor_applied_scale;
  }
  const MT_Vector4 world = MT_Matrix4x4(camera->GetCameraToWorld()) *
                           MT_Vector4(anchor_x + (float(local[0]) - previous_x) * scale,
                                      anchor_y + (float(local[1]) - previous_y) * scale,
                                      float(local[2]),
                                      1.0f);
  if (std::fabs(world[3]) <= MT_EPSILON) {
    return;
  }
  const MT_Vector3 position(world[0] / world[3], world[1] / world[3], world[2] / world[3]);
  if (!IsFinite(position) || !std::isfinite(scale) || !std::isfinite(size_scale)) {
    return;
  }
  if (placed_menu != nullptr && placed_root) {
    object->NodeSetWorldScale(object->NodeGetWorldScaling() * size_scale);
  }
  record.anchor_applied_scale = placement;
  record.anchor_applied_offset = offset_scale;
  object->NodeSetWorldPosition(position);
  object->NodeUpdateGS(0.0f);
  StoreFrame(current, record.anchor_applied_frame);
}

void LN_UIService::ElementHitTransform(const ElementRecord &record,
                                       MT_Vector3 &r_position,
                                       MT_Vector3 &r_scale)
{
  /* The clickable area keeps the element's normal size and place: if it grew and shrank with
   * the hover and press look, a press near the edge would shrink the button out from under the
   * pointer. A look also scales the element's children about it, so undo the ancestors' too. */
  KX_GameObject *object = record.object;
  r_position = object->NodeGetWorldPosition();
  r_scale = object->NodeGetWorldScaling() / record.look_factor;
  for (KX_GameObject *parent = object->GetParent(); parent != nullptr;
       parent = parent->GetParent())
  {
    const ElementRecord *parent_record = FindElementRecord(parent);
    if (parent_record != nullptr && parent_record->look_factor != 1.0f) {
      const MT_Vector3 origin = parent->NodeGetWorldPosition();
      r_position = origin + (r_position - origin) / parent_record->look_factor;
      r_scale /= parent_record->look_factor;
    }
  }
}

void LN_UIService::CollectElements()
{
  /* Before the anchors: a split that changed moves the menus first. */
  PlaceMenusInPlayerViews();
  /* Before the elements: player 0's pointer decides here whether the mouse points. */
  UpdatePlayerPointers();
  if (m_elements.empty()) {
    return;
  }
  struct PointerRay {
    KX_Camera *camera = nullptr;
    MT_Vector3 from = MT_Vector3(0.0f, 0.0f, 0.0f);
    MT_Vector3 to = MT_Vector3(0.0f, 0.0f, 0.0f);
    bool valid = false;
  };
  const LN_MouseSnapshot *mouse = m_input != nullptr ? &m_input->GetMouse() : nullptr;
  auto make_ray = [&](KX_Camera *camera) {
    PointerRay ray;
    ray.camera = camera;
    if (camera != nullptr && camera->GetScene() == &m_scene && mouse != nullptr &&
        mouse->has_position)
    {
      ray.valid = LN_ComputeCameraPointerRay(camera,
                                             float(mouse->x),
                                             float(mouse->y),
                                             std::max(camera->GetCameraFar(), 0.0f),
                                             ray.from,
                                             ray.to,
                                             HoldsPointerInView(camera));
    }
    return ray;
  };
  const PointerRay overlay_ray = make_ray(m_scene.GetOverlayCamera());
  const PointerRay main_ray = make_ray(
      mouse != nullptr && mouse->has_position ?
          WorldPointerCamera(float(mouse->x), float(mouse->y)) :
          m_scene.GetActiveCamera());

  /* Main-world elements must not be clicked through walls: one physics ray, cast only when such
   * an element is actually under the pointer. */
  bool world_ray_cast = false;
  KX_GameObject *world_hit = nullptr;
  float world_hit_fraction = 1.0f;
  auto cast_world_ray = [&]() {
    if (world_ray_cast) {
      return;
    }
    world_ray_cast = true;
    PHY_IPhysicsEnvironment *environment = m_scene.GetPhysicsEnvironment();
    if (environment == nullptr || !main_ray.valid) {
      return;
    }
    UIWorldRayCallback callback(m_scene);
    callback.m_ignored = ShownPointerCursors();
    callback.m_camera = main_ray.camera;
    KX_RayCast::RayTest(environment, main_ray.from, main_ray.to, callback);
    const float length = (main_ray.to - main_ray.from).length();
    if (callback.m_object != nullptr && length > MT_EPSILON) {
      world_hit = callback.m_object;
      world_hit_fraction = (callback.m_hitPoint - main_ray.from).length() / length;
    }
  };

  /* The camera each player sees the game world with: their split screen view's, else the active
   * camera (a shared screen). Menus and HUDs are seen through the menu camera by everyone. */
  KX_Camera *overlay_camera = overlay_ray.camera;
  std::array<KX_Camera *, LN_MAX_GAMEPADS> world_cameras;
  world_cameras.fill(m_scene.GetActiveCamera());
  {
    static_assert(KX_PlayerViews::MaxPlayers == LN_MAX_GAMEPADS);
    std::array<int, KX_PlayerViews::MaxPlayers> players;
    std::array<KX_Camera *, KX_PlayerViews::MaxPlayers> cameras;
    const int count = m_scene.GetPlayerViews().CollectViews(
        m_scene.GetActiveCamera(), players, cameras);
    for (int index = 0; index < count; ++index) {
      world_cameras[size_t(players[size_t(index)])] = cameras[size_t(index)];
    }
  }

  for (ElementRecord &record : m_elements) {
    KX_GameObject *object = record.object;
    if (m_manager.ResolveObjectHandle(record.handle) != object ||
        m_manager.IsGameObjectQueuedForRemoval(object))
    {
      continue;
    }
    const bool overlay = m_scene.IsOverlayRuntimeObject(object);
    if (overlay && (overlay_camera == nullptr || overlay_camera->GetScene() != &m_scene)) {
      continue;
    }
    /* An anchored parent carries its children: anchoring them too would move them twice. A hidden
     * one is anchored too, so it is already in place the frame logic shows it. */
    if (overlay && record.settings.anchor != LN_UIElementAnchor::None &&
        !HasAnchoredAncestor(object))
    {
      ApplyElementAnchor(record, overlay_camera);
    }
    if (!object->GetVisible() || !ObjectPassesViewClipping(object)) {
      continue;
    }
    /* A Bar only shows a value: no pointer, focus or keys reach it, so a HUD bar never takes a
     * click or the selection. */
    if (record.settings.widget == LN_UIWidget::Bar) {
      continue;
    }

    MT_Vector3 world_position;
    MT_Vector3 world_scale;
    ElementHitTransform(record, world_position, world_scale);
    MT_Vector3 bounds_min;
    MT_Vector3 bounds_max;
    if (!ObjectBounds(object, bounds_min, bounds_max)) {
      continue;
    }
    const PointerRay &ray = overlay ? overlay_ray : main_ray;
    const int menu_index = MenuStackIndexForObject(object);
    const uint8_t users = ElementUsers(object, menu_index, record.settings);
    for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
      if ((users & UserBit(user)) == 0) {
        continue;
      }
      /* Player 0 points with the mouse in the view under it (a minimap too). The others, and
       * player 0 while the mouse is in another player's view, reach it with keys and gamepad in
       * their own view; a Set Player Pointer cursor sets its own ray (ResolveInteractions). */
      const bool pointer = user == 0 && !VirtualPointerActive(0) && ray.camera != nullptr &&
                           ray.camera->GetScene() == &m_scene;
      KX_Camera *camera = pointer ? ray.camera :
                          overlay ? overlay_camera :
                                    world_cameras[size_t(user)];
      if (camera == nullptr || camera->GetScene() != &m_scene) {
        continue;
      }
      /* Set Camera Visibility: what the player's own view does not draw is not theirs to use,
       * unless player 0's mouse is in a view that draws it (a marker only a minimap shows). */
      KX_Camera *view_camera = overlay ? overlay_camera : world_cameras[size_t(user)];
      const bool hidden_under_pointer = pointer && m_scene.IsHiddenForCamera(object, camera);
      if ((!pointer || hidden_under_pointer) && m_scene.IsHiddenForCamera(object, view_camera)) {
        continue;
      }
      Registration registration;
      registration.bounds_min = bounds_min;
      registration.bounds_max = bounds_max;
      if (!LN_ProjectWorldToScreen(camera, world_position, registration.screen_position)) {
        continue;
      }
      registration.object = object;
      registration.camera = camera;
      registration.world_position = world_position;
      registration.world_orientation = object->NodeGetWorldOrientation();
      registration.world_scale = world_scale;
      registration.padding = record.settings.padding;
      registration.interaction_domain = overlay ? LN_InteractionDomain::Overlay :
                                                  LN_InteractionDomain::Main;
      registration.enabled = object->GetUIInteractable();
      registration.user = user;
      if (pointer && ray.valid) {
        registration.ray_from = ray.from;
        registration.ray_to = ray.to;
      }
      else {
        /* Keep the element registered for keyboard and gamepad focus while the pointer is
         * outside the view (or is not this player's), but let it hit nothing. */
        registration.ray_from = registration.ray_to = camera->NodeGetWorldPosition();
        registration.has_precomputed_hit = true;
        registration.precomputed_hit = false;
      }
      if (!overlay && pointer && ray.valid) {
        float fraction = 1.0f;
        MT_Vector3 point;
        MT_Vector3 normal;
        /* Tested by the ray before it is marked as tested: RayIntersectsRegistration answers
         * from the stored result once has_precomputed_hit is set. */
        const bool hit = RayIntersectsRegistration(registration, fraction, point, normal);
        registration.has_precomputed_hit = true;
        registration.precomputed_hit = hit;
        if (registration.precomputed_hit) {
          cast_world_ray();
          const bool occluded = world_hit != nullptr && world_hit != object &&
                                !IsDescendantOf(world_hit, object) &&
                                world_hit_fraction + 1.0e-4f < fraction;
          registration.precomputed_hit = !occluded;
          registration.precomputed_fraction = fraction;
          registration.precomputed_point = point;
          registration.precomputed_normal = normal;
        }
      }
      /* Not drawn in the view the pointer is in: nothing to click there. */
      if (hidden_under_pointer) {
        registration.has_precomputed_hit = true;
        registration.precomputed_hit = false;
      }
      RegisterElementFor(record, record.settings, menu_index, std::move(registration));
    }
  }
}

uint8_t LN_UIService::ElementUsers(KX_GameObject *object,
                                   const int menu_index,
                                   const ElementSettings &settings) const
{
  /* A menu's elements are the player's who opened it. Elsewhere the player who took control of
   * it has it alone; else its Player says whose it is (player 0 has the mouse), or Any Player:
   * every player's, each with their own focus. */
  if (menu_index >= 0) {
    return UserBit(MenuUser(menu_index));
  }
  if (const int32_t controller = ControllerOf(object); controller >= 0) {
    return UserBit(controller);
  }
  return settings.any_player ? uint8_t((1u << LN_MAX_GAMEPADS) - 1u) : UserBit(settings.player);
}

void LN_UIService::RegisterElement(KX_GameObject *object,
                                   const ElementSettings &settings,
                                   Registration registration)
{
  if (object == nullptr) {
    return;
  }
  ElementRecord *record = FindElementRecord(object);
  if (record == nullptr) {
    ElementRecord created;
    created.object = object;
    created.handle = m_manager.MakeObjectHandle(object, object->GetName());
    created.settings = settings;
    m_elements.push_back(std::move(created));
    m_elementIndex[object] = m_elements.size() - 1;
    record = &m_elements.back();
  }
  const int menu_index = MenuStackIndexForObject(object);
  const uint8_t users = ElementUsers(object, menu_index, settings);
  for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
    if ((users & UserBit(user)) == 0) {
      continue;
    }
    Registration copy = registration;
    copy.user = user;
    if (user != 0) {
      /* The mouse is player 0's. */
      copy.has_precomputed_hit = true;
      copy.precomputed_hit = false;
    }
    RegisterElementFor(*record, settings, menu_index, std::move(copy));
  }
}

void LN_UIService::RegisterElementFor(ElementRecord &record,
                                      const ElementSettings &settings,
                                      const int menu_index,
                                      Registration registration)
{
  KX_GameObject *object = record.object;
  const int32_t user = registration.user;
  const ListenerKey listener = ElementListener(object, user);
  registration.listener = listener;
  registration.object = object;
  registration.priority = settings.priority;
  registration.tab_order = settings.tab_order;
  registration.padding = settings.padding;
  registration.drag_threshold = settings.drag_threshold;
  registration.propagation = settings.pointer;
  /* An empty grid cell (Show Empty) takes drops but has nothing to drag. */
  const auto cell = m_slotBindings.find(object);
  const bool draggable = settings.draggable &&
                         (cell == m_slotBindings.end() || cell->second.item.exists);
  registration.role = draggable ? LN_UIInteractableRole::DragSource :
                                  LN_UIInteractableRole::Control;
  registration.capture_on_press = true;
  registration.focusable = settings.focus != LN_UIElementFocus::None;
  registration.focus_on_press = settings.focus != LN_UIElementFocus::None;
  registration.navigable = settings.focus == LN_UIElementFocus::All;
  registration.drop_target = settings.drop_target;
  registration.click_pick_up = draggable && settings.click_pick_up;
  registration.drop_group = settings.drop_group;
  registration.accept_categories = settings.accept_categories;
  const int domain_rank = registration.interaction_domain == LN_InteractionDomain::Overlay ? 1 :
                                                                                              0;
  RegisterInteractable(std::move(registration));
  if (m_registeredListeners.contains(listener)) {
    record.users |= UserBit(user);
  }
  record.registered = record.users != 0;
  record.menu_index = menu_index;
  record.domain_rank = domain_rank;
}

void LN_UIService::BuildElementEvents()
{
  m_elementEvents.clear();
  for (ElementRecord &record : m_elements) {
    /* Each player it is or was registered for has their own events (Any Player: every player). */
    const uint8_t users = record.users | record.previous_users;
    record.previous_users = record.users;
    for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
      if ((users & UserBit(user)) == 0) {
        continue;
      }
      ElementRecord::PlayerState &player = record.players[size_t(user)];
      const auto state_item = (record.users & UserBit(user)) != 0 ?
                                  m_currentStates.find(ElementListener(record.object, user)) :
                                  m_currentStates.end();
      const bool has_state = state_item != m_currentStates.end();
      const InteractionState &state = has_state ? state_item->second : s_emptyInteractionState;
      auto add = [&](const LN_UIElementEvent type,
                     const MT_Vector3 &point,
                     const MT_Vector3 &normal,
                     const LN_ObjectHandle &target = LN_ObjectHandle()) -> ElementEvent & {
        ElementEvent &event = m_elementEvents.emplace_back();
        event.type = type;
        event.object = record.handle;
        event.target = target;
        event.point = point;
        event.normal = normal;
        event.action = record.settings.action;
        event.menu_index = record.menu_index;
        event.domain_rank = record.domain_rank;
        event.priority = record.settings.priority;
        event.user = user;
        GetBoundItem(record.object, event.item, event.index);
        if (KX_GameObject *target_object = m_manager.ResolveObjectHandle(target)) {
          GetBoundItem(target_object, event.target_item, event.target_index);
          if (const ElementRecord *target_record = FindElementRecord(target_object)) {
            event.target_action = target_record->settings.action;
          }
        }
        /* How many it is about: what a drag carries (a split stack's part), else all of the item
         * (the dragged one where this one receives it). */
        const bool receives = type == LN_UIElementEvent::DropReceived ||
                              type == LN_UIElementEvent::DropEnter ||
                              type == LN_UIElementEvent::DropExit;
        const bool drags = receives || type == LN_UIElementEvent::DragStart ||
                           type == LN_UIElementEvent::Drag || type == LN_UIElementEvent::Drop ||
                           type == LN_UIElementEvent::DropOutside ||
                           type == LN_UIElementEvent::DragCancel;
        const LN_Value &about = receives ? event.target_item : event.item;
        std::string name;
        int32_t count = 0;
        if (about.exists && about.type == LN_ValueType::ObjectRef) {
          count = 1;
        }
        else if (!LN_InventoryShownEntry(about, name, count)) {
          count = 0;
        }
        event.amount = drags && has_state && state.drag_amount > 0 ? state.drag_amount : count;
        return event;
      };
      if (has_state) {
        if (state.entered) {
          add(LN_UIElementEvent::Enter, state.point, state.normal);
        }
        if (state.exited) {
          add(LN_UIElementEvent::Exit, state.point, state.normal);
        }
        if (state.pressed) {
          add(LN_UIElementEvent::Press, state.point, state.normal);
        }
        if (state.released) {
          add(LN_UIElementEvent::Release, state.point, state.normal);
        }
        if (state.clicked) {
          add(LN_UIElementEvent::Click, state.point, state.normal);
        }
        if (state.secondary_clicked) {
          add(LN_UIElementEvent::SecondaryClick, state.point, state.normal);
        }
        if (state.drag_started) {
          add(LN_UIElementEvent::DragStart, state.point, state.normal);
        }
        if (state.dragging) {
          add(LN_UIElementEvent::Drag, state.point, state.normal);
        }
        if (state.drop_accepted) {
          add(LN_UIElementEvent::Drop, state.drop_point, state.drop_normal, state.target_object)
              .accepted = true;
        }
        else if (state.drop_rejected) {
          add(LN_UIElementEvent::Drop, state.point, state.normal).needs_world_target = true;
          add(LN_UIElementEvent::DropOutside, state.point, state.normal).needs_world_target = true;
        }
        if (state.dropped) {
          /* This one is the Drop Target: the event's target is the element dropped on it. */
          add(LN_UIElementEvent::DropReceived, state.point, state.normal, state.source_object)
              .accepted = true;
        }
        if (state.drag_cancelled) {
          add(LN_UIElementEvent::DragCancel, state.point, state.normal);
        }
      }
      else {
        if (player.was_over) {
          /* The element stopped being interactive (hidden, disabled, culled) while hovered. */
          add(LN_UIElementEvent::Exit, MT_Vector3(0.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 0.0f));
        }
        /* Its drag was cancelled because it stopped being interactive. */
        const auto cancelled = m_currentStates.find(ElementListener(record.object, user));
        if (cancelled != m_currentStates.end() && cancelled->second.drag_cancelled) {
          add(LN_UIElementEvent::DragCancel, cancelled->second.point, cancelled->second.normal);
        }
      }
      player.was_over = has_state && state.over;
      /* Drop Enter and Drop Exit: what is dragged over this one now, against last frame. A drop on
       * it ends that this frame too, so a preview never outlives the drop. */
      const bool drop_over = has_state && state.drop_over && !state.dropped;
      const LN_ObjectHandle drop_source = drop_over ? state.source_object : LN_ObjectHandle();
      const bool drop_accepted = drop_over && !state.drop_refused;
      /* Also when it stops or starts taking the same dragged item (its slot's item changed). */
      if (!SameObjectHandle(player.drop_hover_source, drop_source) ||
          (drop_source.IsValid() && drop_accepted != player.drop_hover_accepted))
      {
        if (player.drop_hover_source.IsValid()) {
          add(LN_UIElementEvent::DropExit, state.point, state.normal, player.drop_hover_source)
              .accepted = player.drop_hover_accepted;
        }
        if (drop_source.IsValid()) {
          add(LN_UIElementEvent::DropEnter, state.point, state.normal, drop_source).accepted =
              !state.drop_refused;
        }
      }
      player.drop_hover_source = drop_source;
      player.drop_hover_accepted = drop_accepted;
      /* Focus events follow the focus look: a mouse player's hidden selection sends none. */
      const bool focused = has_state && state.focus_visible;
      if (focused != player.was_focused) {
        add(focused ? LN_UIElementEvent::Focus : LN_UIElementEvent::FocusLost,
            state.point,
            state.normal);
        player.was_focused = focused;
      }
      if ((record.users & UserBit(user)) != 0 && SlotUsedBy(record, user)) {
        add(LN_UIElementEvent::Use,
            record.object->NodeGetWorldPosition(),
            MT_Vector3(0.0f, 0.0f, 1.0f));
      }
    }
  }
  for (const HighlightChange &change : m_highlightChanges) {
    /* Leaving one Action's elements tells that Action's On UI Event with an empty Object (a name
     * line clears itself). Moving between elements of one Action only names the new one, since
     * a node sees one event of a type per frame. */
    if (change.had && (!change.has || change.to.action != change.from.action)) {
      ElementEvent &event = m_elementEvents.emplace_back();
      event.type = LN_UIElementEvent::Highlight;
      event.action = change.from.action;
      event.user = change.user;
      /* After every event that names an element, so an unfiltered node sees the new one. */
      event.menu_index = -2;
    }
    if (change.has) {
      KX_GameObject *object = m_manager.ResolveObjectHandle(change.to.object);
      if (object == nullptr) {
        continue;
      }
      ElementEvent &event = m_elementEvents.emplace_back();
      event.type = LN_UIElementEvent::Highlight;
      event.object = change.to.object;
      event.point = object->NodeGetWorldPosition();
      event.action = change.to.action;
      event.menu_index = change.to.menu_index;
      event.domain_rank = change.to.domain_rank;
      event.priority = change.to.priority;
      event.user = change.user;
      GetBoundItem(object, event.item, event.index);
    }
  }
  /* Item Shown: no more a frame than a node hears (a big grid opening), the rest the next ones. */
  size_t shown_events = 0;
  for (const CellShown &cell : m_cellsShown) {
    if (shown_events == kMaxEventsPerNodeFrame) {
      break;
    }
    /* Told once the slot is a UI Element (a hidden cell tells it when it shows). */
    KX_GameObject *object = m_manager.ResolveObjectHandle(cell.object);
    const ElementRecord *record = object != nullptr ? FindElementRecord(object) : nullptr;
    if (record == nullptr) {
      continue;
    }
    std::vector<size_t> &announced = m_viewStates[cell.view].shown_announced;
    if (cell.slot < announced.size()) {
      announced[cell.slot] = cell.fingerprint;
    }
    ElementEvent &event = m_elementEvents.emplace_back();
    event.type = LN_UIElementEvent::ItemShown;
    event.object = record->handle;
    event.point = object->NodeGetWorldPosition();
    event.normal = MT_Vector3(0.0f, 0.0f, 1.0f);
    event.action = record->settings.action;
    event.menu_index = record->menu_index;
    event.domain_rank = record->domain_rank;
    event.priority = record->settings.priority;
    event.user = cell.user;
    event.item = cell.item;
    event.index = cell.index;
    std::string name;
    if (cell.item.exists && cell.item.type == LN_ValueType::ObjectRef) {
      event.amount = 1;
    }
    else if (!LN_InventoryShownEntry(cell.item, name, event.amount)) {
      event.amount = 0;
    }
    shown_events++;
  }
  /* The topmost menu, then overlay before world, then priority: the same order as the pointer. */
  std::stable_sort(m_elementEvents.begin(),
                   m_elementEvents.end(),
                   [](const ElementEvent &a, const ElementEvent &b) {
                     if (a.menu_index != b.menu_index) {
                       return a.menu_index > b.menu_index;
                     }
                     if (a.domain_rank != b.domain_rank) {
                       return a.domain_rank > b.domain_rank;
                     }
                     return a.priority > b.priority;
                   });
}

void LN_UIService::UpdateElementPresentation(const double unscaled_delta_time)
{
  const float delta_time = float(std::max(0.0, unscaled_delta_time));
  for (ElementRecord &record : m_elements) {
    KX_GameObject *object = record.object;
    if (m_manager.ResolveObjectHandle(record.handle) != object) {
      continue;
    }
    /* Each player's state (an Any Player element has one per player). */
    std::array<const InteractionState *, LN_MAX_GAMEPADS> states = {};
    for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
      if ((record.users & UserBit(user)) != 0) {
        const auto state_item = m_currentStates.find(ElementListener(object, user));
        if (state_item != m_currentStates.end()) {
          states[size_t(user)] = &state_item->second;
        }
      }
    }
    const ElementSettings &settings = record.settings;
    bool transformed = false;

    if (settings.draggable && settings.follow_cursor) {
      for (int32_t user = 0; user < LN_MAX_GAMEPADS && !record.following; ++user) {
        const InteractionState *state = states[size_t(user)];
        if (state != nullptr && state->drag_started) {
          record.following = true;
          record.drag_user = user;
          record.drag_parent = object->GetSGNode()->GetSGParent();
          record.drag_home = object->NodeGetLocalPosition();
          record.drag_home_world = object->NodeGetWorldPosition();
          record.drag_offset = record.drag_home_world - state->drag_origin;
          /* Item Only: a copy of the item follows and the slot stays in its cell. Else the slot
           * follows, and a split stack leaves a copy of it in the cell with the rest. */
          record.drag_picture = settings.drag_item_only && StartDragPicture(object);
          if (!record.drag_picture) {
            StartDragGhost(object);
          }
        }
      }
      if (record.following && record.drag_picture) {
        const InteractionState *state = states[size_t(record.drag_user)];
        if (state != nullptr && state->dragging) {
          PlaceDragPicture(object, state->point + record.drag_offset);
        }
        else {
          /* The copy stays where it was let go until the grid shows where the item went, next
           * frame (SweepDragGhosts takes it then): the item never vanishes for a frame. */
          record.following = false;
          record.drag_picture = false;
        }
      }
      else if (record.following) {
        const InteractionState *state = states[size_t(record.drag_user)];
        if (state != nullptr && state->dragging) {
          object->NodeSetWorldPosition(state->point + record.drag_offset);
        }
        else {
          /* Drop or cancel: return home, in the parent's space so a parent that moved or scaled
           * meanwhile does not shift it. If game logic changed the parent during the drag, the
           * old local position means nothing there: return to the world position instead. A
           * Drop handler that runs later this frame can still move the object somewhere else. */
          if (object->GetSGNode()->GetSGParent() == record.drag_parent) {
            object->NodeSetLocalPosition(record.drag_home);
          }
          else {
            object->NodeSetWorldPosition(record.drag_home_world);
          }
          record.following = false;
          EndDragGhost(object);
        }
        transformed = true;
      }
    }

    if (settings.hover_scale != 1.0f || settings.press_scale != 1.0f ||
        record.look_factor != 1.0f)
    {
      /* Pressed by one player, else hovered or focused by one: Any Player shows any player's. */
      bool pressed = false;
      bool hovered = false;
      for (const InteractionState *state : states) {
        if (state != nullptr) {
          pressed = pressed || (state->captured && state->over && !state->dragging);
          /* A dragged element keeps its hover look while it follows; an Item Only drag's item
           * keeps it (its copy took the size) and the slot left in its cell goes back. */
          hovered = hovered || state->over || state->focus_visible ||
                    (state->dragging && !record.drag_picture);
        }
      }
      float target = 1.0f;
      if (object->GetUIInteractable()) {
        target = pressed ? settings.press_scale : hovered ? settings.hover_scale : 1.0f;
      }
      const MT_Vector3 local_scale = object->NodeGetLocalScaling();
      if (record.look_factor == 1.0f) {
        /* At rest the object's own scale is authoritative, including changes by game logic. */
        record.base_scale = local_scale;
      }
      else if ((local_scale - record.look_scale).length2() > 1.0e-12f) {
        /* Game logic set a new scale during the look: keep it as the scale to return to. */
        record.base_scale = local_scale / record.look_factor;
        record.look_scale = local_scale;
      }
      float factor = target;
      if (settings.look_time > 0.0f) {
        /* Blend linearly so a full change between any two looks takes Look Time. */
        const float span = std::max({std::abs(settings.hover_scale - 1.0f),
                                     std::abs(settings.press_scale - 1.0f),
                                     std::abs(settings.hover_scale - settings.press_scale),
                                     1.0e-6f});
        const float max_step = span * delta_time / settings.look_time;
        const float delta = target - record.look_factor;
        factor = std::abs(delta) <= max_step ? target :
                                               record.look_factor + std::copysign(max_step, delta);
      }
      if (factor != record.look_factor) {
        record.look_factor = factor;
        record.look_scale = record.base_scale * factor;
        object->NodeSetLocalScale(record.look_scale);
        transformed = true;
      }
    }
    if (transformed) {
      object->NodeUpdateGS(0.0f);
    }
    PresentWidget(record);
    PresentBar(record, delta_time);
  }
  /* Split drag copies whose drag ended go before this frame is drawn. */
  RemoveQueuedTemplateCopies();
  PresentHighlights();
  PresentTooltips();

  /* Drop and Drop Outside of one release share the answer: look once per dragged element. */
  const ElementEvent *resolved = nullptr;
  WorldDrop resolved_over = WorldDrop::None;
  for (ElementEvent &event : m_elementEvents) {
    if (!event.needs_world_target) {
      continue;
    }
    WorldDrop over = WorldDrop::None;
    if (resolved != nullptr && SameObjectHandle(resolved->object, event.object)) {
      event.needs_world_target = false;
      event.target = resolved->target;
      event.target_item = LN_Value();
      event.target_index = -1;
      event.target_action.clear();
      event.point = resolved->point;
      event.normal = resolved->normal;
      over = resolved_over;
    }
    else {
      over = ResolveWorldDropTarget(event);
    }
    /* Let go over other UI, a minimap or over nothing, the drag did nothing: the element goes
     * back, like a cancel. Drop Outside is only for the game world; a grid that drops in front
     * of its list's object takes any release over the player's view (the sky too). */
    if (event.type == LN_UIElementEvent::DropOutside && over != WorldDrop::Object &&
        !(over == WorldDrop::View && DropsInFront(event.object)))
    {
      event.type = LN_UIElementEvent::DragCancel;
    }
    resolved = &event;
    resolved_over = over;
  }
  ApplyGridInventory();
  PresentGridHints();
  PresentMessages(delta_time);
  CollectUISounds();
}

LN_UIService::WorldDrop LN_UIService::ResolveWorldDropTarget(ElementEvent &event)
{
  event.needs_world_target = false;
  const LN_MouseSnapshot *mouse = m_input != nullptr ? &m_input->GetMouse() : nullptr;
  /* A pointer cursor stays on its surface: there is no game world under it. */
  if (mouse == nullptr || !mouse->has_position || event.user != 0 || VirtualPointerActive(0)) {
    return WorldDrop::None;
  }
  const WorldDropHit hit = m_worldDropForTests ?
                               m_worldDropForTests(float(mouse->x), float(mouse->y)) :
                               WorldDropUnderPointer(event, float(mouse->x), float(mouse->y));
  if (hit.over == WorldDrop::Object && hit.object != nullptr) {
    event.target = m_manager.MakeObjectHandle(hit.object, hit.object->GetName());
    event.target_item = LN_Value();
    event.target_index = -1;
    event.target_action.clear();
    event.point = hit.point;
    event.normal = hit.normal;
    return WorldDrop::Object;
  }
  return hit.over == WorldDrop::Object ? WorldDrop::View : hit.over;
}

LN_UIService::WorldDropHit LN_UIService::WorldDropUnderPointer(const ElementEvent &event,
                                                               const float pointer_x,
                                                               const float pointer_y) const
{
  WorldDropHit result;
  /* Dropped onto other UI (a panel, a label, a background): it did not leave the UI. What is
   * dragged is no such UI: the dragged element, or a drag's copy (an Item Only drag's item is
   * under the pointer). */
  KX_GameObject *dragged = m_manager.ResolveObjectHandle(event.object);
  KX_Camera *overlay_camera = m_scene.GetOverlayCamera();
  MT_Vector3 from;
  MT_Vector3 to;
  if (overlay_camera != nullptr &&
      LN_ComputeCameraPointerRay(overlay_camera,
                                 pointer_x,
                                 pointer_y,
                                 std::max(overlay_camera->GetCameraFar(), 0.0f),
                                 from,
                                 to))
  {
    if (EXP_ListValue<KX_GameObject> *objects = m_scene.GetObjectList()) {
      for (KX_GameObject *object : *objects) {
        if (object == nullptr || object == overlay_camera || !object->GetVisible() ||
            !m_scene.IsOverlayRuntimeObject(object) || IsDescendantOf(object, dragged) ||
            m_dragGhostObjects.contains(object))
        {
          continue;
        }
        Registration registration;
        if (!ObjectBounds(object, registration.bounds_min, registration.bounds_max)) {
          continue;
        }
        registration.ray_from = from;
        registration.ray_to = to;
        registration.world_position = object->NodeGetWorldPosition();
        registration.world_orientation = object->NodeGetWorldOrientation();
        registration.world_scale = object->NodeGetWorldScaling();
        float fraction;
        MT_Vector3 point;
        MT_Vector3 normal;
        if (RayIntersectsRegistration(registration, fraction, point, normal)) {
          return result;
        }
      }
    }
  }
  /* Only into the player's own view (the mouse is player 0's): a camera drawn over it (a minimap,
   * a picture in picture) shows the world too, but letting go there is not dropping into it, as
   * letting go over another player's view is not (LN_CameraUnderPointer gives none there). */
  KX_Camera *main_camera = LN_CameraUnderPointer(m_scene, pointer_x, pointer_y);
  PHY_IPhysicsEnvironment *environment = m_scene.GetPhysicsEnvironment();
  if (main_camera == nullptr || main_camera != PlayerViewCamera(0) || environment == nullptr ||
      !LN_ComputeCameraPointerRay(main_camera,
                                  pointer_x,
                                  pointer_y,
                                  std::max(main_camera->GetCameraFar(), 0.0f),
                                  from,
                                  to))
  {
    return result;
  }
  result.over = WorldDrop::View;
  UIWorldRayCallback callback(m_scene);
  callback.m_ignored = ShownPointerCursors();
  callback.m_camera = main_camera;
  KX_RayCast::RayTest(environment, from, to, callback);
  if (callback.m_object == nullptr || IsDescendantOf(callback.m_object, dragged)) {
    return result;
  }
  result.over = WorldDrop::Object;
  result.object = callback.m_object;
  result.point = callback.m_hitPoint;
  result.normal = callback.m_hitNormal;
  return result;
}

/* -------------------------------------------------------------------- */
/** \name Grid View Inventory: moves, drops and hints
 *
 * A Grid View whose Object and Property name its list can change that list itself, with the
 * Inventory node's rules, in the UI frame: the On UI Event routes that run after it see what
 * happened in their Outcome output, and the grids show it from the next frame on, as when a route
 * moves the item.
 * \{ */

namespace {

/** Drops: how far down a floor is looked for, how far from a wall in the way a drop stays, and
 * how far off the surface it hit the floor ray starts. Drops in front keep kDropSpacing from the
 * pickups lying there (on the same level: within kDropSameLevel up or down). */
constexpr float kDropFloorDepth = 100.0f;
constexpr float kDropWallGap = 0.25f;
constexpr float kDropSurfaceGap = 0.1f;
constexpr float kDropSpacing = 0.5f;
constexpr float kDropSameLevel = 1.5f;

/** A physics ray for drops: the first solid object, visible or not (an invisible floor holds
 * things), outside menus, not a sensor, nor one of `m_ignored` or their children. */
class UIDropRayCallback : public KX_RayCast {
 public:
  explicit UIDropRayCallback(KX_Scene &scene) : KX_RayCast(nullptr, false, false), m_scene(scene)
  {
    m_hitFound = false;
  }

  bool RayHit(KX_ClientObjectInfo *client) override
  {
    KX_GameObject *object = client != nullptr ? client->m_gameobject : nullptr;
    if (object == nullptr || client->isSensor() || m_scene.IsOverlayRuntimeObject(object) ||
        Ignored(object))
    {
      return false;
    }
    m_object = object;
    m_hitFound = true;
    return true;
  }

  bool needBroadphaseRayCast(PHY_IPhysicsController *controller) override
  {
    KX_ClientObjectInfo *info = controller != nullptr ?
                                    static_cast<KX_ClientObjectInfo *>(
                                        controller->GetNewClientInfo()) :
                                    nullptr;
    return info != nullptr && info->m_gameobject != nullptr && !info->isSensor() &&
           !m_scene.IsOverlayRuntimeObject(info->m_gameobject) && !Ignored(info->m_gameobject);
  }

  bool Ignored(KX_GameObject *object) const
  {
    for (KX_GameObject *ignored : m_ignored) {
      for (KX_GameObject *item = object; item != nullptr; item = item->GetParent()) {
        if (item == ignored) {
          return true;
        }
      }
    }
    return false;
  }

  KX_Scene &m_scene;
  KX_GameObject *m_object = nullptr;
  std::vector<KX_GameObject *> m_ignored;
};

/** An item's words for a tooltip or a hint (ExpandItemTokens). */
struct ItemTokens {
  /** The item as its list holds it ({item}) and how many ({count}); empty without an item. */
  std::string entry;
  int32_t count = 0;
  /** The item it shows (a recipe line: what it makes), {name} when it has no "name" property. */
  std::string shown;
  /** That item's Item Library object (an object item: the object), and the UI Element: their
   * game properties fill in the other words, the item's first. */
  KX_GameObject *source = nullptr;
  KX_GameObject *element = nullptr;
  /** The item as its list holds it: its own values ({durability}) come before all those. */
  LN_Value own;
};

/** The words of `item`, a Grid View list value; `find_item` gives an item's library object. */
template<typename FindItem>
ItemTokens GridItemTokens(LN_Manager &manager,
                          KX_Scene &scene,
                          const LN_Value &item,
                          KX_GameObject *element,
                          const FindItem &find_item)
{
  ItemTokens tokens;
  tokens.element = element;
  if (item.exists && item.type == LN_ValueType::ObjectRef) {
    /* An object item has its own properties (as its category). */
    KX_GameObject *object = item.runtime_ref.shared_domain != 0 ?
                                manager.ResolveSharedObjectRef(item.runtime_ref) :
                                nullptr;
    if (object == nullptr && !item.reference_name.empty()) {
      if (EXP_ListValue<KX_GameObject> *objects = scene.GetObjectList()) {
        object = objects->FindValue(item.reference_name);
      }
    }
    tokens.entry = tokens.shown = item.reference_name;
    tokens.count = 1;
    tokens.source = object != nullptr ? object : find_item(tokens.shown);
    return tokens;
  }
  int32_t shown_count = 0;
  if (!LN_InventoryEntry(item, tokens.entry, tokens.count) ||
      !LN_InventoryShownEntry(item, tokens.shown, shown_count))
  {
    tokens = ItemTokens();
    tokens.element = element;
    return tokens;
  }
  tokens.source = find_item(tokens.shown);
  tokens.own = item;
  return tokens;
}

/** A UI Element that is no grid slot may name an item itself, as a pickup does: its "item" game
 * property, and "count" how many (1 without it). Else it has no item and its own game properties
 * fill in the words. */
template<typename FindItem>
ItemTokens ElementItemTokens(KX_GameObject *element, const FindItem &find_item)
{
  ItemTokens tokens;
  tokens.element = element;
  EXP_Value *item = element->GetProperty("item");
  if (item == nullptr || item->GetText().empty()) {
    return tokens;
  }
  tokens.entry = tokens.shown = item->GetText();
  tokens.count = 1;
  if (EXP_Value *count = element->GetProperty("count")) {
    const double number = count->GetNumber();
    if (std::isfinite(number)) {
      tokens.count = int32_t(std::clamp(std::round(number), 0.0, 2147483647.0));
    }
  }
  tokens.source = find_item(tokens.shown);
  return tokens;
}

/** `text` with an item's words in braces: {item} and {count} as the list holds it, {name} (the
 * "name" value, else the item it shows) and any other value ({description}, {price},
 * {durability}): the item's own first (a slot's own values), then its object's game properties,
 * then the element's. A value's own braces are filled in too, once (a description saying
 * "Durability {durability}"). {description} and {category} are empty when missing; other braces
 * stay as written for the text filter ({Use} key names, {Player.gold}). Blank lines at the end
 * go (an item without a description). */
std::string ExpandItemTokens(const std::string &text,
                             const ItemTokens &tokens,
                             const bool nested = false)
{
  const auto property_text = [&](const std::string &name, std::string &r_text) {
    if (const LN_Value *own = OwnValue(tokens.own, name)) {
      r_text = OwnValueText(*own);
    }
    else {
      EXP_Value *value = nullptr;
      for (KX_GameObject *object : {tokens.source, tokens.element}) {
        if ((value = object != nullptr ? object->GetProperty(name) : nullptr) != nullptr) {
          break;
        }
      }
      if (value == nullptr) {
        return false;
      }
      r_text = value->GetText();
    }
    if (!nested && r_text.find('{') != std::string::npos) {
      r_text = ExpandItemTokens(r_text, tokens, true);
    }
    return true;
  };
  std::string result;
  result.reserve(text.size() + 32);
  size_t at = 0;
  while (at < text.size()) {
    const size_t open = text.find('{', at);
    const size_t close = open != std::string::npos ? text.find('}', open + 1) : std::string::npos;
    if (close == std::string::npos) {
      result.append(text, at, std::string::npos);
      break;
    }
    result.append(text, at, open - at);
    const std::string token = text.substr(open + 1, close - open - 1);
    std::string value;
    if (token == "item") {
      result += tokens.entry;
    }
    else if (token == "count") {
      if (!tokens.entry.empty()) {
        result += std::to_string(tokens.count);
      }
    }
    else if (token == "name") {
      result += property_text(token, value) && !value.empty() ? value : tokens.shown;
    }
    else if (property_text(token, value)) {
      result += value;
    }
    else if (token != "description" && token != "category") {
      result.append(text, open, close - open + 1);
    }
    at = close + 1;
  }
  while (!result.empty() && (result.back() == '\n' || result.back() == ' ')) {
    result.pop_back();
  }
  return result;
}

}  // namespace

KX_GameObject *LN_UIService::FindLibraryItem(const blender::Collection *library,
                                             const std::string &item) const
{
  if (item.empty()) {
    return nullptr;
  }
  if (m_itemLibraryForTests) {
    return m_itemLibraryForTests(item);
  }
  if (library == nullptr) {
    const blender::Scene *blender_scene = m_scene.GetBlenderScene();
    library = blender_scene != nullptr ? blender_scene->gm.item_library : nullptr;
  }
  return LN_FindLibraryItem(m_scene, library, item);
}

const LN_UIService::ViewRegistration *LN_UIService::GridOfSlot(const KX_GameObject *object) const
{
  const auto binding = m_slotBindings.find(const_cast<KX_GameObject *>(object));
  if (binding == m_slotBindings.end()) {
    return nullptr;
  }
  for (const ViewRegistration &view : m_viewRegistrations) {
    if (view.kind == ViewKind::Grid && view.listener == binding->second.view) {
      return &view;
    }
  }
  return nullptr;
}

bool LN_UIService::ReadGridList(const ViewRegistration &view, std::vector<LN_Value> &r_list)
{
  r_list.clear();
  if (view.list_object == nullptr || view.list_property.empty() ||
      m_manager.IsGameObjectQueuedForRemoval(view.list_object))
  {
    return false;
  }
  EXP_Value *property = view.list_object->GetProperty(view.list_property);
  if (property == nullptr) {
    /* An inventory nothing was put in yet. */
    return true;
  }
  /* Objects in it as the grid's own items hold them (shared references), so an item is found
   * again by its fingerprint and written back as the very object. */
  LN_Value value = LN_GamePropertyValue(property, &m_manager);
  if (!value.exists || value.type != LN_ValueType::List) {
    WarnGridListNotAList(view.list_object, view.list_property);
    return false;
  }
  r_list = std::move(value.list_value);
  return true;
}

void LN_UIService::WarnGridListNotAList(KX_GameObject *object, const std::string &property)
{
  if (object != nullptr &&
      m_gridWarnings.insert("list\x1f" + object->GetName() + "\x1f" + property).second)
  {
    CM_Warning("Object Grid View: game property '" << property << "' of '" << object->GetName()
                                                   << "' is not a List or Table, so the grid "
                                                      "neither shows nor changes it");
  }
}

bool LN_UIService::WriteGridList(const ViewRegistration &view, const std::vector<LN_Value> &list)
{
  LN_Value value;
  value.type = LN_ValueType::List;
  value.exists = true;
  value.list_value = list;
  std::string reason;
  if (!LN_CommandBuffer::ApplyGamePropertyValue(
          view.list_object, view.list_property, value, nullptr, reason))
  {
    if (m_gridWarnings.insert("write\x1f" + view.list_object->GetName() + "\x1f" +
                              view.list_property)
            .second)
    {
      CM_Warning("Object Grid View: cannot write game property '"
                 << view.list_property << "' of '" << view.list_object->GetName()
                 << "': " << reason);
    }
    return false;
  }
  /* Its trees read the new list in this tick, as after a widget's change. */
  const LN_ObjectHandle owner = m_manager.MakeObjectHandle(view.list_object,
                                                           view.list_object->GetName());
  if (std::none_of(m_widgetChangedOwners.begin(),
                   m_widgetChangedOwners.end(),
                   [&](const LN_ObjectHandle &handle) { return SameObjectHandle(handle, owner); }))
  {
    m_widgetChangedOwners.push_back(owner);
  }
  return true;
}

int32_t LN_UIService::FindGridItem(const std::vector<LN_Value> &list,
                                   const int32_t index,
                                   const LN_Value &item)
{
  return LN_InventoryFindPlace(list, index, item);
}

KX_GameObject *LN_UIService::SolidWorldHit(const MT_Vector3 &from,
                                           const MT_Vector3 &to,
                                           KX_GameObject *ignored,
                                           MT_Vector3 &r_point) const
{
  const float length = (to - from).length();
  PHY_IPhysicsEnvironment *environment = m_worldRayForTests ? nullptr :
                                                              m_scene.GetPhysicsEnvironment();
  if (length <= MT_EPSILON || (!m_worldRayForTests && environment == nullptr)) {
    return nullptr;
  }
  std::vector<KX_GameObject *> skipped;
  if (ignored != nullptr) {
    skipped.push_back(ignored);
  }
  /* A shape the ray starts in is hit at once (convex shapes are solid): it is not in the way. */
  for (int attempt = 0; attempt < 4; ++attempt) {
    KX_GameObject *hit = nullptr;
    float fraction = 1.0f;
    if (m_worldRayForTests) {
      hit = m_worldRayForTests(from, to, skipped, fraction);
      r_point = from + (to - from) * fraction;
    }
    else {
      UIDropRayCallback callback(m_scene);
      callback.m_ignored = skipped;
      KX_RayCast::RayTest(environment, from, to, callback);
      hit = callback.m_object;
      r_point = callback.m_hitPoint;
      fraction = hit != nullptr ? (callback.m_hitPoint - from).length() / length : 1.0f;
    }
    if (hit != nullptr && fraction * length <= 1.0e-4f) {
      skipped.push_back(hit);
      continue;
    }
    return hit;
  }
  return nullptr;
}

MT_Vector3 LN_UIService::DropFloorPoint(const MT_Vector3 &point,
                                        const MT_Vector3 &normal,
                                        KX_GameObject *ignored) const
{
  /* From a little off what it hit (out of a wall, above a floor), straight down. */
  const MT_Vector3 away = normal.length2() > 1.0e-8f ? normal.normalized() :
                                                       MT_Vector3(0.0f, 0.0f, 1.0f);
  const MT_Vector3 start = point + away * kDropSurfaceGap;
  MT_Vector3 floor;
  if (SolidWorldHit(start, start - MT_Vector3(0.0f, 0.0f, kDropFloorDepth), ignored, floor) !=
      nullptr)
  {
    return floor;
  }
  return point;
}

bool LN_UIService::DropGridStack(const ViewRegistration &view,
                                 const int32_t index,
                                 const LN_Value &item,
                                 const MT_Vector3 &position,
                                 const int32_t amount)
{
  std::vector<LN_Value> list;
  if (!ReadGridList(view, list)) {
    return false;
  }
  const int32_t at = FindGridItem(list, index, item);
  std::string name;
  int32_t count = 0;
  if (at < 0 || !LN_InventoryEntry(list[size_t(at)], name, count) || count <= 0) {
    return false;
  }
  /* Like Drop Item: nothing leaves the list unless its copy can go into the world. */
  KX_GameObject *source = FindLibraryItem(view.icon_library, name);
  if (source == nullptr) {
    if (m_gridWarnings.insert("library\x1f" + name).second) {
      CM_Warning("Object Grid View: '" << name
                                       << "' has no object in the Item Library, so it is not "
                                          "dropped (set the list's Inventory Rules Item Library, "
                                          "or Scene > Items and UI > Item Library)");
    }
    return false;
  }
  std::string reason;
  if (!m_manager.PreflightAddObjectPersistence(source, 0.0f, false, reason)) {
    if (m_gridWarnings.insert("copy\x1f" + name).second) {
      CM_Warning("Object Grid View: '" << name << "' cannot be copied into the world (" << reason
                                       << "); keep the Item Library hidden");
    }
    return false;
  }
  /* A split stack's part goes; the rest stays in the slot. */
  const int32_t dropped = amount > 0 && amount < count ? amount : count;
  LN_InventoryRemoveFromSlot(list, at, dropped, LayoutOf(view));
  if (!WriteGridList(view, list)) {
    return false;
  }
  /* Objects join the world on Logic + Physics steps (as a UI route's Drop Item). */
  m_manager.QueueItemDrop(source, position, name, dropped);
  return true;
}

LN_InventoryOutcome LN_UIService::MoveGridItem(const ViewRegistration &target,
                                               const ViewRegistration &source,
                                               const ElementEvent &event)
{
  if (event.index < 0 || event.target_index < 0 || !event.target_item.exists) {
    return LN_InventoryOutcome::None;
  }
  const bool same_list = SameGridList(source, target);
  /* A filtered grid's empty cell is no place of its own: an item of its list moved there would
   * go to a place the grid does not show. */
  if (same_list && !event.item.exists && IsFiltered(target)) {
    return LN_InventoryOutcome::None;
  }
  std::vector<LN_Value> target_list;
  std::vector<LN_Value> source_list;
  if (!ReadGridList(target, target_list) || (!same_list && !ReadGridList(source, source_list))) {
    return LN_InventoryOutcome::None;
  }
  /* The player's move as plain values; the lists and their Inventory Rules decide the rest
   * (LN_InventoryApplyMove needs no grid). */
  LN_InventoryMove move;
  move.from = event.target_index;
  move.from_item = event.target_item;
  move.to = event.index;
  move.to_item = event.item;
  move.amount = event.amount;
  move.allow_swap = target.inventory.swap;
  /* The dragged slot gets the other item in a swap only if it takes its kind (AcceptsDrop). */
  const ElementRecord *dragged = FindElementRecord(m_manager.ResolveObjectHandle(event.target));
  move.swap_back_allowed = dragged == nullptr || !event.item.exists ||
                           SwapBackAllowed(dragged->settings.drop_target,
                                           dragged->settings.accept_categories,
                                           event.item,
                                           target.listener);
  /* A one-cell grid (equipment, a quick slot) only swaps or stacks: a second item in its list
   * would never show. */
  move.into_list_fallback = !HoldsOneSlot(target);
  const LN_InventoryRules target_rules = ListRules(target);
  const LN_InventoryRules source_rules = ListRules(source);
  std::vector<LN_Recipe> recipes;
  if (same_list && !target_rules.recipes_property.empty()) {
    if (EXP_Value *property = target.list_object->GetProperty(target_rules.recipes_property)) {
      LN_ParseRecipes(LN_GamePropertyValue(property, &m_manager), recipes);
    }
  }
  const std::vector<LN_Value> target_before = target_list;
  std::string made;
  const LN_InventoryOutcome outcome = LN_InventoryApplyMove(move,
                                                            target_list,
                                                            target_rules,
                                                            same_list ? nullptr : &source_list,
                                                            same_list ? nullptr : &source_rules,
                                                            recipes,
                                                            InventoryLookup(),
                                                            made);
  if (outcome != LN_InventoryOutcome::Moved && outcome != LN_InventoryOutcome::Stacked &&
      outcome != LN_InventoryOutcome::Combined && outcome != LN_InventoryOutcome::Swapped)
  {
    return outcome;
  }
  /* The receiving list first: if the other one cannot be written, it is put back. */
  if (!WriteGridList(target, target_list)) {
    return LN_InventoryOutcome::None;
  }
  if (!same_list && !WriteGridList(source, source_list)) {
    WriteGridList(target, target_before);
    return LN_InventoryOutcome::None;
  }
  return outcome;
}

LN_InventoryLayout LN_UIService::LayoutOf(const ViewRegistration &view)
{
  return view.rules.Layout();
}

const LN_UIService::ViewRegistration *LN_UIService::HandlingGrid(
    const LN_ObjectHandle &slot) const
{
  KX_GameObject *object = m_manager.ResolveObjectHandle(slot);
  const ViewRegistration *view = object != nullptr ? GridOfSlot(object) : nullptr;
  return view != nullptr && view->inventory.handle_moves && view->list_object != nullptr ? view :
                                                                                           nullptr;
}

bool LN_UIService::DropsInFront(const LN_ObjectHandle &slot) const
{
  const ViewRegistration *view = HandlingGrid(slot);
  return view != nullptr && view->inventory.drop_into_world && !view->inventory.drop_at_pointer;
}

void LN_UIService::NearbyPickups(const MT_Vector3 &center,
                                 const float radius,
                                 std::vector<MT_Vector3> &r_positions) const
{
  m_manager.QueuedItemDropPositions(r_positions);
  /* A test world (SetWorldRayForTests) has no scene objects. */
  EXP_ListValue<KX_GameObject> *objects = m_worldRayForTests ? nullptr : m_scene.GetObjectList();
  if (objects == nullptr) {
    return;
  }
  for (KX_GameObject *object : *objects) {
    if (object == nullptr || m_scene.IsOverlayRuntimeObject(object) ||
        m_manager.IsGameObjectQueuedForRemoval(object) || object->GetProperty("item") == nullptr)
    {
      continue;
    }
    const MT_Vector3 position = object->NodeGetWorldPosition();
    const float dx = position.x() - center.x();
    const float dy = position.y() - center.y();
    if (dx * dx + dy * dy <= radius * radius) {
      r_positions.push_back(position);
    }
  }
}

MT_Vector3 LN_UIService::InFrontDropPosition(const ViewRegistration &view) const
{
  /* Ahead of the list's object (the player) along the ground, not through a wall. Dropped copies
   * don't move (no physics), so a drop takes the first free spot around there: in a pickup that
   * lies there already (dropped or placed: it has an "item" property) it would hide. */
  KX_GameObject *owner = view.list_object;
  const MT_Vector3 origin = owner->NodeGetWorldPosition();
  MT_Vector3 forward = owner->NodeGetWorldOrientation().getColumn(1);
  forward.z() = 0.0f;
  forward = forward.length2() > 1.0e-8f ? forward.normalized() : MT_Vector3(0.0f, 1.0f, 0.0f);
  const MT_Vector3 right(forward.y(), -forward.x(), 0.0f);
  const float distance = std::max(0.0f, view.inventory.distance);
  const auto ground_spot = [&](const MT_Vector3 &point) {
    MT_Vector3 spot = point;
    const MT_Vector3 way = point - origin;
    MT_Vector3 wall;
    if (way.length2() > 1.0e-8f && SolidWorldHit(origin, point, owner, wall) != nullptr) {
      spot = origin + way.normalized() * std::max(0.0f, (wall - origin).length() - kDropWallGap);
    }
    return DropFloorPoint(spot, MT_Vector3(0.0f, 0.0f, 1.0f), owner);
  };
  std::vector<MT_Vector3> pickups;
  NearbyPickups(origin + forward * distance, 4.0f * kDropSpacing, pickups);
  /* The spot ahead, then beside it, further ahead, nearer: steps of kDropSpacing (ahead, right). */
  static constexpr int8_t kSpots[][2] = {{0, 0},
                                         {0, 1},
                                         {0, -1},
                                         {1, 0},
                                         {1, 1},
                                         {1, -1},
                                         {-1, 1},
                                         {-1, -1},
                                         {0, 2},
                                         {0, -2},
                                         {1, 2},
                                         {1, -2},
                                         {2, 0},
                                         {2, 1},
                                         {2, -1}};
  MT_Vector3 first;
  for (size_t index = 0; index < std::size(kSpots); ++index) {
    const MT_Vector3 spot = ground_spot(origin +
                                        forward * (distance + kSpots[index][0] * kDropSpacing) +
                                        right * (kSpots[index][1] * kDropSpacing));
    if (index == 0) {
      first = spot;
    }
    const bool taken = std::any_of(
        pickups.begin(), pickups.end(), [&](const MT_Vector3 &pickup) {
          const float dx = pickup.x() - spot.x();
          const float dy = pickup.y() - spot.y();
          return dx * dx + dy * dy < kDropSpacing * kDropSpacing * 0.81f &&
                 std::fabs(pickup.z() - spot.z()) < kDropSameLevel;
        });
    if (!taken) {
      return spot + MT_Vector3(0.0f, 0.0f, view.inventory.hover_height);
    }
  }
  /* Every spot around is taken: on the first one after all. */
  return first + MT_Vector3(0.0f, 0.0f, view.inventory.hover_height);
}

void LN_UIService::ApplyGridInventory()
{
  if (m_passive) {
    return;
  }
  for (ElementEvent &event : m_elementEvents) {
    switch (event.type) {
      case LN_UIElementEvent::DropReceived: {
        /* This slot took the dragged one (the event's target): both grids must handle moves, so
         * no route moves the item a second time. */
        const ViewRegistration *target = HandlingGrid(event.object);
        const ViewRegistration *source = HandlingGrid(event.target);
        if (target == nullptr || source == nullptr) {
          break;
        }
        const LN_InventoryOutcome outcome = MoveGridItem(*target, *source, event);
        event.outcome = LN_InventoryOutcomeName(outcome, false);
        event.inventory_unchanged = outcome == LN_InventoryOutcome::None ||
                                    outcome == LN_InventoryOutcome::NoRoom;
        /* The dragged slot's Drop of the same release says the same. */
        for (ElementEvent &drop : m_elementEvents) {
          if (drop.type == LN_UIElementEvent::Drop && drop.accepted &&
              SameObjectHandle(drop.object, event.target) &&
              SameObjectHandle(drop.target, event.object))
          {
            drop.outcome = event.outcome;
          }
        }
        break;
      }
      case LN_UIElementEvent::DropOutside: {
        /* Let go over the player's view (else it became a Drag Cancel): in front of the list's
         * object, or with At Pointer on the floor below the pointer. */
        const ViewRegistration *view = HandlingGrid(event.object);
        if (view == nullptr || !view->inventory.drop_into_world || event.index < 0 ||
            !event.item.exists)
        {
          break;
        }
        const MT_Vector3 position =
            view->inventory.drop_at_pointer ?
                DropFloorPoint(event.point, event.normal, view->list_object) +
                    MT_Vector3(0.0f, 0.0f, view->inventory.hover_height) :
                InFrontDropPosition(*view);
        if (DropGridStack(*view, event.index, event.item, position, event.amount)) {
          /* Point says where it landed, in the Drop of the same release too. */
          event.outcome = "Dropped";
          event.point = position;
          for (ElementEvent &drop : m_elementEvents) {
            if (drop.type == LN_UIElementEvent::Drop && !drop.accepted &&
                drop.user == event.user && SameObjectHandle(drop.object, event.object))
            {
              drop.outcome = event.outcome;
              drop.point = position;
            }
          }
        }
        break;
      }
      case LN_UIElementEvent::SecondaryClick: {
        const ViewRegistration *view = HandlingGrid(event.object);
        if (view == nullptr || !view->inventory.drop_in_front || event.index < 0 ||
            !event.item.exists)
        {
          break;
        }
        /* With Split Action held, a part of it (also what the event's Amount says). */
        const int32_t amount = SplitAmount(view, event.item, event.user);
        if (amount > 0) {
          event.amount = amount;
        }
        const MT_Vector3 position = InFrontDropPosition(*view);
        if (DropGridStack(*view, event.index, event.item, position, amount)) {
          event.outcome = "Dropped";
          event.point = position;
        }
        break;
      }
      default:
        break;
    }
  }
}

void LN_UIService::PresentGridHints()
{
  for (auto &[object, hint] : m_hints) {
    hint.used = false;
  }
  for (const auto &pointers : m_pointerStates) {
    for (const auto &[user, pointer] : pointers) {
      if (!pointer.drag_active || !pointer.has_drop_target) {
        continue;
      }
      KX_GameObject *target = m_manager.ResolveObjectHandle(pointer.drop_object);
      const ViewRegistration *view = target != nullptr ? GridOfSlot(target) : nullptr;
      KX_GameObject *hint_object = view != nullptr ? view->inventory.hint_text : nullptr;
      if (hint_object == nullptr) {
        continue;
      }
      HintState &hint = m_hints[hint_object];
      if (m_manager.ResolveObjectHandle(hint.hint) != hint_object) {
        hint = HintState();
        hint.hint = m_manager.MakeObjectHandle(hint_object, hint_object->GetName());
      }
      if (hint.used) {
        /* Another player's drag speaks there this frame. */
        continue;
      }
      hint.used = true;
      KX_GameObject *dragged = m_manager.ResolveObjectHandle(pointer.captured_object);
      /* The words change only with the slot, the dragged item and whether the slot takes it:
       * kept while they don't, written again only if another writer emptied a shared line. */
      size_t key = std::hash<const void *>{}(target);
      HashCombine(key, static_cast<const void *>(dragged));
      HashCombine(key, pointer.drop_refused);
      HashCombine(key, pointer.has_drag_item ? ValueFingerprint(pointer.drag_item) : size_t(0));
      HashCombine(key, size_t(pointer.drag_amount));
      if (hint.key_known && hint.key == key) {
        if (!hint.text.empty() && CurrentUIText(hint_object, hint.text_object).empty()) {
          WriteUIText(hint_object, hint.text_object, hint.text);
        }
        continue;
      }
      hint.key = key;
      hint.key_known = true;
      /* The dragged item, found in the library of the grid it comes from. */
      const ViewRegistration *source = dragged != nullptr ? GridOfSlot(dragged) : nullptr;
      const blender::Collection *library = source != nullptr ? source->icon_library :
                                                               view->icon_library;
      const auto find_item = [&](const std::string &name) {
        return FindLibraryItem(library, name);
      };
      ItemTokens tokens;
      if (pointer.has_drag_item) {
        /* {count}: what a split stack's part carries. */
        tokens = GridItemTokens(m_manager,
                                m_scene,
                                pointer.drag_amount > 0 ?
                                    LN_InventoryEntryWithCount(pointer.drag_item,
                                                               pointer.drag_amount) :
                                    pointer.drag_item,
                                dragged,
                                find_item);
      }
      else if (dragged != nullptr) {
        tokens = ElementItemTokens(dragged, find_item);
      }
      std::string text = ExpandItemTokens(
          pointer.drop_refused ? view->inventory.refuse_hint : view->inventory.accept_hint,
          tokens);
      if (text != hint.text ||
          (!text.empty() && CurrentUIText(hint_object, hint.text_object).empty()))
      {
        WriteUIText(hint_object, hint.text_object, text);
        hint.text = std::move(text);
      }
    }
  }
  /* The drag left (or was dropped): empty what was written. */
  for (auto it = m_hints.begin(); it != m_hints.end();) {
    if (it->second.used) {
      ++it;
      continue;
    }
    KX_GameObject *hint_object = m_manager.ResolveObjectHandle(it->second.hint);
    if (hint_object != nullptr && !it->second.text.empty()) {
      ClearUIText(hint_object, it->second.text_object, it->second.text);
    }
    it = m_hints.erase(it);
  }
}

/** \} */

const LN_UIService::ElementEvent *LN_UIService::FindElementEvent(const LN_UIElementEvent type,
                                                                const std::string &action,
                                                                const int32_t user,
                                                                const size_t occurrence,
                                                                size_t *r_count,
                                                                const bool from_last) const
{
  const auto matches = [&](const ElementEvent &event) {
    return event.type == type && ActionListed(action, event.action) &&
           (user < 0 || event.user == user);
  };
  size_t count = 0;
  if (!m_passive && (r_count != nullptr || from_last)) {
    count = size_t(std::count_if(m_elementEvents.begin(), m_elementEvents.end(), matches));
  }
  if (r_count != nullptr) {
    *r_count = count;
  }
  const size_t considered = std::min(count, kMaxEventsPerNodeFrame);
  if (m_passive || (from_last && occurrence >= considered)) {
    return nullptr;
  }
  const size_t wanted = from_last ? considered - 1 - occurrence : occurrence;
  size_t index = 0;
  for (const ElementEvent &event : m_elementEvents) {
    if (matches(event) && index++ == wanted) {
      return &event;
    }
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Element extras: Highlight Object, widgets and UI sounds
 * \{ */

namespace {

/** A widget's game property as the widget sees it. */
struct WidgetValue {
  enum class Kind : uint8_t { None, Bool, Int, Float, Text };
  Kind kind = Kind::None;
  double number = 0.0;
  std::string text;
};

WidgetValue ReadWidgetValue(KX_GameObject *owner, const std::string &property)
{
  WidgetValue result;
  EXP_Value *value = owner != nullptr && !property.empty() ? owner->GetProperty(property) :
                                                             nullptr;
  if (value == nullptr) {
    return result;
  }
  switch (value->GetValueType()) {
    case VALUE_BOOL_TYPE:
      result.kind = WidgetValue::Kind::Bool;
      result.number = value->GetNumber() != 0.0 ? 1.0 : 0.0;
      break;
    case VALUE_INT_TYPE:
      result.kind = WidgetValue::Kind::Int;
      result.number = value->GetNumber();
      break;
    case VALUE_FLOAT_TYPE:
      result.kind = WidgetValue::Kind::Float;
      result.number = value->GetNumber();
      break;
    case VALUE_STRING_TYPE:
      result.kind = WidgetValue::Kind::Text;
      result.text = value->GetText();
      break;
    default:
      break;
  }
  if (!std::isfinite(result.number)) {
    result.number = 0.0;
  }
  return result;
}

LN_Value WidgetLNValue(const WidgetValue &value)
{
  LN_Value result;
  result.exists = true;
  switch (value.kind) {
    case WidgetValue::Kind::Bool:
      result.type = LN_ValueType::Bool;
      result.bool_value = value.number != 0.0;
      break;
    case WidgetValue::Kind::Int:
      result.type = LN_ValueType::Int;
      result.int_value = int32_t(std::clamp(std::round(value.number), -2147483648.0, 2147483647.0));
      break;
    case WidgetValue::Kind::Float:
      result.type = LN_ValueType::Float;
      result.float_value = float(value.number);
      break;
    case WidgetValue::Kind::Text:
      result.type = LN_ValueType::String;
      result.string_value = value.text;
      break;
    case WidgetValue::Kind::None:
      result.exists = false;
      break;
  }
  return result;
}

/** The same value as the property stores it (a Float property holds a float). */
bool SameWidgetValue(const WidgetValue &a, const WidgetValue &b)
{
  if (a.kind != b.kind) {
    return false;
  }
  switch (a.kind) {
    case WidgetValue::Kind::Text:
      return a.text == b.text;
    case WidgetValue::Kind::Float:
      return float(a.number) == float(b.number);
    case WidgetValue::Kind::Bool:
    case WidgetValue::Kind::Int:
      return std::round(a.number) == std::round(b.number);
    case WidgetValue::Kind::None:
      break;
  }
  return true;
}

std::string WidgetText(const WidgetValue &value)
{
  switch (value.kind) {
    case WidgetValue::Kind::Bool:
      return value.number != 0.0 ? "On" : "Off";
    case WidgetValue::Kind::Int:
      return std::to_string(int64_t(std::round(value.number)));
    case WidgetValue::Kind::Float: {
      char text[32];
      std::snprintf(text, sizeof(text), "%.2f", value.number);
      return text;
    }
    case WidgetValue::Kind::Text:
      return value.text;
    case WidgetValue::Kind::None:
      break;
  }
  return std::string();
}

/** A Selector's options: a Choice game property's option names, read from its object. */
std::vector<std::string> ChoiceOptions(KX_GameObject *owner, const std::string &property)
{
  std::vector<std::string> options;
  blender::Object *object = owner != nullptr ? owner->GetBlenderObject() : nullptr;
  const blender::bProperty *prop = object != nullptr ?
                                       blender::BKE_bproperty_object_get(object, property.c_str()) :
                                       nullptr;
  if (prop == nullptr || prop->type != GPROP_CHOICE) {
    return options;
  }
  for (const blender::bProperty *item = static_cast<const blender::bProperty *>(prop->items.first);
       item != nullptr;
       item = item->next)
  {
    options.emplace_back(item->name);
  }
  return options;
}

/** The track's long axis (x or y), and the axis across it (its thinnest, the depth), as the
 * object is drawn (its scale included). */
void SliderAxes(const MT_Vector3 &bounds_min,
                const MT_Vector3 &bounds_max,
                const MT_Vector3 &scale,
                int &r_axis,
                int &r_normal)
{
  MT_Vector3 size = bounds_max - bounds_min;
  for (int axis = 0; axis < 3; ++axis) {
    size[axis] *= std::abs(scale[axis]);
  }
  r_axis = size.x() >= size.y() ? 0 : 1;
  r_normal = 2;
  for (int axis = 0; axis < 3; ++axis) {
    if (size[axis] < size[r_normal]) {
      r_normal = axis;
    }
  }
  if (r_normal == r_axis) {
    r_normal = 2;
  }
}

/** `part` is `element` or one of its parents: hiding it would hide the element too. */
bool HoldsElement(KX_GameObject *part, KX_GameObject *element)
{
  for (KX_GameObject *object = element; object != nullptr; object = object->GetParent()) {
    if (object == part) {
      return true;
    }
  }
  return false;
}

}  // namespace

KX_GameObject *LN_UIService::ElementPart(KX_GameObject *element,
                                         blender::Object *source,
                                         PartCache &cache) const
{
  if (element == nullptr || source == nullptr) {
    return nullptr;
  }
  /* Found once by name (names do not change); a part not found is looked for again only now and
   * then, not every frame. */
  if (KX_GameObject *cached = m_manager.ResolveObjectHandle(cache.handle)) {
    return cached;
  }
  if (cache.missed_frame != UINT64_MAX && m_frameIndex - cache.missed_frame < 30) {
    return nullptr;
  }
  /* Copies (menus, grid slots) render temporary Blender objects but keep the authored name. */
  const std::string name = source->id.name + 2;
  if (element->GetName() == name) {
    cache.handle = m_manager.MakeObjectHandle(element, name);
    return element;
  }
  /* A child made from it, so each copy has its own; else the active object of that name. */
  KX_GameObject *found = nullptr;
  std::vector<SG_Node *> stack;
  if (element->GetSGNode() != nullptr) {
    stack.push_back(element->GetSGNode());
  }
  while (!stack.empty() && found == nullptr) {
    SG_Node *node = stack.back();
    stack.pop_back();
    for (SG_Node *child : node->GetSGChildren()) {
      KX_GameObject *object = static_cast<KX_GameObject *>(child->GetSGClientObject());
      if (object != nullptr && object->GetName() == name &&
          !m_manager.IsGameObjectQueuedForRemoval(object))
      {
        found = object;
        break;
      }
      stack.push_back(child);
    }
  }
  if (found == nullptr) {
    /* In a player's copy of a menu, that copy's object of the name. */
    found = m_scene.GetObjectList()->FindValue(name);
    if (found != nullptr) {
      found = m_scene.OverlayCounterpart(found, element);
    }
  }
  if (found != nullptr) {
    cache.handle = m_manager.MakeObjectHandle(found, name);
    cache.missed_frame = UINT64_MAX;
  }
  else {
    cache.missed_frame = m_frameIndex;
  }
  return found;
}

bool LN_UIService::StepFocusedWidget(KX_GameObject *focused,
                                     const LN_UIAction action,
                                     const int32_t user)
{
  if (action != LN_UIAction::NavigateLeft && action != LN_UIAction::NavigateRight) {
    return false;
  }
  const ElementRecord *record = focused != nullptr ? FindElementRecord(focused) : nullptr;
  if (record == nullptr || (record->settings.widget != LN_UIWidget::Slider &&
                            record->settings.widget != LN_UIWidget::Selector))
  {
    return false;
  }
  WidgetSteps &steps = m_widgetSteps[focused];
  steps.steps += action == LN_UIAction::NavigateRight ? 1 : -1;
  steps.user = user;
  return true;
}

void LN_UIService::ResolveWidgets()
{
  m_widgetChangedOwners.clear();
  ResolveTextEditing();
  const size_t phase = PhaseIndex(m_event);
  for (ElementRecord &record : m_elements) {
    const ElementSettings &settings = record.settings;
    KX_GameObject *element = record.object;
    if (settings.widget == LN_UIWidget::None || settings.widget_property.empty() ||
        !record.registered || m_manager.ResolveObjectHandle(record.handle) != element)
    {
      continue;
    }
    KX_GameObject *owner = WidgetOwner(record);
    if (owner == nullptr) {
      continue;
    }
    /* The player who clicked it (Any Player: the first of them), else who stepped it. */
    int32_t clicked_by = -1;
    for (int32_t user = 0; user < LN_MAX_GAMEPADS && clicked_by < 0; ++user) {
      if ((record.users & UserBit(user)) != 0) {
        const auto state_item = m_currentStates.find(ElementListener(element, user));
        if (state_item != m_currentStates.end() && state_item->second.clicked) {
          clicked_by = user;
        }
      }
    }
    const bool clicked = clicked_by >= 0;
    const auto step_item = m_widgetSteps.find(element);
    const int32_t steps = step_item != m_widgetSteps.end() ? step_item->second.steps : 0;
    int32_t used_by = clicked                         ? clicked_by :
                      step_item != m_widgetSteps.end() ? step_item->second.user :
                                                         0;
    const WidgetValue current = ReadWidgetValue(owner, settings.widget_property);
    WidgetValue next = current;
    /* A press of Left or Right: Int properties step by whole numbers. */
    const auto step_size = [&](const double step) {
      return current.kind == WidgetValue::Kind::Int ? std::max(1.0, std::round(step)) : step;
    };
    switch (settings.widget) {
      case LN_UIWidget::Toggle:
        if (clicked && current.kind != WidgetValue::Kind::Text) {
          next.kind = current.kind == WidgetValue::Kind::None ? WidgetValue::Kind::Bool :
                                                                current.kind;
          next.number = current.number != 0.0 ? 0.0 : 1.0;
        }
        break;
      case LN_UIWidget::Text:
        /* A click (or Accept on it) starts typing into it; ResolveTextEditing does the rest. */
        if (clicked_by == 0 && !SameObjectHandle(m_editingText, record.handle)) {
          m_editingText = record.handle;
          m_editingTextFrame = m_frameIndex;
          m_textEditCancelled = false;
        }
        break;
      case LN_UIWidget::Selector: {
        const int32_t move = steps + (clicked ? 1 : 0);
        if (move == 0) {
          break;
        }
        if (current.kind == WidgetValue::Kind::Text) {
          const std::vector<std::string> options = ChoiceOptions(owner, settings.widget_property);
          if (options.empty()) {
            break;
          }
          const auto found = std::find(options.begin(), options.end(), current.text);
          const int32_t index = found != options.end() ? int32_t(found - options.begin()) : 0;
          const int32_t count = int32_t(options.size());
          next.text = options[size_t(((index + move) % count + count) % count)];
        }
        else if (current.kind == WidgetValue::Kind::Bool) {
          if (move % 2 != 0) {
            next.number = current.number != 0.0 ? 0.0 : 1.0;
          }
        }
        else if (current.kind == WidgetValue::Kind::Int ||
                 current.kind == WidgetValue::Kind::Float)
        {
          /* Numbers step through Min to Max and wrap around (no Step: by 1). */
          const double step = step_size(settings.widget_step > 0.0f ? settings.widget_step : 1.0);
          const double low = std::min(settings.widget_min, settings.widget_max);
          const double high = std::max(settings.widget_min, settings.widget_max);
          double value = current.number + move * step;
          if (value > high + 1.0e-6) {
            value = low;
          }
          else if (value < low - 1.0e-6) {
            value = high;
          }
          next.number = value;
        }
        break;
      }
      case LN_UIWidget::Slider: {
        if (current.kind != WidgetValue::Kind::Int && current.kind != WidgetValue::Kind::Float &&
            current.kind != WidgetValue::Kind::None)
        {
          break;
        }
        /* Held press on the track (player 0's mouse or a player's pointer cursor): the value
         * under the pointer, also off the track. */
        int32_t held_by = -1;
        for (const auto &[user, pointer] : m_pointerStates[phase]) {
          if (pointer.has_capture && !pointer.carried &&
              m_manager.ResolveObjectHandle(pointer.captured_object) == element)
          {
            held_by = user;
            break;
          }
        }
        const bool held = held_by >= 0;
        if (!held && steps == 0) {
          break;
        }
        if (held) {
          used_by = held_by;
        }
        next.kind = current.kind == WidgetValue::Kind::None ? WidgetValue::Kind::Float :
                                                              current.kind;
        const double low = settings.widget_min;
        const double high = settings.widget_max;
        double value = current.number;
        /* That player's registration has their pointer's ray. */
        const auto registration = std::find_if(
            m_registrations.begin(), m_registrations.end(), [&](const Registration &item) {
              return item.object == element && item.user == held_by;
            });
        MT_Vector3 bounds_min;
        MT_Vector3 bounds_max;
        if (held && registration != m_registrations.end() &&
            ObjectBounds(element, bounds_min, bounds_max))
        {
          int axis = 0;
          int normal = 2;
          SliderAxes(bounds_min, bounds_max, element->NodeGetWorldScaling(), axis, normal);
          MT_Vector3 from;
          MT_Vector3 to;
          const float length = bounds_max[axis] - bounds_min[axis];
          if (length > kScaleEpsilon && WorldToObjectSpace(element, registration->ray_from, from) &&
              WorldToObjectSpace(element, registration->ray_to, to))
          {
            const float depth = to[normal] - from[normal];
            if (std::abs(depth) > kScaleEpsilon) {
              const float plane = (bounds_min[normal] + bounds_max[normal]) * 0.5f;
              const MT_Vector3 point = from + (to - from) * ((plane - from[normal]) / depth);
              const float t = std::clamp((point[axis] - bounds_min[axis]) / length, 0.0f, 1.0f);
              if (std::isfinite(t)) {
                value = low + (high - low) * t;
              }
            }
          }
        }
        /* No Step: a twentieth of the range per press, and no snapping. */
        const double step = settings.widget_step > 0.0f ? double(settings.widget_step) :
                                                           (high - low) / 20.0;
        value += steps * step_size(step);
        if (settings.widget_step > 0.0f) {
          value = low + std::round((value - low) / step) * step;
        }
        value = std::clamp(value, std::min(low, high), std::max(low, high));
        if (next.kind == WidgetValue::Kind::Int) {
          value = std::round(value);
        }
        next.number = value;
        break;
      }
      case LN_UIWidget::None:
        break;
    }
    if (SameWidgetValue(next, current)) {
      continue;
    }
    const LN_Value value = WidgetLNValue(next);
    std::string reason;
    if (!LN_CommandBuffer::ApplyGamePropertyValue(
            owner, settings.widget_property, value, nullptr, reason))
    {
      CM_Warning("UI Element '" << element->GetName() << "' widget: " << reason);
      continue;
    }
    m_widgetChangedOwners.push_back(m_manager.MakeObjectHandle(owner, owner->GetName()));
    ElementEvent &event = m_elementEvents.emplace_back();
    event.type = LN_UIElementEvent::Changed;
    event.object = record.handle;
    event.point = element->NodeGetWorldPosition();
    event.action = settings.action;
    event.menu_index = record.menu_index;
    event.domain_rank = record.domain_rank;
    event.priority = settings.priority;
    event.user = used_by;
    event.item = value;
  }
  m_widgetSteps.clear();
}

void LN_UIService::ResolveTextEditing()
{
  KX_GameObject *element = m_manager.ResolveObjectHandle(m_editingText);
  ElementRecord *record = element != nullptr ? FindElementRecord(element) : nullptr;
  if (record == nullptr || !record->registered || record->settings.widget != LN_UIWidget::Text ||
      record->settings.widget_property.empty() || !element->GetVisible() || m_input == nullptr)
  {
    m_editingText = LN_ObjectHandle();
    m_textEditCancelled = false;
    return;
  }
  /* The click or Accept that started typing is not typed. */
  if (m_editingTextFrame == m_frameIndex) {
    return;
  }
  const ElementSettings &settings = record->settings;
  KX_GameObject *owner = WidgetOwner(*record);
  const WidgetValue current = owner != nullptr ? ReadWidgetValue(owner, settings.widget_property) :
                                                 WidgetValue();
  if (owner == nullptr ||
      (current.kind != WidgetValue::Kind::Text && current.kind != WidgetValue::Kind::None))
  {
    CM_Warning("UI Element '" << element->GetName()
                              << "' Text Field: its game property must be a String");
    m_editingText = LN_ObjectHandle();
    return;
  }
  std::string text = current.text;
  const size_t max_length = settings.widget_max >= 1.0f ? size_t(settings.widget_max) : SIZE_MAX;
  size_t length = blender::BLI_strlen_utf8(text.c_str());
  for (const wchar_t character : m_input->GetTextInput()) {
    const uint32_t code = uint32_t(character);
    if (code < 32 || code == 127 || (code >= 0xD800 && code <= 0xDFFF) || length >= max_length) {
      continue;
    }
    char bytes[8];
    text.append(bytes, blender::BLI_str_utf8_from_unicode(code, bytes, sizeof(bytes)));
    length++;
  }
  /* Backspace takes the last character; held, it repeats. */
  const auto remove_last = [&]() {
    size_t end = text.size();
    while (end > 0 && (uint8_t(text[end - 1]) & 0xC0) == 0x80) {
      end--;
    }
    text.resize(end > 0 ? end - 1 : 0);
  };
  if (m_input->GetStatus(SCA_IInputDevice::BACKSPACEKEY, SCA_InputEvent::JUSTACTIVATED)) {
    remove_last();
    m_backspaceHeld = 0.0;
    m_backspaceNext = 0.45;
  }
  else if (m_input->GetStatus(SCA_IInputDevice::BACKSPACEKEY, SCA_InputEvent::ACTIVE)) {
    m_backspaceHeld += m_frameDelta;
    if (m_backspaceHeld >= m_backspaceNext) {
      remove_last();
      m_backspaceNext = std::max(m_backspaceNext, m_backspaceHeld) + 0.05;
    }
  }
  LN_Value value;
  value.type = LN_ValueType::String;
  value.exists = true;
  value.string_value = text;
  if (text != current.text || current.kind == WidgetValue::Kind::None) {
    std::string reason;
    if (LN_CommandBuffer::ApplyGamePropertyValue(
            owner, settings.widget_property, value, nullptr, reason))
    {
      m_widgetChangedOwners.push_back(m_manager.MakeObjectHandle(owner, owner->GetName()));
    }
  }
  /* Every key pressed while typing is the text's: game logic does not see it either. */
  for (int32_t code = SCA_IInputDevice::BEGINKEY + 1; code < SCA_IInputDevice::ENDKEY; ++code) {
    if (m_input->GetStatus(code, SCA_InputEvent::JUSTACTIVATED)) {
      m_manager.ConsumeInputStroke(code);
    }
  }
  const bool enter =
      m_input->GetStatus(SCA_IInputDevice::RETKEY, SCA_InputEvent::JUSTACTIVATED) ||
      m_input->GetStatus(SCA_IInputDevice::PADENTER, SCA_InputEvent::JUSTACTIVATED);
  const bool clicked_away =
      m_input->GetStatus(SCA_IInputDevice::LEFTMOUSE, SCA_InputEvent::JUSTACTIVATED) &&
      m_manager.ResolveObjectHandle(m_pointerHovers[0]) != element;
  if (!enter && !m_textEditCancelled && !clicked_away) {
    return;
  }
  /* Finished: the text is told once, as a Changed event. */
  m_editingText = LN_ObjectHandle();
  m_textEditCancelled = false;
  ElementEvent &event = m_elementEvents.emplace_back();
  event.type = LN_UIElementEvent::Changed;
  event.object = record->handle;
  event.point = element->NodeGetWorldPosition();
  event.action = settings.action;
  event.menu_index = record->menu_index;
  event.domain_rank = record->domain_rank;
  event.priority = settings.priority;
  /* Only player 0 has the keyboard to type with. */
  event.user = 0;
  event.item = value;
}

void LN_UIService::PresentWidget(ElementRecord &record)
{
  const ElementSettings &settings = record.settings;
  KX_GameObject *element = record.object;
  if (settings.widget == LN_UIWidget::None || settings.widget == LN_UIWidget::Bar ||
      settings.widget_property.empty() || settings.widget_part == nullptr)
  {
    return;
  }
  KX_GameObject *owner = WidgetOwner(record);
  KX_GameObject *part = ElementPart(element, settings.widget_part, record.widget_part);
  /* A check mark or handle that is the element (or holds it) would hide or move it too; a
   * Selector's or Text Field's text may be the element itself. */
  if (owner == nullptr || part == nullptr ||
      (!ELEM(settings.widget, LN_UIWidget::Selector, LN_UIWidget::Text) &&
       HoldsElement(part, element)))
  {
    return;
  }
  const WidgetValue value = ReadWidgetValue(owner, settings.widget_property);
  const bool typing = settings.widget == LN_UIWidget::Text &&
                      SameObjectHandle(m_editingText, record.handle);
  size_t shown = size_t(value.kind);
  HashCombine(shown, value.number);
  HashCombine(shown, value.text);
  HashCombine(shown, typing);
  /* Game logic showing the element with its children shows an off Toggle's mark too. */
  const bool shown_again = settings.widget == LN_UIWidget::Toggle && value.number == 0.0 &&
                           part->GetVisibleIgnoringViewClip();
  if (record.widget_shown_known && shown == record.widget_shown && !shown_again) {
    return;
  }
  bool placed = true;
  switch (settings.widget) {
    case LN_UIWidget::Toggle:
      part->SetVisible(value.number != 0.0, true);
      break;
    case LN_UIWidget::Selector:
    case LN_UIWidget::Text: {
      /* A text object shows its Text game property; a Text Field being typed into shows a
       * caret. */
      LN_Value text;
      text.type = LN_ValueType::String;
      text.exists = true;
      text.string_value = settings.widget == LN_UIWidget::Text ? value.text + (typing ? "_" : "") :
                                                                  WidgetText(value);
      /* What a player typed shows as typed, braces too. */
      if (part->GetGameObjectType() == SCA_IObject::OBJ_TEXT) {
        static_cast<KX_FontObject *>(part)->SetLiteralText(text.string_value);
      }
      std::string reason;
      LN_CommandBuffer::ApplyGamePropertyValue(part, "Text", text, nullptr, reason);
      break;
    }
    case LN_UIWidget::Slider: {
      /* Not placed yet (a track popping in from scale 0): try again next frame. */
      MT_Vector3 bounds_min;
      MT_Vector3 bounds_max;
      if (!ObjectBounds(element, bounds_min, bounds_max)) {
        placed = false;
        break;
      }
      if (!record.handle_known) {
        if (!HandleInBarSpace(element, part, record.handle_base, record.handle_scale)) {
          placed = false;
          break;
        }
        record.handle_known = true;
      }
      int axis = 0;
      int normal = 2;
      SliderAxes(bounds_min, bounds_max, element->NodeGetWorldScaling(), axis, normal);
      const double range = double(settings.widget_max) - double(settings.widget_min);
      const float t = std::abs(range) > 1.0e-9 ?
                          float(std::clamp((value.number - settings.widget_min) / range, 0.0, 1.0)) :
                          0.0f;
      MT_Vector3 position = record.handle_base;
      position[axis] = bounds_min[axis] + (bounds_max[axis] - bounds_min[axis]) * t;
      const MT_Vector3 track_scale = element->NodeGetWorldScaling();
      part->NodeSetWorldPosition(element->NodeGetWorldPosition() +
                                 element->NodeGetWorldOrientation() *
                                     MT_Vector3(position.x() * track_scale.x(),
                                                position.y() * track_scale.y(),
                                                position.z() * track_scale.z()));
      part->NodeUpdateGS(0.0);
      break;
    }
    case LN_UIWidget::None:
    case LN_UIWidget::Bar:
      break;
  }
  if (placed) {
    record.widget_shown = shown;
    record.widget_shown_known = true;
  }
}

KX_GameObject *LN_UIService::WidgetOwner(ElementRecord &record) const
{
  KX_GameObject *element = record.object;
  const ElementSettings &settings = record.settings;
  if (settings.widget_object != nullptr) {
    return ElementPart(element, settings.widget_object, record.widget_owner);
  }
  for (KX_GameObject *object = element; object != nullptr; object = object->GetParent()) {
    if (object->GetProperty(settings.widget_property) != nullptr) {
      return object;
    }
  }
  /* Else, in a menu, its owner: each player's copy of a per-player menu shows that player's. */
  if (KX_GameObject *menu_owner = MenuOwnerOfObject(element);
      menu_owner != nullptr && menu_owner->GetProperty(settings.widget_property) != nullptr)
  {
    return menu_owner;
  }
  return element;
}

void LN_UIService::PresentBar(ElementRecord &record, const float delta_time)
{
  const ElementSettings &settings = record.settings;
  KX_GameObject *element = record.object;
  if (settings.widget != LN_UIWidget::Bar || settings.widget_property.empty() ||
      settings.widget_part == nullptr)
  {
    return;
  }
  KX_GameObject *owner = WidgetOwner(record);
  KX_GameObject *fill = ElementPart(element, settings.widget_part, record.widget_part);
  /* A fill that is the bar (or holds it) would scale the bar itself. */
  if (owner == nullptr || fill == nullptr || HoldsElement(fill, element)) {
    return;
  }
  const auto numeric = [](const WidgetValue &value) {
    return ELEM(value.kind, WidgetValue::Kind::Bool, WidgetValue::Kind::Int, WidgetValue::Kind::Float);
  };
  const WidgetValue value = ReadWidgetValue(owner, settings.widget_property);
  double maximum = settings.widget_max;
  if (!settings.widget_max_property.empty()) {
    const WidgetValue full = ReadWidgetValue(owner, settings.widget_max_property);
    if (numeric(full)) {
      maximum = full.number;
    }
  }
  const double range = maximum - double(settings.widget_min);
  float target = 0.0f;
  /* No range (a max_health not set yet): empty. */
  if (numeric(value) && std::abs(range) > 1.0e-9) {
    target = float(std::clamp((value.number - settings.widget_min) / range, 0.0, 1.0));
  }
  else if (!numeric(value) &&
           m_barWarnings.insert(element->GetName() + "\x1fvalue").second)
  {
    CM_Warning("UI Element '" << element->GetName() << "' Bar: '" << owner->GetName()
                              << "' has no number property '" << settings.widget_property
                              << "', so the bar shows empty");
  }
  /* The fill as authored, once the bar and fill have a size (not when popping in from scale 0).
   * A copy of a bar (an enemy added, a grid slot) takes the shape of the first bar seen with its
   * names: its own fill may be shrunk already. */
  if (!record.handle_known) {
    /* A bar popping in from scale 0 has no space to place its fill in yet. */
    const MT_Vector3 bar_scale = element->NodeGetWorldScaling();
    if (std::abs(bar_scale.x()) <= kScaleEpsilon || std::abs(bar_scale.y()) <= kScaleEpsilon ||
        std::abs(bar_scale.z()) <= kScaleEpsilon)
    {
      return;
    }
    const std::string shape_key = element->GetName() + '\x1f' + fill->GetName();
    if (const auto shape = m_barShapes.find(shape_key); shape != m_barShapes.end()) {
      record.handle_base = shape->second.base;
      record.handle_scale = shape->second.scale;
      record.fill_scale = shape->second.fill_scale;
      record.fill_start = shape->second.start;
      record.fill_end = shape->second.end;
      record.fill_axis = shape->second.axis;
    }
    else {
      MT_Vector3 bar_min;
      MT_Vector3 bar_max;
      MT_Vector3 fill_min;
      MT_Vector3 fill_max;
      if (!ObjectBounds(element, bar_min, bar_max) || !ObjectBounds(fill, fill_min, fill_max) ||
          !HandleInBarSpace(element, fill, record.handle_base, record.handle_scale))
      {
        /* A size of 0 (popping in) gets one later; an Empty never does. */
        const auto sizeless = [](KX_GameObject *object) {
          const blender::Object *blender_object = object->GetBlenderObject();
          return blender_object == nullptr || blender_object->type == blender::OB_EMPTY;
        };
        if ((sizeless(element) || sizeless(fill)) &&
            m_barWarnings.insert(element->GetName() + "\x1f" "bounds").second)
        {
          CM_Warning("UI Element '" << element->GetName()
                                    << "' Bar: the bar and its Part need a size (an Empty has "
                                       "none): use meshes");
        }
        return;
      }
      int normal = 2;
      SliderAxes(bar_min, bar_max, element->NodeGetWorldScaling(), record.fill_axis, normal);
      const int axis = record.fill_axis;
      const float a = record.handle_base[axis] + fill_min[axis] * record.handle_scale[axis];
      const float b = record.handle_base[axis] + fill_max[axis] * record.handle_scale[axis];
      record.fill_start = std::min(a, b);
      record.fill_end = std::max(a, b);
      record.fill_scale = fill->NodeGetLocalScaling();
      m_barShapes.emplace(shape_key,
                          BarShape{record.handle_base,
                                   record.handle_scale,
                                   record.fill_scale,
                                   record.fill_start,
                                   record.fill_end,
                                   record.fill_axis});
    }
    record.handle_known = true;
  }
  /* A new value: the fill moves toward it over Look Time (0: at once). */
  float shown = target;
  if (record.bar_known && settings.look_time > 0.0f) {
    const float step = std::max(0.0f, delta_time) / settings.look_time;
    const float delta = target - record.bar_shown;
    shown = std::abs(delta) <= step ? target : record.bar_shown + std::copysign(step, delta);
  }
  if (record.bar_known && shown == record.bar_shown) {
    return;
  }
  record.bar_shown = shown;
  record.bar_known = true;
  /* Scaled along the bar about its start edge, which stays where it was authored (left, or the
   * bottom of an upright bar). Never quite 0: a zero scale has no inverse. */
  const int axis = record.fill_axis;
  const float factor = std::max(shown, 1.0e-4f);
  MT_Vector3 scale = record.fill_scale;
  scale[axis] *= factor;
  MT_Vector3 position = record.handle_base;
  position[axis] = record.fill_start + (record.handle_base[axis] - record.fill_start) * factor;
  const MT_Vector3 bar_scale = element->NodeGetWorldScaling();
  fill->NodeSetLocalScale(scale);
  fill->NodeSetWorldPosition(element->NodeGetWorldPosition() +
                             element->NodeGetWorldOrientation() *
                                 MT_Vector3(position.x() * bar_scale.x(),
                                            position.y() * bar_scale.y(),
                                            position.z() * bar_scale.z()));
  fill->NodeUpdateGS(0.0);
}

void LN_UIService::PresentHighlights()
{
  /* A Highlight Object shows while any element using it is highlighted: a child frame of each
   * element, or one object several elements share (a description box). The first element using
   * it remembers what was applied, so it is set once however many elements share it. */
  m_highlightFrames.clear();
  m_refusedFrames.clear();
  /* A slot whose Accept Categories refuse what the mouse drags over it does not light up, so it
   * does not look like a place to drop. A carry keeps it: there it shows where the focus is. Its
   * Refused Object shows for both. */
  std::unordered_set<const KX_GameObject *> refusing;
  /* The refusing drop targets, with the player and the dragged element of a refusal. */
  struct Refusal {
    int32_t user = 0;
    LN_ObjectHandle dragged;
  };
  std::unordered_map<const KX_GameObject *, Refusal> refused;
  for (const auto &pointers : m_pointerStates) {
    for (const auto &[user, pointer] : pointers) {
      if (pointer.drag_active && pointer.has_drop_target && pointer.drop_refused) {
        const KX_GameObject *target = m_manager.ResolveObjectHandle(pointer.drop_object);
        refused.try_emplace(target, Refusal{user, pointer.captured_object});
        if (!pointer.carried) {
          refusing.insert(target);
        }
      }
    }
  }
  /* Highlight Start/End and Refused Start/End: the same states the frames below show, told once
   * when they change, for every element (an animation needs no frame object). */
  const auto add_state_event = [&](ElementRecord &record,
                                   const LN_UIElementEvent type,
                                   const int32_t user,
                                   const LN_ObjectHandle &target) {
    ElementEvent &event = m_elementEvents.emplace_back();
    event.type = type;
    event.object = record.handle;
    event.target = target;
    event.point = record.object->NodeGetWorldPosition();
    event.action = record.settings.action;
    event.menu_index = record.menu_index;
    event.domain_rank = record.domain_rank;
    event.priority = record.settings.priority;
    event.user = user;
    GetBoundItem(record.object, event.item, event.index);
    KX_GameObject *target_object = m_manager.ResolveObjectHandle(target);
    if (target_object != nullptr) {
      GetBoundItem(target_object, event.target_item, event.target_index);
      if (const ElementRecord *target_record = FindElementRecord(target_object)) {
        event.target_action = target_record->settings.action;
      }
    }
    const LN_Value &about = target_object != nullptr ? event.target_item : event.item;
    std::string name;
    if (!LN_InventoryShownEntry(about, name, event.amount)) {
      event.amount = about.exists && about.type == LN_ValueType::ObjectRef ? 1 : 0;
    }
  };
  for (ElementRecord &record : m_elements) {
    if (m_manager.ResolveObjectHandle(record.handle) != record.object) {
      continue;
    }
    int32_t lit_user = -1;
    if (record.registered && !refusing.contains(record.object)) {
      for (const auto &[user, highlight] : m_highlights) {
        if (SameObjectHandle(highlight.object, record.handle) && (lit_user < 0 || user < lit_user))
        {
          lit_user = user;
        }
      }
    }
    if ((lit_user >= 0) != record.lit) {
      record.lit = lit_user >= 0;
      record.lit_user = record.lit ? lit_user : record.lit_user;
      add_state_event(record,
                      record.lit ? LN_UIElementEvent::HighlightStart :
                                   LN_UIElementEvent::HighlightEnd,
                      record.lit_user,
                      LN_ObjectHandle());
    }
    const auto refusal = record.registered ? refused.find(record.object) : refused.end();
    if ((refusal != refused.end()) != record.refused_lit) {
      record.refused_lit = refusal != refused.end();
      if (record.refused_lit) {
        record.refused_user = refusal->second.user;
        record.refused_by = refusal->second.dragged;
      }
      add_state_event(record,
                      record.refused_lit ? LN_UIElementEvent::RefusedStart :
                                           LN_UIElementEvent::RefusedEnd,
                      record.refused_user,
                      record.refused_by);
    }
  }
  for (ElementRecord &record : m_elements) {
    if ((record.settings.highlight == nullptr && record.settings.refused == nullptr) ||
        m_manager.ResolveObjectHandle(record.handle) != record.object)
    {
      continue;
    }
    KX_GameObject *frame = ElementPart(
        record.object, record.settings.highlight, record.highlight_object);
    if (frame != nullptr && !HoldsElement(frame, record.object)) {
      bool shown = false;
      if (record.registered && !refusing.contains(record.object)) {
        for (const auto &[user, highlight] : m_highlights) {
          shown = shown || SameObjectHandle(highlight.object, record.handle);
        }
      }
      HighlightFrame &entry = m_highlightFrames[frame];
      if (entry.record == nullptr) {
        entry.record = &record;
      }
      entry.shown = entry.shown || shown;
    }
    KX_GameObject *refused_frame = ElementPart(
        record.object, record.settings.refused, record.refused_object);
    if (refused_frame != nullptr && !HoldsElement(refused_frame, record.object)) {
      HighlightFrame &entry = m_refusedFrames[refused_frame];
      if (entry.record == nullptr) {
        entry.record = &record;
      }
      entry.shown = entry.shown || (record.registered && refused.contains(record.object));
    }
  }
  for (const auto &[frame, entry] : m_highlightFrames) {
    ElementRecord &record = *entry.record;
    /* Game logic showing the element with its children shows the frame too. */
    const bool shown_again = !entry.shown && frame->GetVisibleIgnoringViewClip();
    if (!record.highlight_known || entry.shown != record.highlight_shown || shown_again) {
      frame->SetVisible(entry.shown, true);
      record.highlight_shown = entry.shown;
      record.highlight_known = true;
    }
  }
  for (const auto &[frame, entry] : m_refusedFrames) {
    ElementRecord &record = *entry.record;
    const bool shown_again = !entry.shown && frame->GetVisibleIgnoringViewClip();
    if (!record.refused_known || entry.shown != record.refused_shown || shown_again) {
      frame->SetVisible(entry.shown, true);
      record.refused_shown = entry.shown;
      record.refused_known = true;
    }
  }
}

KX_GameObject *LN_UIService::UITextObject(KX_GameObject *object, LN_ObjectHandle &text_object)
{
  if (KX_GameObject *target = m_manager.ResolveObjectHandle(text_object)) {
    return target;
  }
  KX_GameObject *target = nullptr;
  if (object->GetGameObjectType() != SCA_IObject::OBJ_TEXT) {
    for (KX_GameObject *child : object->GetChildrenRecursive()) {
      if (child != nullptr && child->GetGameObjectType() == SCA_IObject::OBJ_TEXT &&
          !m_manager.IsGameObjectQueuedForRemoval(child))
      {
        target = child;
        break;
      }
    }
  }
  /* No text object: the object's own "Text" game property holds it (for game logic). */
  if (target == nullptr) {
    target = object;
  }
  text_object = m_manager.MakeObjectHandle(target, target->GetName());
  return target;
}

void LN_UIService::WriteUIText(KX_GameObject *object,
                               LN_ObjectHandle &text_object,
                               const std::string &text)
{
  /* Text objects show their "Text" property; braces left in it work as in any text. */
  EXP_StringValue *value = new EXP_StringValue(text, "Text");
  UITextObject(object, text_object)->SetProperty("Text", value);
  value->Release();
}

std::string LN_UIService::CurrentUIText(KX_GameObject *object, LN_ObjectHandle &text_object)
{
  EXP_Value *current = UITextObject(object, text_object)->GetProperty("Text");
  return current != nullptr ? current->GetText() : std::string();
}

void LN_UIService::ClearUIText(KX_GameObject *object,
                               LN_ObjectHandle &text_object,
                               const std::string &written)
{
  if (CurrentUIText(object, text_object) == written) {
    WriteUIText(object, text_object, std::string());
  }
}

bool LN_UIService::ShowMessage(KX_GameObject *object,
                               const int32_t user,
                               const std::string &text,
                               const double seconds,
                               const int32_t lines,
                               const int32_t count)
{
  /* A menu object opened as menus: the copy in the player's own menu, none without one. */
  blender::Collection *collection = object != nullptr && user >= 0 ?
                                        m_scene.GetOverlayCollectionOf(object) :
                                        nullptr;
  if (collection != nullptr) {
    KX_GameObject *copy = object;
    for (const auto &[handle, session] : m_sessions) {
      if (session.collection != collection) {
        continue;
      }
      copy = nullptr;
      if (session.user == user) {
        for (const LN_ObjectHandle &member : session.objects) {
          if (KX_GameObject *viewer = m_manager.ResolveObjectHandle(member)) {
            copy = m_scene.OverlayCounterpart(object, viewer);
            break;
          }
        }
        break;
      }
    }
    object = copy;
  }
  if (object == nullptr || m_manager.IsGameObjectQueuedForRemoval(object)) {
    return false;
  }
  /* Nothing to say: an empty text, or none of it (a pickup that did not fit). */
  if (text.empty() || count == 0) {
    return true;
  }
  auto box = std::find_if(m_messages.begin(), m_messages.end(), [&](const MessageBox &item) {
    return m_manager.ResolveObjectHandle(item.object) == object;
  });
  if (box == m_messages.end()) {
    box = m_messages.emplace(m_messages.end());
    box->object = m_manager.MakeObjectHandle(object, object->GetName());
  }
  /* The same text again: its line moves to the end, starts over and adds up the counts (a pickup
   * feed: "Apple x2", then "Apple x5"). */
  int64_t total = count > 0 ? count : -1;
  const auto same = std::find_if(box->lines.begin(),
                                 box->lines.end(),
                                 [&](const MessageBox::Line &line) { return line.text == text; });
  if (same != box->lines.end()) {
    total = same->count > 0 ? std::max<int64_t>(total, 0) + same->count : total;
    box->lines.erase(same);
  }
  box->lines.push_back(
      {text, seconds > 0.0 ? seconds : std::numeric_limits<double>::infinity(), total});
  const size_t keep = size_t(std::max(lines, 1));
  if (box->lines.size() > keep) {
    box->lines.erase(box->lines.begin(), box->lines.end() - ptrdiff_t(keep));
  }
  box->written = MessageText(*box);
  WriteUIText(object, box->text_object, box->written);
  return true;
}

std::string LN_UIService::MessageText(const MessageBox &box)
{
  static constexpr std::string_view kCountToken = "{count}";
  std::string text;
  for (const MessageBox::Line &line : box.lines) {
    std::string shown = line.text;
    /* {count} shows the total where the text puts it; else " x5" after it (nothing for 1). A
     * message without a count stays as written. */
    if (line.count > 0 && shown.find(kCountToken) != std::string::npos) {
      const std::string number = std::to_string(line.count);
      for (size_t at = shown.find(kCountToken); at != std::string::npos;
           at = shown.find(kCountToken, at + number.size()))
      {
        shown.replace(at, kCountToken.size(), number);
      }
    }
    else if (line.count > 1) {
      shown += " x" + std::to_string(line.count);
    }
    text += (text.empty() ? "" : "\n") + shown;
  }
  return text;
}

void LN_UIService::PresentMessages(const double delta_time)
{
  std::erase_if(m_messages, [&](MessageBox &box) {
    KX_GameObject *object = m_manager.ResolveObjectHandle(box.object);
    if (object == nullptr || m_manager.IsGameObjectQueuedForRemoval(object)) {
      return true;
    }
    for (MessageBox::Line &line : box.lines) {
      line.seconds -= delta_time;
    }
    if (std::erase_if(box.lines, [](const MessageBox::Line &line) { return line.seconds <= 0.0; }) ==
        0)
    {
      return false;
    }
    /* Game logic wrote over the messages: theirs stays. */
    if (CurrentUIText(object, box.text_object) != box.written) {
      return box.lines.empty();
    }
    box.written = MessageText(box);
    WriteUIText(object, box.text_object, box.written);
    return box.lines.empty();
  });
}

void LN_UIService::PresentTooltips()
{
  m_tooltipFrames.clear();
  for (ElementRecord &record : m_elements) {
    if (record.settings.tooltip == nullptr ||
        m_manager.ResolveObjectHandle(record.handle) != record.object)
    {
      continue;
    }
    KX_GameObject *tooltip = ElementPart(
        record.object, record.settings.tooltip, record.tooltip_object);
    if (tooltip == nullptr || HoldsElement(tooltip, record.object)) {
      continue;
    }
    TooltipFrame &frame = m_tooltipFrames[tooltip];
    if (frame.first == nullptr) {
      frame.first = &record;
    }
    if (!record.registered) {
      continue;
    }
    /* The first player pointing at it; a grid slot says something only while it holds an item.
     * Not while that player drags or carries something: the tooltip would cover where it goes
     * (the drop hint says what letting go there does). */
    for (int32_t user = 0; user < LN_MAX_GAMEPADS; ++user) {
      const auto highlight = m_highlights.find(user);
      if (highlight == m_highlights.end() ||
          !SameObjectHandle(highlight->second.object, record.handle) || UserDrags(user))
      {
        continue;
      }
      LN_Value item;
      int32_t index = -1;
      if ((frame.shown == nullptr || user < frame.user) &&
          (!GetBoundItem(record.object, item, index) || item.exists))
      {
        frame.shown = &record;
        frame.user = user;
      }
      break;
    }
  }
  for (auto &[tooltip, frame] : m_tooltipFrames) {
    TooltipState &state = m_tooltips[tooltip];
    if (m_manager.ResolveObjectHandle(state.tooltip) != tooltip) {
      state = TooltipState();
      state.tooltip = m_manager.MakeObjectHandle(tooltip, tooltip->GetName());
    }
    const ElementRecord &record = frame.shown != nullptr ? *frame.shown : *frame.first;
    const ElementSettings &settings = record.settings;
    state.mode = settings.tooltip_mode;
    if (frame.shown == nullptr) {
      if (settings.tooltip_mode == LN_UITooltipMode::TextOnly) {
        /* Only what the tooltip wrote: an info line keeps its own text until then. */
        if (!state.text.empty()) {
          ClearUIText(tooltip, state.text_object, state.text);
          state.text.clear();
        }
      }
      else if (!state.visibility_known || state.shown || tooltip->GetVisibleIgnoringViewClip()) {
        /* Also when game logic showed it with its parent's children. */
        tooltip->SetVisible(false, true);
        state.shown = false;
        state.visibility_known = true;
      }
      continue;
    }
    /* Made every frame: the words follow the item and the properties they show ({count},
     * {price}). */
    KX_GameObject *element = record.object;
    LN_Value item;
    int32_t index = -1;
    const bool slot = GetBoundItem(element, item, index);
    const ViewRegistration *view = slot ? GridOfSlot(element) : nullptr;
    const blender::Collection *library = view != nullptr ? view->icon_library : nullptr;
    const auto find_item = [&](const std::string &name) { return FindLibraryItem(library, name); };
    std::string text = ExpandItemTokens(
        settings.tooltip_text,
        slot ? GridItemTokens(m_manager, m_scene, item, element, find_item) :
               ElementItemTokens(element, find_item));
    /* Written when it changed, or when another writer emptied a shared line (a hint that ended).
     * A message game logic wrote over it stays until the words change. */
    if (!state.text_known || text != state.text ||
        (!text.empty() && CurrentUIText(tooltip, state.text_object).empty()))
    {
      WriteUIText(tooltip, state.text_object, text);
      state.text = std::move(text);
      state.text_known = true;
    }
    if (settings.tooltip_mode == LN_UITooltipMode::Beside) {
      /* The offset is in the tooltip's parent space: it scales with a menu placed in a player's
       * view. */
      MT_Vector3 position = element->NodeGetWorldPosition();
      if (const KX_GameObject *parent = tooltip->GetParent()) {
        position += parent->NodeGetWorldOrientation() *
                    (parent->NodeGetWorldScaling() * settings.tooltip_offset);
      }
      else {
        position += settings.tooltip_offset;
      }
      if ((tooltip->NodeGetWorldPosition() - position).length2() > 1.0e-12f) {
        tooltip->NodeSetWorldPosition(position);
        tooltip->NodeUpdateGS(0.0f);
      }
    }
    if (settings.tooltip_mode != LN_UITooltipMode::TextOnly &&
        (!state.visibility_known || !state.shown))
    {
      tooltip->SetVisible(true, true);
      state.shown = true;
      state.visibility_known = true;
    }
  }
  /* A tooltip no element uses any more (its elements went): hidden, or its text emptied. */
  for (auto it = m_tooltips.begin(); it != m_tooltips.end();) {
    KX_GameObject *tooltip = m_manager.ResolveObjectHandle(it->second.tooltip);
    if (tooltip != nullptr && m_tooltipFrames.contains(tooltip)) {
      ++it;
      continue;
    }
    if (tooltip != nullptr && tooltip == it->first) {
      if (it->second.mode == LN_UITooltipMode::TextOnly) {
        if (!it->second.text.empty()) {
          ClearUIText(tooltip, it->second.text_object, it->second.text);
        }
      }
      else if (it->second.shown) {
        tooltip->SetVisible(false, true);
      }
    }
    it = m_tooltips.erase(it);
  }
}

void LN_UIService::CollectUISounds()
{
  m_soundEvents.clear();
  std::vector<SoundEvent> backs = std::move(m_backSounds);
  m_backSounds.clear();
  if (m_passive) {
    return;
  }
  /* Each player's most telling sound of the frame: a click over the move it caused. */
  const auto rank = [](const UISound sound) {
    switch (sound) {
      case UISound::Back:
        return 0;
      case UISound::Denied:
        return 1;
      case UISound::Accept:
        return 2;
      default:
        return 3;
    }
  };
  const auto call_for = [&](SoundEvent sound) {
    for (SoundEvent &existing : m_soundEvents) {
      if (existing.user == sound.user) {
        if (rank(sound.sound) < rank(existing.sound)) {
          existing = std::move(sound);
        }
        return;
      }
    }
    m_soundEvents.push_back(std::move(sound));
  };
  const auto element_sound = [&](const UISound kind, const ElementEvent &event) {
    SoundEvent sound;
    sound.sound = kind;
    sound.user = event.user;
    sound.object = event.object;
    sound.action = event.action;
    sound.menu = MenuNameAt(event.menu_index);
    return sound;
  };
  for (SoundEvent &back : backs) {
    call_for(std::move(back));
  }
  bool pressed = false;
  for (const ElementEvent &event : m_elementEvents) {
    /* A click on a panel or backdrop is no control's: no sound. */
    const ElementRecord *record = event.object.IsValid() ?
                                      FindElementRecord(m_manager.ResolveObjectHandle(event.object)) :
                                      nullptr;
    if (record != nullptr && IsBackgroundElement(record->settings)) {
      continue;
    }
    switch (event.type) {
      case LN_UIElementEvent::DropReceived:
        /* Grids that handle moves put it back (no room, nothing to stack or swap). */
        call_for(element_sound(event.inventory_unchanged ? UISound::Denied : UISound::Accept,
                               event));
        break;
      case LN_UIElementEvent::Click:
      case LN_UIElementEvent::SecondaryClick:
        call_for(element_sound(UISound::Accept, event));
        break;
      case LN_UIElementEvent::Changed:
        /* A Toggle or Selector the player changed, a Text Field finished; a Slider is silent
         * (it changes all along a drag). */
        if (record == nullptr || record->settings.widget != LN_UIWidget::Slider) {
          call_for(element_sound(UISound::Accept, event));
        }
        break;
      case LN_UIElementEvent::DragCancel:
        call_for(element_sound(UISound::Denied, event));
        break;
      case LN_UIElementEvent::Press:
        pressed = true;
        break;
      default:
        break;
    }
  }
  if (m_pressedDisabledElement && !pressed) {
    call_for(m_pressedDisabled);
  }
  /* The highlight moving to another element, or to another item when navigating pages a grid;
   * not the first one a menu (or the scene) starts with. */
  for (const HighlightChange &change : m_highlightChanges) {
    const uint64_t opened = m_menuChangedFrame[size_t(change.user)];
    const bool just_opened = opened == m_frameIndex || opened + 1 == m_frameIndex;
    const auto navigation = m_frameNavigation.find(change.user);
    const bool navigated = navigation != m_frameNavigation.end() &&
                           navigation->second.has_action;
    if (change.has && !just_opened &&
        (!change.had || navigated || !SameObjectHandle(change.from.object, change.to.object)))
    {
      SoundEvent move;
      move.sound = UISound::Move;
      move.user = change.user;
      move.object = change.to.object;
      move.action = change.to.action;
      move.menu = MenuNameAt(change.to.menu_index);
      call_for(std::move(move));
    }
  }
}

const LN_UIService::SoundEvent *LN_UIService::FindSoundEvent(const int32_t user,
                                                             const size_t occurrence,
                                                             size_t *r_count) const
{
  size_t count = 0;
  const SoundEvent *found = nullptr;
  for (const SoundEvent &event : m_soundEvents) {
    if (user >= 0 && event.user != user) {
      continue;
    }
    if (count++ == occurrence) {
      found = &event;
    }
  }
  if (r_count != nullptr) {
    *r_count = count;
  }
  return m_passive ? nullptr : found;
}

void LN_UIService::AddSoundEventForTests(const SoundEvent &event)
{
  m_soundEvents.push_back(event);
}

/** \} */

size_t LN_UIService::MenuCountForTests() const
{
  return m_sessions.size();
}

LN_UIService::MenuHandle LN_UIService::AddMenuForTests(
    const int32_t user,
    const LN_UIMenuRouting routing,
    const LN_UIMenuBackBehavior back_behavior,
    const std::vector<KX_GameObject *> &objects)
{
  MenuSession session;
  session.handle = AllocateMenuHandle();
  session.user = user;
  session.routing = routing;
  session.back_behavior = back_behavior;
  for (KX_GameObject *object : objects) {
    session.objects.push_back(m_manager.MakeObjectHandle(object, object->GetName()));
  }
  const MenuHandle handle = session.handle;
  m_sessions.emplace(handle, std::move(session));
  m_menuStack.push_back(handle);
  m_menuIndexValid = false;
  return handle;
}

size_t LN_UIService::CaptureCountForTests() const
{
  size_t count = 0;
  for (const auto &phase : m_pointerStates) {
    for (const auto &pointer : phase) {
      count += pointer.second.has_capture ? 1u : 0u;
    }
  }
  return count;
}

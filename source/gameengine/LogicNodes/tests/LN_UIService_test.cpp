/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "BKE_game_input_map.hh"
#include "DNA_scene_types.h"

#include "DEV_InputDevice.h"
#include "EXP_ArrayValue.h"
#include "EXP_DictValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "KX_Camera.h"
#include "KX_GameObject.h"
#include "KX_NodeRelationships.h"
#include "KX_ObjectReferenceValue.h"
#include "LN_CommandBuffer.h"
#include "LN_InputMap.h"
#include "LN_Inventory.h"
#include "LN_Manager.h"
#include "LN_RuntimeTree.h"
#include "LN_Snapshot.h"
#include "LN_UIService.h"
#include "SG_Node.h"
#include "wm_event_types.hh"

namespace {

class TestRootParentRelation : public SG_ParentRelation {
 public:
  bool UpdateChildCoordinates(SG_Node *child,
                              const SG_Node * /*parent*/,
                              bool &parent_updated) override
  {
    if (child == nullptr) {
      return false;
    }
    child->SetWorldFromLocalTransform();
    /* Match the production root relation: updating this node invalidates the world transforms of
     * all descendants in the same scene-graph walk. */
    parent_updated = true;
    return true;
  }

  SG_ParentRelation *NewCopy() override
  {
    return new TestRootParentRelation();
  }
};

template<typename ObjectType> class SceneBoundObject {
 public:
  explicit SceneBoundObject(KX_Scene *scene) : node(&object, scene, callbacks)
  {
    node.SetParentRelation(new TestRootParentRelation());
    object.SetSGNode(&node);
  }

  ~SceneBoundObject()
  {
    object.SetSGNode(nullptr);
  }

  SG_Callbacks callbacks;
  ObjectType object;
  SG_Node node;
};

LN_UIService::Registration MakeRegistration(const LN_RuntimeTree *tree,
                                            const uint32_t query_index,
                                            KX_GameObject &object,
                                            KX_Camera &camera,
                                            const int priority,
                                            const LN_UIPropagation propagation,
                                            const int user = 0)
{
  LN_UIService::Registration registration;
  registration.listener = {tree, query_index};
  registration.object = &object;
  registration.camera = &camera;
  registration.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  registration.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  registration.priority = priority;
  registration.propagation = propagation;
  registration.user = user;
  return registration;
}

void CaptureInput(DEV_InputDevice &device,
                  LN_InputSnapshot &snapshot,
                  const int mouse_x,
                  const int mouse_y,
                  const int mouse_button_value = -1)
{
  device.ClearInputs();
  device.ConvertMoveEvent(mouse_x, mouse_y);
  if (mouse_button_value >= 0) {
    device.ConvertEvent(SCA_IInputDevice::LEFTMOUSE, mouse_button_value, 0);
  }
  snapshot.Capture(&device, nullptr);
}

void PressKey(DEV_InputDevice &device,
              LN_InputSnapshot &input,
              const SCA_IInputDevice::SCA_EnumInputs key,
              const bool pressed)
{
  device.ClearInputs();
  device.ConvertEvent(key, pressed ? 1 : 0, 0);
  input.Capture(&device, nullptr);
}

/* The UI sound of this frame (On UI Sound's, any player), None without one. */
LN_UISound FrameSound(const LN_UIService &service)
{
  const LN_UIService::SoundEvent *sound = service.FindSoundEvent(-1);
  return sound != nullptr ? sound->sound : LN_UISound::None;
}

TEST(LN_UIService, RoutesOverlappingHitsByPriorityAndPropagation)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> front(scene);
  SceneBoundObject<KX_GameObject> back(scene);

  const LN_RuntimeTree *front_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *back_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  LN_UIService::Registration front_registration = MakeRegistration(
      front_tree, 0, front.object, camera.object, 10, LN_UIPropagation::Block);
  LN_UIService::Registration back_registration = MakeRegistration(
      back_tree, 0, back.object, camera.object, 0, LN_UIPropagation::Block);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(back_registration);
  service.RegisterInteractable(front_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({front_tree, 0}).over);
  EXPECT_FALSE(service.GetInteractionState({back_tree, 0}).over);

  front_registration.propagation = LN_UIPropagation::PassThrough;
  service.BeginTick(LN_Event::OnFixedUpdate, 2, nullptr);
  service.RegisterInteractable(back_registration);
  service.RegisterInteractable(front_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({front_tree, 0}).over);
  EXPECT_TRUE(service.GetInteractionState({back_tree, 0}).over);
}

TEST(LN_UIService, ProducesStableEnterOverExitAndFocusState)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  object.object.SetName("Button");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 7, object.object, camera.object, 0, LN_UIPropagation::Block);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({tree, 7}).entered);
  EXPECT_TRUE(service.GetInteractionState({tree, 7}).over);
  EXPECT_EQ(service.SetFocus(0, &object.object, std::string()), LN_UIResult::Success);
  EXPECT_EQ(service.GetFocus(0, std::string()), &object.object);

  service.BeginTick(LN_Event::OnFixedUpdate, 2, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({tree, 7}).entered);
  EXPECT_TRUE(service.GetInteractionState({tree, 7}).over);
  EXPECT_TRUE(service.GetInteractionState({tree, 7}).focused);

  registration.ray_from = MT_Vector3(4.0f, 0.0f, -2.0f);
  registration.ray_to = MT_Vector3(4.0f, 0.0f, 2.0f);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({tree, 7}).exited);
  EXPECT_FALSE(service.GetInteractionState({tree, 7}).over);

  service.NotifyObjectRemoved(&object.object);
  EXPECT_EQ(service.GetFocus(0, std::string()), nullptr);
}

TEST(LN_UIService, DoesNotMirrorTheSystemPointerToAdditionalUsers)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 0, object.object, camera.object, 0, LN_UIPropagation::Block, 1);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({tree, 0}).over);
}

TEST(LN_UIService, CapturesDragOutsideTheHitAndReleasesOnTargetRemoval)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  object.object.SetName("Draggable");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 4, object.object, camera.object, 0, LN_UIPropagation::Block);
  registration.role = LN_UIInteractableRole::DragSource;
  registration.drag_threshold = 5.0f;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100);
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  const LN_UIService::ListenerKey listener{tree, 4};
  EXPECT_TRUE(service.GetInteractionState(listener).pressed);
  EXPECT_TRUE(service.GetInteractionState(listener).captured);
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  registration.ray_from = MT_Vector3(4.0f, 0.0f, -2.0f);
  registration.ray_to = MT_Vector3(4.0f, 0.0f, 2.0f);
  CaptureInput(device, input, 112, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState(listener).over);
  EXPECT_TRUE(service.GetInteractionState(listener).captured);
  EXPECT_TRUE(service.GetInteractionState(listener).drag_started);
  EXPECT_TRUE(service.GetInteractionState(listener).dragging);

  service.NotifyObjectRemoved(&object.object);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  EXPECT_EQ(service.GetFocus(0, std::string()), nullptr);
}

TEST(LN_UIService, CapturedDragKeepsAWorldPointAtTheOriginalPressDepth)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> source(scene);
  SceneBoundObject<KX_GameObject> blocker(scene);
  source.object.SetName("DragSource");
  blocker.object.SetName("Blocker");
  const LN_RuntimeTree *source_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *blocker_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  LN_UIService::Registration source_registration = MakeRegistration(
      source_tree, 4, source.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration blocker_registration = MakeRegistration(
      blocker_tree, 5, blocker.object, camera.object, 10, LN_UIPropagation::Block);
  source_registration.role = LN_UIInteractableRole::DragSource;
  source_registration.drag_threshold = 1.0f;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(source_registration);
  service.ResolveInteractions();
  ASSERT_TRUE(service.GetInteractionState({source_tree, 4}).captured);

  /* The pointer leaves the source and enters a higher-priority blocking object. Capture must keep
   * producing the pointer ray projected at the depth where the source was pressed, rather than
   * falling back to a zero vector. */
  source_registration.ray_from = MT_Vector3(1.0f, 0.0f, -2.0f);
  source_registration.ray_to = MT_Vector3(1.0f, 0.0f, 2.0f);
  blocker_registration.ray_from = source_registration.ray_from;
  blocker_registration.ray_to = source_registration.ray_to;
  blocker_registration.world_position = MT_Vector3(1.0f, 0.0f, 0.0f);
  CaptureInput(device, input, 112, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(blocker_registration);
  service.ResolveInteractions();

  const LN_UIService::InteractionState &drag = service.GetInteractionState({source_tree, 4});
  EXPECT_FALSE(drag.over);
  EXPECT_TRUE(drag.captured);
  EXPECT_TRUE(drag.dragging);
  EXPECT_NEAR(drag.point.x(), 1.0f, 1.0e-6f);
  EXPECT_NEAR(drag.point.y(), 0.0f, 1.0e-6f);
  EXPECT_NEAR(drag.point.z(), -0.5f, 1.0e-6f);
  EXPECT_NEAR(drag.normal.z(), -1.0f, 1.0e-6f);
}

TEST(LN_UIService, CapturedDragUsesACameraFacingPlaneInPerspective)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> source(scene);
  source.object.SetName("PerspectiveDragSource");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 4, source.object, camera.object, 0, LN_UIPropagation::Block);
  registration.ray_from = MT_Vector3(0.0f, 0.0f, 0.0f);
  registration.ray_to = MT_Vector3(0.0f, 0.0f, -4.0f);
  registration.world_position = MT_Vector3(0.0f, 0.0f, -2.0f);
  registration.role = LN_UIInteractableRole::DragSource;
  registration.drag_threshold = 0.0f;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  ASSERT_TRUE(service.GetInteractionState({tree, 4}).captured);

  /* Rotate the normalized pointer ray 45 degrees. A fraction along the ray would put the point
   * on a sphere and change Z; the correct drag plane retains camera-space depth Z = -1.5. */
  const float diagonal = std::sqrt(8.0f);
  registration.ray_to = MT_Vector3(diagonal, 0.0f, -diagonal);
  CaptureInput(device, input, 112, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();

  const LN_UIService::InteractionState &drag = service.GetInteractionState({tree, 4});
  EXPECT_TRUE(drag.captured);
  EXPECT_TRUE(drag.dragging);
  EXPECT_NEAR(drag.point.x(), 1.5f, 1.0e-5f);
  EXPECT_NEAR(drag.point.y(), 0.0f, 1.0e-5f);
  EXPECT_NEAR(drag.point.z(), -1.5f, 1.0e-5f);
}

TEST(LN_UIService, CapturedDragSourceDoesNotOccludeDropTargetHover)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> source(scene);
  SceneBoundObject<KX_GameObject> target(scene);
  const LN_RuntimeTree *source_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *target_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  LN_UIService::Registration source_registration = MakeRegistration(
      source_tree, 4, source.object, camera.object, 10, LN_UIPropagation::Block);
  LN_UIService::Registration target_registration = MakeRegistration(
      target_tree, 5, target.object, camera.object, 0, LN_UIPropagation::Block);
  source_registration.role = LN_UIInteractableRole::DragSource;
  source_registration.drag_threshold = 0.0f;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(source_registration);
  service.ResolveInteractions();
  ASSERT_TRUE(service.GetInteractionState({source_tree, 4}).dragging);

  /* The captured drag source remains a Block control, but it must not hide an underlying drop
   * target while dragging. The source still receives capture independently. */
  CaptureInput(device, input, 101, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(target_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({source_tree, 4}).captured);
  EXPECT_TRUE(service.GetInteractionState({source_tree, 4}).dragging);
  EXPECT_TRUE(service.GetInteractionState({target_tree, 5}).over);
}

TEST(LN_UIService, BlockingDragSourceDoesNotClickAControlBehindItBeforeDragging)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> source(scene);
  SceneBoundObject<KX_GameObject> behind(scene);
  const LN_RuntimeTree *source_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *behind_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  LN_UIService::Registration source_registration = MakeRegistration(
      source_tree, 4, source.object, camera.object, 10, LN_UIPropagation::Block);
  LN_UIService::Registration behind_registration = MakeRegistration(
      behind_tree, 5, behind.object, camera.object, 0, LN_UIPropagation::Block);
  source_registration.role = LN_UIInteractableRole::DragSource;
  source_registration.drag_threshold = 6.0f;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(behind_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({source_tree, 4}).pressed);
  EXPECT_FALSE(service.GetInteractionState({behind_tree, 5}).pressed);

  CaptureInput(device, input, 100, 100, 0);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(behind_registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({behind_tree, 5}).released);
  EXPECT_FALSE(service.GetInteractionState({behind_tree, 5}).clicked);
}

TEST(LN_UIService, RetargetingACapturedListenerCancelsThePressSequence)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> original(scene);
  SceneBoundObject<KX_GameObject> replacement(scene);
  original.object.SetName("Original");
  replacement.object.SetName("Replacement");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 4, original.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  ASSERT_EQ(service.CaptureCountForTests(), 1u);

  registration.object = &replacement.object;
  CaptureInput(device, input, 101, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  EXPECT_FALSE(service.GetInteractionState({tree, 4}).captured);
  EXPECT_FALSE(service.GetInteractionState({tree, 4}).dragging);
}

TEST(LN_UIService, ChangingTheInteractionCameraCancelsCapture)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> first_camera(scene);
  SceneBoundObject<KX_Camera> second_camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  object.object.SetName("Draggable");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 4, object.object, first_camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  ASSERT_EQ(service.CaptureCountForTests(), 1u);

  registration.camera = &second_camera.object;
  CaptureInput(device, input, 101, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  EXPECT_FALSE(service.GetInteractionState({tree, 4}).captured);
}

TEST(LN_UIService, RemovingTheInteractionCameraCancelsCaptureImmediately)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  object.object.SetName("CameraBoundControl");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::ListenerKey listener{tree, 42};
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 42, object.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  ASSERT_TRUE(service.GetInteractionState(listener).captured);
  ASSERT_EQ(service.CaptureCountForTests(), 1u);

  service.NotifyObjectRemoved(&camera.object);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  EXPECT_FALSE(service.GetInteractionState(listener).captured);
}

TEST(LN_UIService, QueuedRemovalObjectsCannotBeRegisteredAgain)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  object.object.SetName("PendingRemoval");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::ListenerKey listener{tree, 44};
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 44, object.object, camera.object, 0, LN_UIPropagation::Block);

  manager.NotifyGameObjectQueuedForRemoval(&object.object, KX_ObjectRemovalReason::Gameplay);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.DeclareInteractable(listener, 0, true);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState(listener).valid);
  EXPECT_FALSE(service.GetInteractionState(listener).over);
}

TEST(LN_UIService, DragReleaseOutsideEndsCaptureWithoutClicking)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 5, object.object, camera.object, 0, LN_UIPropagation::Block);
  registration.role = LN_UIInteractableRole::DragSource;
  registration.drag_threshold = 3.0f;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 40, 40);
  CaptureInput(device, input, 40, 40, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();

  registration.ray_from = MT_Vector3(4.0f, 0.0f, -2.0f);
  registration.ray_to = MT_Vector3(4.0f, 0.0f, 2.0f);
  CaptureInput(device, input, 50, 40);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({tree, 5}).dragging);

  CaptureInput(device, input, 50, 40, 0);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  const LN_UIService::InteractionState &released = service.GetInteractionState({tree, 5});
  EXPECT_TRUE(released.released);
  EXPECT_TRUE(released.drag_ended);
  EXPECT_FALSE(released.clicked);
  EXPECT_FALSE(released.captured);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

TEST(LN_UIService, PressAndReleaseInsideProducesOneClick)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 6, object.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 25, 25);
  CaptureInput(device, input, 25, 25, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();

  CaptureInput(device, input, 25, 25, 0);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  const LN_UIService::InteractionState &released = service.GetInteractionState({tree, 6});
  EXPECT_TRUE(released.released);
  EXPECT_TRUE(released.clicked);
  EXPECT_FALSE(released.drag_ended);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  CaptureInput(device, input, 25, 25);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({tree, 6}).clicked);
}

TEST(LN_UIService, AButtonStillClicksWhenThePointerMovesDuringThePress)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 6, object.object, camera.object, 0, LN_UIPropagation::Block);
  registration.drag_threshold = 6.0f;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterInteractable(registration);
    service.ResolveInteractions();
  };
  CaptureInput(device, input, 25, 25, 1);
  tick(1);
  /* Far past the drag threshold, still over the button. */
  CaptureInput(device, input, 60, 25);
  tick(2);
  EXPECT_TRUE(service.GetInteractionState({tree, 6}).captured);
  EXPECT_FALSE(service.GetInteractionState({tree, 6}).drag_started);
  EXPECT_FALSE(service.GetInteractionState({tree, 6}).dragging);
  CaptureInput(device, input, 60, 25, 0);
  tick(3);
  EXPECT_TRUE(service.GetInteractionState({tree, 6}).clicked);
  EXPECT_FALSE(service.GetInteractionState({tree, 6}).drag_ended);
}

TEST(LN_UIService, CapturedPassThroughPressReleasesEveryRecipient)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> front(scene);
  SceneBoundObject<KX_GameObject> back(scene);
  const LN_RuntimeTree *front_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *back_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  const LN_UIService::Registration front_registration = MakeRegistration(
      front_tree, 1, front.object, camera.object, 10, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration back_registration = MakeRegistration(
      back_tree, 2, back.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 70, 70);
  CaptureInput(device, input, 70, 70, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(front_registration);
  service.RegisterInteractable(back_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({front_tree, 1}).pressed);
  EXPECT_TRUE(service.GetInteractionState({front_tree, 1}).captured);
  EXPECT_TRUE(service.GetInteractionState({back_tree, 2}).pressed);

  CaptureInput(device, input, 70, 70, 0);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(front_registration);
  service.RegisterInteractable(back_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({front_tree, 1}).released);
  EXPECT_TRUE(service.GetInteractionState({front_tree, 1}).clicked);
  EXPECT_TRUE(service.GetInteractionState({back_tree, 2}).released);
  EXPECT_TRUE(service.GetInteractionState({back_tree, 2}).clicked);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  CaptureInput(device, input, 70, 70);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(front_registration);
  service.RegisterInteractable(back_registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({front_tree, 1}).released);
  EXPECT_FALSE(service.GetInteractionState({back_tree, 2}).released);
}

TEST(LN_UIService, ReconcilesCaptureWhenAReleaseWasDiscardedDuringPause)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 8, object.object, camera.object, 0, LN_UIPropagation::Block);
  const LN_UIService::ListenerKey listener{tree, 8};

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 90, 90);
  CaptureInput(device, input, 90, 90, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState(listener).pressed);
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  /* Fixed gameplay did not run while paused, so its release edge must not become a delayed click
   * after resuming. The newest steady device state still records that the button is now up. */
  CaptureInput(device, input, 90, 90, 0);
  input.ClearTransientState();
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState(listener).released);
  EXPECT_FALSE(service.GetInteractionState(listener).clicked);
  EXPECT_FALSE(service.GetInteractionState(listener).captured);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  CaptureInput(device, input, 90, 90, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState(listener).pressed);
  EXPECT_TRUE(service.GetInteractionState(listener).captured);
}

TEST(LN_UIService, CancelsCaptureWhenTheInteractableDeclarationDisappears)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 10, object.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 20, 20, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  device.ClearInputs();
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.ResolveInteractions();
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

TEST(LN_UIService, DisablingACapturedInteractableCannotProduceALaterClick)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 11, object.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 20, 20, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  registration.enabled = false;
  device.ClearInputs();
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  registration.enabled = true;
  CaptureInput(device, input, 20, 20, 0);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({tree, 11}).clicked);
}

TEST(LN_UIService, FocusedAcceptShowsAHiddenFocusBeforeItClicks)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  object.object.SetName("FocusedButton");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 12, object.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterInteractable(registration);
    service.ResolveInteractions();
  };
  tick(1);
  ASSERT_EQ(service.SetFocus(0, &object.object, std::string()), LN_UIResult::Success);
  tick(2);
  /* Set UI Focus selects it, but a mouse player does not see that yet. */
  EXPECT_TRUE(service.GetInteractionState({tree, 12}).focused);
  EXPECT_FALSE(service.GetInteractionState({tree, 12}).focus_visible);

  /* The first Accept only shows the selection: nothing invisible is ever pressed. */
  PressKey(device, input, SCA_IInputDevice::RETKEY, true);
  tick(3);
  EXPECT_FALSE(service.GetInteractionState({tree, 12}).clicked);
  EXPECT_TRUE(service.GetInteractionState({tree, 12}).focus_visible);

  /* Now it is shown, Accept is its ordinary click, once per press. */
  PressKey(device, input, SCA_IInputDevice::RETKEY, false);
  tick(4);
  PressKey(device, input, SCA_IInputDevice::RETKEY, true);
  tick(5);
  EXPECT_TRUE(service.GetInteractionState({tree, 12}).clicked);
  device.ClearInputs();
  input.Capture(&device, nullptr);
  tick(6);
  EXPECT_FALSE(service.GetInteractionState({tree, 12}).clicked);
}

TEST(LN_UIService, GameplayKeysDoNotPressAClickedButtonOutsideMenus)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration first_registration = MakeRegistration(
      tree, 1, first.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration second_registration = MakeRegistration(
      tree, 2, second.object, camera.object, 0, LN_UIPropagation::Block);
  second_registration.ray_from = second_registration.ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterInteractable(first_registration);
    service.RegisterInteractable(second_registration);
    service.ResolveInteractions();
  };
  /* Click a HUD button: it gets focus. */
  CaptureInput(device, input, 10, 10, 1);
  tick(1);
  CaptureInput(device, input, 10, 10, 0);
  tick(2);
  ASSERT_TRUE(service.GetInteractionState({tree, 1}).clicked);
  ASSERT_EQ(service.GetFocus(0, std::string()), &first.object);

  /* Jumping with Space, walking with the arrows or pressing Tab neither presses it again nor moves
   * focus: no menu is open and Set UI Focus never asked for keyboard control. */
  for (const SCA_IInputDevice::SCA_EnumInputs key :
       {SCA_IInputDevice::SPACEKEY, SCA_IInputDevice::DOWNARROWKEY, SCA_IInputDevice::TABKEY})
  {
    PressKey(device, input, key, true);
    tick(3);
    EXPECT_FALSE(service.GetInteractionState({tree, 1}).clicked);
    EXPECT_FALSE(service.GetInteractionState({tree, 1}).focus_visible);
    EXPECT_EQ(service.GetFocus(0, std::string()), &first.object);
    PressKey(device, input, key, false);
    tick(4);
  }
}

TEST(LN_UIService, PointerIgnoredControlsRemainKeyboardAndGamepadFocusable)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  object.object.SetName("KeyboardOnlyButton");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 16, object.object, camera.object, 0, LN_UIPropagation::Ignore);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  ASSERT_EQ(service.SetFocus(0, &object.object, std::string()), LN_UIResult::Success);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const bool enter_down) {
    PressKey(device, input, SCA_IInputDevice::RETKEY, enter_down);
    service.BeginTick(LN_Event::OnFixedUpdate, index, &input);
    service.RegisterInteractable(registration);
    service.ResolveInteractions();
  };
  /* The first press shows the selection, the second one presses it. */
  tick(2, true);
  tick(3, false);
  tick(4, true);
  EXPECT_FALSE(service.GetInteractionState({tree, 16}).over);
  EXPECT_TRUE(service.GetInteractionState({tree, 16}).clicked);
}

TEST(LN_UIService, DisabledInteractablesCannotReceiveExplicitFocus)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 13, object.object, camera.object, 0, LN_UIPropagation::Block);
  registration.enabled = false;

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_EQ(service.SetFocus(0, &object.object, std::string()), LN_UIResult::InvalidTarget);
  EXPECT_EQ(service.GetFocus(0, std::string()), nullptr);
}

TEST(LN_UIService, DisablingAFocusedInteractableImmediatelyClearsItsFocusedOutput)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 14, object.object, camera.object, 0, LN_UIPropagation::Block);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  ASSERT_EQ(service.SetFocus(0, &object.object, std::string()), LN_UIResult::Success);

  service.BeginTick(LN_Event::OnFixedUpdate, 2, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({tree, 14}).focused);

  registration.enabled = false;
  service.BeginTick(LN_Event::OnFixedUpdate, 3, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({tree, 14}).focused);
  EXPECT_EQ(service.GetFocus(0, std::string()), nullptr);
}

TEST(LN_UIService, RejectsMalformedHitTestData)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 9, object.object, camera.object, 0, LN_UIPropagation::Block);

  registration.world_orientation[0][0] = std::numeric_limits<float>::quiet_NaN();
  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({tree, 9}).valid);

  registration.world_orientation = MT_Matrix3x3::Identity();
  registration.bounds_min = MT_Vector3(1.0f, 1.0f, 1.0f);
  registration.bounds_max = MT_Vector3(-1.0f, -1.0f, -1.0f);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({tree, 9}).valid);
}

TEST(LN_UIService, RejectsFiniteInputsWhoseHitMathOverflows)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 23, object.object, camera.object, 0, LN_UIPropagation::Block);
  registration.ray_from = MT_Vector3(std::numeric_limits<float>::max(), 0.0f, 0.0f);
  registration.ray_to = MT_Vector3(-std::numeric_limits<float>::max(), 0.0f, 0.0f);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_FALSE(service.GetInteractionState({tree, 23}).valid);
  EXPECT_FALSE(service.GetInteractionState({tree, 23}).over);
}

TEST(LN_UIService, TabNavigationUsesDeterministicTabOrder)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  first.object.SetName("First");
  second.object.SetName("Second");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration first_registration = MakeRegistration(
      tree, 1, first.object, camera.object, 0, LN_UIPropagation::PassThrough);
  LN_UIService::Registration second_registration = MakeRegistration(
      tree, 2, second.object, camera.object, 0, LN_UIPropagation::PassThrough);
  first_registration.tab_order = 10;
  second_registration.tab_order = 20;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnFixedUpdate, index, &input);
    service.RegisterInteractable(second_registration);
    service.RegisterInteractable(first_registration);
    service.ResolveInteractions();
  };
  /* Outside menus, Set UI Focus is what lets the keyboard control this UI. */
  tick(1);
  ASSERT_EQ(service.SetFocus(0, &first.object, std::string()), LN_UIResult::Success);
  /* The first Tab shows the hidden selection where it is. */
  PressKey(device, input, SCA_IInputDevice::TABKEY, true);
  tick(2);
  EXPECT_EQ(service.GetFocus(0, std::string()), &first.object);

  PressKey(device, input, SCA_IInputDevice::TABKEY, false);
  tick(3);
  PressKey(device, input, SCA_IInputDevice::TABKEY, true);
  tick(4);
  EXPECT_EQ(service.GetFocus(0, std::string()), &second.object);
}

TEST(LN_UIService, RemovingTheFocusedObjectEndsKeyboardControlOutsideMenus)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> last(scene);
  first.object.SetName("First");
  last.object.SetName("Last");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration first_registration = MakeRegistration(
      tree, 14, first.object, camera.object, 0, LN_UIPropagation::PassThrough);
  LN_UIService::Registration last_registration = MakeRegistration(
      tree, 15, last.object, camera.object, 0, LN_UIPropagation::PassThrough);
  first_registration.tab_order = 10;
  last_registration.tab_order = 20;
  SceneBoundObject<KX_GameObject> removed(scene);
  const LN_UIService::Registration removed_registration = MakeRegistration(
      tree, 16, removed.object, camera.object, 0, LN_UIPropagation::PassThrough);

  /* Keyboard control was granted by Set UI Focus (a title screen), then the focused object went
   * away with that UI. Other UI (a HUD) remains. */
  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(removed_registration);
  service.ResolveInteractions();
  ASSERT_EQ(service.SetFocus(0, &removed.object, std::string()), LN_UIResult::Success);
  service.NotifyObjectRemoved(&removed.object);
  ASSERT_EQ(service.GetFocus(0, std::string()), nullptr);

  /* Shift+Tab is game input again: the HUD does not take focus or the key. */
  DEV_InputDevice device;
  LN_InputSnapshot input;
  device.ConvertEvent(SCA_IInputDevice::LEFTSHIFTKEY, 1, 0);
  device.ConvertEvent(SCA_IInputDevice::TABKEY, 1, 0);
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(first_registration);
  service.RegisterInteractable(last_registration);
  service.ResolveInteractions();
  EXPECT_EQ(service.GetFocus(0, std::string()), nullptr);
  EXPECT_FALSE(manager.GetInputConsumption().IsPressConsumed(SCA_IInputDevice::TABKEY));
}

TEST(LN_UIService, ReverseTabFromAFocusOutsideTheNavigationStartsAtTheLastInteractable)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> last(scene);
  SceneBoundObject<KX_GameObject> hidden(scene);
  first.object.SetName("First");
  last.object.SetName("Last");
  hidden.object.SetName("Hidden");
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration first_registration = MakeRegistration(
      tree, 14, first.object, camera.object, 0, LN_UIPropagation::PassThrough);
  LN_UIService::Registration last_registration = MakeRegistration(
      tree, 15, last.object, camera.object, 0, LN_UIPropagation::PassThrough);
  first_registration.tab_order = 10;
  last_registration.tab_order = 20;
  const LN_UIService::Registration hidden_registration = MakeRegistration(
      tree, 16, hidden.object, camera.object, 0, LN_UIPropagation::PassThrough);

  /* Set UI Focus gave the keyboard to a button that game logic then stopped registering. */
  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(hidden_registration);
  service.ResolveInteractions();
  ASSERT_EQ(service.SetFocus(0, &hidden.object, std::string()), LN_UIResult::Success);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  device.ConvertEvent(SCA_IInputDevice::LEFTSHIFTKEY, 1, 0);
  device.ConvertEvent(SCA_IInputDevice::TABKEY, 1, 0);
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(first_registration);
  service.RegisterInteractable(last_registration);
  service.ResolveInteractions();
  EXPECT_EQ(service.GetFocus(0, std::string()), &last.object);
}

TEST(LN_UIService, DragSourceDropsOnACompatibleDropTarget)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> source(scene);
  SceneBoundObject<KX_GameObject> target(scene);
  const LN_RuntimeTree *source_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *target_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  LN_UIService::Registration source_registration = MakeRegistration(
      source_tree, 1, source.object, camera.object, 10, LN_UIPropagation::Block);
  LN_UIService::Registration target_registration = MakeRegistration(
      target_tree, 2, target.object, camera.object, 0, LN_UIPropagation::Block);
  source_registration.role = LN_UIInteractableRole::DragSource;
  source_registration.drag_threshold = 1.0f;
  source_registration.drop_group = "Inventory";
  target_registration.drop_target = true;
  target_registration.drop_group = "Inventory";
  target_registration.capture_on_press = false;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(source_registration);
  service.ResolveInteractions();

  /* On the first drag tick, the captured source no longer hides the target behind it. */
  CaptureInput(device, input, 101, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(target_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({source_tree, 1}).drag_started);
  EXPECT_TRUE(service.GetInteractionState({target_tree, 2}).drop_entered);
  EXPECT_TRUE(service.GetInteractionState({target_tree, 2}).drop_over);

  CaptureInput(device, input, 101, 100, 0);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(target_registration);
  service.ResolveInteractions();
  const LN_UIService::InteractionState &source_result = service.GetInteractionState(
      {source_tree, 1});
  const LN_UIService::InteractionState &target_result = service.GetInteractionState(
      {target_tree, 2});
  EXPECT_TRUE(source_result.drop_accepted);
  EXPECT_TRUE(target_result.dropped);
  EXPECT_EQ(manager.ResolveObjectHandle(target_result.source_object), &source.object);
  EXPECT_EQ(manager.ResolveObjectHandle(source_result.target_object), &target.object);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

TEST(LN_UIService, CancellingDragClearsCaptureAndDropOverInTheCancellationTick)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> source(scene);
  SceneBoundObject<KX_GameObject> target(scene);
  const LN_RuntimeTree *source_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *target_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  LN_UIService::Registration source_registration = MakeRegistration(
      source_tree, 1, source.object, camera.object, 10, LN_UIPropagation::Block);
  LN_UIService::Registration target_registration = MakeRegistration(
      target_tree, 2, target.object, camera.object, 0, LN_UIPropagation::Block);
  source_registration.role = LN_UIInteractableRole::DragSource;
  source_registration.drag_threshold = 1.0f;
  target_registration.drop_target = true;
  target_registration.capture_on_press = false;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(source_registration);
  service.ResolveInteractions();

  CaptureInput(device, input, 102, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(target_registration);
  service.ResolveInteractions();
  ASSERT_TRUE(service.GetInteractionState({source_tree, 1}).dragging);
  ASSERT_TRUE(service.GetInteractionState({target_tree, 2}).drop_over);

  device.ClearInputs();
  device.ConvertMoveEvent(103, 100);
  device.ConvertEvent(SCA_IInputDevice::ESCKEY, 1, 0);
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(target_registration);
  service.ResolveInteractions();
  const LN_UIService::InteractionState &source_state = service.GetInteractionState(
      {source_tree, 1});
  const LN_UIService::InteractionState &target_state = service.GetInteractionState(
      {target_tree, 2});
  EXPECT_TRUE(source_state.drag_cancelled);
  EXPECT_TRUE(source_state.drag_ended);
  EXPECT_FALSE(source_state.dragging);
  EXPECT_FALSE(source_state.captured);
  EXPECT_TRUE(target_state.drop_exited);
  EXPECT_FALSE(target_state.drop_over);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

TEST(LN_UIService, ControlKeepsItsTargetWithoutCapture)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> control(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration registration = MakeRegistration(
      tree, 1, control.object, camera.object, 0, LN_UIPropagation::Block);
  registration.capture_on_press = false;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();

  CaptureInput(device, input, 107, 96);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  const LN_UIService::InteractionState &over = service.GetInteractionState({tree, 1});
  EXPECT_TRUE(over.over);
  EXPECT_EQ(manager.ResolveObjectHandle(over.target_object), &control.object);

  registration.has_precomputed_hit = true;
  registration.precomputed_hit = false;
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  const LN_UIService::InteractionState &exited = service.GetInteractionState({tree, 1});
  EXPECT_TRUE(exited.exited);
  EXPECT_EQ(manager.ResolveObjectHandle(exited.target_object), &control.object);
}

TEST(LN_UIService, DropGroupMismatchRejectsWithoutEndingDragEarly)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> source(scene);
  SceneBoundObject<KX_GameObject> target(scene);
  const LN_RuntimeTree *source_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *target_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));
  LN_UIService::Registration source_registration = MakeRegistration(
      source_tree, 1, source.object, camera.object, 10, LN_UIPropagation::Block);
  LN_UIService::Registration target_registration = MakeRegistration(
      target_tree, 2, target.object, camera.object, 0, LN_UIPropagation::Block);
  source_registration.role = LN_UIInteractableRole::DragSource;
  source_registration.drag_threshold = 0.0f;
  source_registration.drop_group = "Inventory";
  target_registration.drop_target = true;
  target_registration.drop_group = "Equipment";

  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100, 1);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterInteractable(source_registration);
  service.ResolveInteractions();
  CaptureInput(device, input, 101, 100);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(target_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({source_tree, 1}).dragging);
  EXPECT_FALSE(service.GetInteractionState({target_tree, 2}).drop_over);
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  CaptureInput(device, input, 101, 100, 0);
  service.BeginTick(LN_Event::OnFixedUpdate, 3, &input);
  service.RegisterInteractable(source_registration);
  service.RegisterInteractable(target_registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({source_tree, 1}).drop_rejected);
  EXPECT_FALSE(service.GetInteractionState({target_tree, 2}).dropped);
}

TEST(LN_UIService, GridViewBindsOnlyItsVisibleSlotPoolAndClipsOutsideSlots)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> inside(scene);
  SceneBoundObject<KX_GameObject> outside(scene);
  inside.object.NodeSetWorldPosition(MT_Vector3(0.0f, 0.0f, 0.0f));
  outside.object.NodeSetWorldPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  inside.object.NodeUpdateGS(0.0);
  outside.object.NodeUpdateGS(0.0);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  LN_UIService::ViewRegistration view;
  view.listener = {tree, 7};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.slots = {&inside.object, &outside.object};
  for (const char *name : {"Sword", "Shield", "Potion"}) {
    LN_Value item;
    item.type = LN_ValueType::String;
    item.exists = true;
    item.string_value = name;
    view.items.push_back(item);
  }

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  LN_Value item;
  int32_t index = -1;
  EXPECT_TRUE(service.GetBoundItem(&inside.object, item, index));
  EXPECT_EQ(index, 0);
  EXPECT_EQ(item.string_value, "Sword");
  EXPECT_TRUE(service.GetBoundItem(&outside.object, item, index));
  EXPECT_EQ(index, 1);
  EXPECT_TRUE(service.ObjectPassesViewClipping(&inside.object));
  EXPECT_FALSE(service.ObjectPassesViewClipping(&outside.object));
  EXPECT_TRUE(inside.object.GetVisible());
  EXPECT_FALSE(outside.object.GetVisible());

  const LN_UIService::ViewState &state = service.GetViewState({tree, 7});
  EXPECT_TRUE(state.valid);
  EXPECT_TRUE(state.items_rebound);
  EXPECT_EQ(state.first_visible_index, 0);
  EXPECT_EQ(state.last_visible_index, 1);

  /* Stable declarations do not pulse rebind, while item data changes do. Hidden slots remain
   * resolvable and reusable instead of disappearing from the next registration pass. */
  service.BeginTick(LN_Event::OnFixedUpdate, 2, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  EXPECT_FALSE(service.GetViewState({tree, 7}).items_rebound);
  EXPECT_TRUE(service.GetBoundItem(&outside.object, item, index));
  view.items[1].string_value = "Tower Shield";
  service.BeginTick(LN_Event::OnFixedUpdate, 3, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  EXPECT_TRUE(service.GetViewState({tree, 7}).items_rebound);
  EXPECT_TRUE(service.GetBoundItem(&outside.object, item, index));
  EXPECT_EQ(item.string_value, "Tower Shield");

  /* Removing the declaration restores visibility owned by the view and clears stale bindings. */
  service.BeginTick(LN_Event::OnFixedUpdate, 4, nullptr);
  service.ResolveViews();
  EXPECT_TRUE(outside.object.GetVisible());
  EXPECT_FALSE(service.GetBoundItem(&inside.object, item, index));
}

TEST(LN_UIService, ShowEmptyKeepsTheCellsPastTheLastItemAsEmptySlots)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 8};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.cull_outside = false;
  view.slots = {&first.object, &second.object};
  LN_Value sword;
  sword.type = LN_ValueType::String;
  sword.exists = true;
  sword.string_value = "Sword";
  view.items = {sword};
  uint64_t tick = 0;
  auto resolve = [&]() {
    service.BeginTick(LN_Event::OnFixedUpdate, ++tick, nullptr);
    service.RegisterView(view);
    service.ResolveViews();
  };
  LN_Value item;
  int32_t index = -1;
  /* Without it, a cell with no item is hidden and bound to nothing. */
  resolve();
  EXPECT_TRUE(first.object.GetVisible());
  EXPECT_FALSE(second.object.GetVisible());
  EXPECT_FALSE(service.GetBoundItem(&second.object, item, index));
  /* With it, the cell stays as an empty slot at its index: a drop there names the place. */
  view.show_empty = true;
  resolve();
  EXPECT_TRUE(second.object.GetVisible());
  ASSERT_TRUE(service.GetBoundItem(&second.object, item, index));
  EXPECT_EQ(index, 1);
  EXPECT_FALSE(item.exists);
  ASSERT_TRUE(service.GetBoundItem(&first.object, item, index));
  EXPECT_EQ(item.string_value, "Sword");
}

TEST(LN_UIService, RemovingAViewCameraImmediatelyReleasesBindingsAndVisibility)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  slot.object.NodeSetWorldPosition(MT_Vector3(2.0f, 0.0f, 0.0f));
  slot.object.NodeUpdateGS(0.0);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::ListenerKey listener{tree, 43};

  LN_UIService::ViewRegistration view;
  view.listener = listener;
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.slots = {&slot.object};
  LN_Value item;
  item.type = LN_ValueType::String;
  item.exists = true;
  item.string_value = "Item";
  view.items.push_back(item);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  int32_t index = -1;
  ASSERT_TRUE(service.GetViewState(listener).valid);
  ASSERT_TRUE(service.GetBoundItem(&slot.object, item, index));
  ASSERT_FALSE(slot.object.GetVisible());

  service.NotifyObjectRemoved(&camera.object);
  EXPECT_FALSE(service.GetViewState(listener).valid);
  EXPECT_FALSE(service.GetBoundItem(&slot.object, item, index));
  EXPECT_TRUE(slot.object.GetVisible());
}

TEST(LN_UIService, EmptyGridHasNoVisibleRangeAndIsBothAtStartAndEnd)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  LN_UIService::ViewRegistration view;
  view.listener = {tree, 20};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  LN_Value item;
  item.type = LN_ValueType::String;
  item.exists = true;
  item.string_value = "No Slot";
  view.items.push_back(item);

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  const LN_UIService::ViewState &state = service.GetViewState({tree, 20});
  EXPECT_TRUE(state.valid);
  EXPECT_EQ(state.first_visible_index, -1);
  EXPECT_EQ(state.last_visible_index, -1);
  EXPECT_TRUE(state.at_start);
  EXPECT_TRUE(state.at_end);
  EXPECT_NEAR(state.normalized.x(), 0.0f, 0.0001f);
}

TEST(LN_UIService, SharedGridSlotUsesOnlyTheWinningViewsBindingAndClipping)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> low_viewport(scene);
  SceneBoundObject<KX_GameObject> high_viewport(scene);
  SceneBoundObject<KX_GameObject> shared_slot(scene);
  const LN_RuntimeTree *low_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *high_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));

  auto make_view = [&](const LN_RuntimeTree *tree,
                       const uint32_t query,
                       KX_GameObject &viewport,
                       const int priority,
                       const char *item_name) {
    LN_UIService::ViewRegistration view;
    view.listener = {tree, query};
    view.kind = LN_UIService::ViewKind::Grid;
    view.viewport = &viewport;
    view.camera = &camera.object;
    view.priority = priority;
    view.slots = {&shared_slot.object};
    LN_Value item;
    item.type = LN_ValueType::String;
    item.exists = true;
    item.string_value = item_name;
    view.items.push_back(item);
    return view;
  };
  LN_UIService::ViewRegistration low = make_view(low_tree, 1, low_viewport.object, 0, "Low");
  low.world_position = MT_Vector3(10.0f, 0.0f, 0.0f);
  LN_UIService::ViewRegistration high = make_view(high_tree, 2, high_viewport.object, 10, "High");

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterView(low);
  service.RegisterView(high);
  service.ResolveViews();
  LN_Value item;
  int32_t index = -1;
  ASSERT_TRUE(service.GetBoundItem(&shared_slot.object, item, index));
  EXPECT_EQ(item.string_value, "High");
  EXPECT_EQ(index, 0);
  EXPECT_TRUE(service.ObjectPassesViewClipping(&shared_slot.object));
  EXPECT_TRUE(shared_slot.object.GetVisible());
}

TEST(LN_UIService, FocusedGridSlotDoesNotReduceTheRequestedWheelPage)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> first_slot(scene);
  SceneBoundObject<KX_GameObject> second_slot(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  LN_UIService::ViewRegistration view;
  view.listener = {tree, 30};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.slots = {&first_slot.object, &second_slot.object};
  view.columns = 2;
  for (int index = 0; index < 6; ++index) {
    LN_Value item;
    item.type = LN_ValueType::Int;
    item.exists = true;
    item.int_value = index;
    view.items.push_back(item);
  }

  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  LN_UIService::Registration focusable = MakeRegistration(
      tree, 31, second_slot.object, camera.object, 0, LN_UIPropagation::Block);
  service.RegisterInteractable(focusable);
  service.ResolveInteractions();
  ASSERT_EQ(service.SetFocus(0, &second_slot.object, std::string()), LN_UIResult::Success);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  device.ConvertMoveEvent(100, 100);
  device.ConvertWheelEvent(-1);
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterView(view);
  service.ResolveViews();
  const LN_UIService::ViewState &state = service.GetViewState({tree, 30});
  EXPECT_TRUE(state.scrolled);
  EXPECT_EQ(state.first_visible_index, 2);
  EXPECT_EQ(state.last_visible_index, 3);
  service.RegisterInteractable(focusable);
  service.ResolveInteractions();
  EXPECT_EQ(service.GetFocus(0, std::string()), &second_slot.object);
}

TEST(LN_UIService, GridNavigationRecyclesSlotsToReachOffscreenItems)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> first_slot(scene);
  SceneBoundObject<KX_GameObject> second_slot(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  LN_UIService::ViewRegistration view;
  view.listener = {tree, 40};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.slots = {&first_slot.object, &second_slot.object};
  view.columns = 1;
  for (int index = 0; index < 4; ++index) {
    LN_Value item;
    item.type = LN_ValueType::Int;
    item.exists = true;
    item.int_value = index;
    view.items.push_back(item);
  }

  LN_UIService::Registration first = MakeRegistration(
      tree, 41, first_slot.object, camera.object, 0, LN_UIPropagation::PassThrough);
  LN_UIService::Registration second = MakeRegistration(
      tree, 42, second_slot.object, camera.object, 0, LN_UIPropagation::PassThrough);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  service.RegisterInteractable(first);
  service.RegisterInteractable(second);
  service.ResolveInteractions();
  ASSERT_EQ(service.SetFocus(0, &second_slot.object, std::string()), LN_UIResult::Success);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const bool down) {
    PressKey(device, input, SCA_IInputDevice::DOWNARROWKEY, down);
    service.BeginTick(LN_Event::OnFixedUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    service.RegisterInteractable(first);
    service.RegisterInteractable(second);
    service.ResolveInteractions();
  };
  /* The first press only shows the hidden selection: the grid does not page yet. */
  tick(2, true);
  EXPECT_FALSE(service.GetViewState({tree, 40}).scrolled);
  EXPECT_EQ(service.GetViewState({tree, 40}).first_visible_index, 0);
  tick(3, false);
  tick(4, true);

  const LN_UIService::ViewState &state = service.GetViewState({tree, 40});
  EXPECT_TRUE(state.scrolled);
  EXPECT_EQ(state.first_visible_index, 1);
  EXPECT_EQ(state.last_visible_index, 2);
  EXPECT_EQ(service.GetFocus(0, std::string()), &second_slot.object);
  LN_Value item;
  int32_t item_index = -1;
  ASSERT_TRUE(service.GetBoundItem(&second_slot.object, item, item_index));
  EXPECT_EQ(item_index, 2);
  EXPECT_EQ(item.int_value, 2);
}

TEST(LN_UIService, GridWheelArithmeticSaturatesExtremeCreatorValues)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  LN_UIService::ViewRegistration view;
  view.listener = {tree, 50};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.slots = {&slot.object};
  view.columns = std::numeric_limits<int32_t>::max();
  view.wheel_rows = std::numeric_limits<int32_t>::max();
  for (int index = 0; index < 4; ++index) {
    LN_Value item;
    item.type = LN_ValueType::Int;
    item.exists = true;
    item.int_value = index;
    view.items.push_back(item);
  }

  DEV_InputDevice device;
  LN_InputSnapshot input;
  device.ConvertWheelEvent(std::numeric_limits<int32_t>::min());
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterView(view);
  service.ResolveViews();
  const LN_UIService::ViewState &state = service.GetViewState({tree, 50});
  EXPECT_EQ(state.first_visible_index, 3);
  EXPECT_EQ(state.last_visible_index, 3);
  EXPECT_TRUE(state.at_end);
}

TEST(LN_UIService, ScrollViewClampsWheelAndRestoresTheAuthoredContentTransform)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> content(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  content.object.NodeSetLocalPosition(MT_Vector3(1.0f, 2.0f, 3.0f));
  content.object.NodeUpdateGS(0.0);
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 9};
  view.kind = LN_UIService::ViewKind::Scroll;
  view.viewport = &viewport.object;
  view.content = &content.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.min_offset = MT_Vector3(0.0f, -2.0f, 0.0f);
  view.max_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  view.wheel_step = MT_Vector3(0.0f, -1.0f, 0.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  device.ConvertMoveEvent(100, 100);
  device.ConvertWheelEvent(1);
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterView(view);
  service.ResolveViews();
  const LN_UIService::ViewState &first = service.GetViewState({tree, 9});
  EXPECT_TRUE(first.scrolled);
  EXPECT_NEAR(first.offset.y(), -1.0f, 0.0001f);
  EXPECT_NEAR(first.normalized.y(), 0.5f, 0.0001f);
  EXPECT_FALSE(first.at_start);
  EXPECT_FALSE(first.at_end);
  EXPECT_NEAR(content.object.NodeGetLocalPosition().y(), 1.0f, 0.0001f);

  device.ClearInputs();
  device.ConvertMoveEvent(100, 100);
  device.ConvertWheelEvent(8);
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterView(view);
  service.ResolveViews();
  const LN_UIService::ViewState &clamped = service.GetViewState({tree, 9});
  EXPECT_NEAR(clamped.offset.y(), -2.0f, 0.0001f);
  EXPECT_TRUE(clamped.at_end);
  EXPECT_NEAR(content.object.NodeGetLocalPosition().y(), 0.0f, 0.0001f);

  service.BeginTick(LN_Event::OnFixedUpdate, 3, nullptr);
  service.ResolveViews();
  EXPECT_NEAR(content.object.NodeGetLocalPosition().x(), 1.0f, 0.0001f);
  EXPECT_NEAR(content.object.NodeGetLocalPosition().y(), 2.0f, 0.0001f);
  EXPECT_NEAR(content.object.NodeGetLocalPosition().z(), 3.0f, 0.0001f);
}

TEST(LN_UIService, ScrollViewFocusFollowBringsAFocusedDescendantIntoView)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> content(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  content.node.AddChild(&button.node);
  button.node.SetParentRelation(new KX_NormalParentRelation());
  button.object.NodeSetLocalPosition(MT_Vector3(0.0f, -2.0f, 0.0f));
  content.object.NodeUpdateGS(0.0);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  LN_UIService::Registration focusable = MakeRegistration(
      tree, 10, button.object, camera.object, 0, LN_UIPropagation::Block);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterInteractable(focusable);
  service.ResolveInteractions();
  ASSERT_EQ(service.SetFocus(0, &button.object, std::string()), LN_UIResult::Success);

  LN_UIService::ViewRegistration view;
  view.listener = {tree, 11};
  view.kind = LN_UIService::ViewKind::Scroll;
  view.viewport = &viewport.object;
  view.content = &content.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.min_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  view.max_offset = MT_Vector3(0.0f, 2.0f, 0.0f);
  view.focus_follow = true;

  service.BeginTick(LN_Event::OnFixedUpdate, 2, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  const LN_UIService::ViewState &state = service.GetViewState({tree, 11});
  EXPECT_TRUE(state.scrolled);
  EXPECT_NEAR(state.offset.y(), 1.5f, 0.0001f);
  EXPECT_TRUE(service.ObjectPassesViewClipping(&button.object));

  /* Release while the authored hierarchy is still alive; production object teardown reports the
   * same lifecycle event through NotifyObjectRemoved before destroying scene-graph nodes. */
  service.BeginTick(LN_Event::OnFixedUpdate, 3, nullptr);
  service.ResolveViews();
}

TEST(LN_UIService, GridScrollbarShowsTheRowsInViewAndDragsTheGrid)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> first_slot(scene);
  SceneBoundObject<KX_GameObject> second_slot(scene);
  SceneBoundObject<KX_GameObject> bar(scene);
  SceneBoundObject<KX_GameObject> handle(scene);
  /* As converted from Blender: the handle's own transform is its Location, under a node that
   * holds the parent inverse. */
  SG_Callbacks inverse_callbacks;
  SG_Node parent_inverse(nullptr, scene, inverse_callbacks);
  bar.node.AddChild(&parent_inverse);
  parent_inverse.SetParentRelation(new KX_NormalParentRelation());
  parent_inverse.SetLocalPosition(MT_Vector3(0.0f, 5.0f, 0.0f));
  parent_inverse.AddChild(&handle.node);
  handle.node.SetParentRelation(new KX_NormalParentRelation());
  handle.object.NodeSetLocalPosition(MT_Vector3(0.0f, -5.0f, 0.0f));
  bar.object.NodeUpdateGS(0.0);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  /* Two slots in one column show 2 of 10 rows. The bar is 4 long; its handle is 1 by 0.2. */
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 60};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.slots = {&first_slot.object, &second_slot.object};
  for (int index = 0; index < 10; ++index) {
    LN_Value item;
    item.type = LN_ValueType::Int;
    item.exists = true;
    item.int_value = index;
    view.items.push_back(item);
  }
  view.scrollbar = &bar.object;
  view.scrollbar_handle = &handle.object;
  view.scrollbar_min = MT_Vector3(-0.1f, -2.0f, 0.0f);
  view.scrollbar_max = MT_Vector3(0.1f, 2.0f, 0.0f);
  view.handle_min = MT_Vector3(-0.1f, -0.5f, 0.0f);
  view.handle_max = MT_Vector3(0.1f, 0.5f, 0.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  /* `button`: 1 presses, 0 releases, -1 keeps the button as it is. */
  auto tick = [&](const uint64_t index, const float pointer_y, const int button) {
    CaptureInput(device, input, 100, 100, button);
    view.ray_from = MT_Vector3(0.0f, pointer_y, -2.0f);
    view.ray_to = MT_Vector3(0.0f, pointer_y, 2.0f);
    service.BeginTick(LN_Event::OnFixedUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    service.ResolveInteractions();
    return service.GetViewState({tree, 60});
  };

  /* A fifth of the rows is in view: the handle is a fifth of the bar, at its top. */
  EXPECT_EQ(tick(1, 3.0f, -1).first_visible_index, 0);
  EXPECT_TRUE(bar.object.GetVisible());
  EXPECT_NEAR(handle.object.NodeGetWorldScaling().y(), 0.8f, 0.0001f);
  EXPECT_NEAR(handle.object.NodeGetWorldPosition().y(), 1.6f, 0.0001f);

  /* A press beside the handle brings it under the pointer: halfway is row 4 of 0..8. */
  EXPECT_EQ(tick(2, 0.0f, 1).first_visible_index, 0);
  EXPECT_EQ(service.CaptureCountForTests(), 1u);
  LN_UIService::ViewState state = tick(3, 0.0f, -1);
  EXPECT_TRUE(state.scrolled);
  EXPECT_EQ(state.first_visible_index, 4);
  EXPECT_NEAR(handle.object.NodeGetWorldPosition().y(), 0.0f, 0.0001f);

  /* The drag keeps following the pointer past the bar's end. */
  state = tick(4, -3.0f, -1);
  EXPECT_EQ(state.first_visible_index, 8);
  EXPECT_TRUE(state.at_end);
  EXPECT_NEAR(handle.object.NodeGetWorldPosition().y(), -1.6f, 0.0001f);
  tick(5, -3.0f, 0);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  EXPECT_EQ(tick(6, 3.0f, -1).first_visible_index, 8);

  /* A press on the handle keeps its grip: the handle stays 0.2 below the pointer. */
  tick(7, -1.4f, 1);
  EXPECT_EQ(tick(8, -1.4f, -1).first_visible_index, 8);
  tick(9, 1.0f, -1);
  EXPECT_NEAR(handle.object.NodeGetWorldPosition().y(), 0.8f, 0.0001f);
  tick(10, 1.0f, 0);

  /* Everything fits: the bar and handle hide and take no clicks. */
  view.items.resize(2);
  EXPECT_EQ(tick(11, 0.0f, -1).first_visible_index, 0);
  EXPECT_FALSE(bar.object.GetVisible());
  EXPECT_FALSE(handle.object.GetVisible());
  tick(12, 0.0f, 1);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  tick(13, 0.0f, 0);

  /* Without the view, the bar shows again and the handle is as authored. */
  service.BeginTick(LN_Event::OnFixedUpdate, 14, nullptr);
  service.ResolveViews();
  EXPECT_TRUE(bar.object.GetVisible());
  EXPECT_NEAR(handle.object.NodeGetLocalScaling().y(), 1.0f, 0.0001f);
  EXPECT_NEAR(handle.object.NodeGetLocalPosition().y(), -5.0f, 0.0001f);
}

TEST(LN_UIService, ScrollViewScrollbarMovesTheContent)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> content(scene);
  SceneBoundObject<KX_GameObject> bar(scene);
  SceneBoundObject<KX_GameObject> handle(scene);
  bar.node.AddChild(&handle.node);
  handle.node.SetParentRelation(new KX_NormalParentRelation());
  bar.object.NodeUpdateGS(0.0);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  /* The viewport is 1 high and the content scrolls 2 more: a third is in view. */
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 61};
  view.kind = LN_UIService::ViewKind::Scroll;
  view.viewport = &viewport.object;
  view.content = &content.object;
  view.camera = &camera.object;
  view.min_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  view.max_offset = MT_Vector3(0.0f, 2.0f, 0.0f);
  view.wheel_step = MT_Vector3(0.0f, 0.25f, 0.0f);
  view.cull_outside = false;
  view.scrollbar = &bar.object;
  view.scrollbar_handle = &handle.object;
  view.scrollbar_min = MT_Vector3(-0.1f, -1.0f, 0.0f);
  view.scrollbar_max = MT_Vector3(0.1f, 1.0f, 0.0f);
  view.handle_min = MT_Vector3(-0.1f, -0.25f, 0.0f);
  view.handle_max = MT_Vector3(0.1f, 0.25f, 0.0f);
  view.ray_from = MT_Vector3(0.0f, -1.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, -1.0f, 2.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const int button) {
    CaptureInput(device, input, 100, 100, button);
    service.BeginTick(LN_Event::OnFixedUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    service.ResolveInteractions();
    return service.GetViewState({tree, 61});
  };

  EXPECT_NEAR(tick(1, -1).normalized.y(), 0.0f, 0.0001f);
  EXPECT_NEAR(handle.object.NodeGetLocalScaling().y(), 4.0f / 3.0f, 0.0001f);
  EXPECT_NEAR(handle.object.NodeGetLocalPosition().y(), 2.0f / 3.0f, 0.0001f);

  /* A press at the bar's bottom scrolls to the end. */
  tick(2, 1);
  const LN_UIService::ViewState state = tick(3, -1);
  EXPECT_TRUE(state.at_end);
  EXPECT_NEAR(state.offset.y(), 2.0f, 0.0001f);
  EXPECT_NEAR(content.object.NodeGetLocalPosition().y(), 2.0f, 0.0001f);
  EXPECT_NEAR(handle.object.NodeGetLocalPosition().y(), -2.0f / 3.0f, 0.0001f);
  tick(4, 0);

  /* No scroll range: nothing to scroll, so no scrollbar. */
  view.max_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  tick(5, -1);
  EXPECT_FALSE(bar.object.GetVisible());

  service.BeginTick(LN_Event::OnFixedUpdate, 6, nullptr);
  service.ResolveViews();
}

TEST(LN_UIService, OverlappingViewsRestoreVisibilityAfterTheFinalOwnerReleases)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> outer_viewport(scene);
  SceneBoundObject<KX_GameObject> inner_viewport(scene);
  SceneBoundObject<KX_GameObject> outer_content(scene);
  SceneBoundObject<KX_GameObject> inner_content(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  outer_content.node.AddChild(&inner_content.node);
  inner_content.node.SetParentRelation(new KX_NormalParentRelation());
  inner_content.node.AddChild(&item.node);
  item.node.SetParentRelation(new KX_NormalParentRelation());
  item.object.NodeSetLocalPosition(MT_Vector3(4.0f, 0.0f, 0.0f));
  outer_content.object.NodeUpdateGS(0.0);

  LN_UIService::ViewRegistration outer;
  outer.listener = {tree, 60};
  outer.kind = LN_UIService::ViewKind::Scroll;
  outer.viewport = &outer_viewport.object;
  outer.content = &outer_content.object;
  outer.camera = &camera.object;
  outer.cull_outside = true;
  LN_UIService::ViewRegistration inner = outer;
  inner.listener = {tree, 61};
  inner.viewport = &inner_viewport.object;
  inner.content = &inner_content.object;

  ASSERT_TRUE(item.object.GetVisible());
  service.BeginTick(LN_Event::OnFixedUpdate, 1, nullptr);
  service.RegisterView(outer);
  service.RegisterView(inner);
  service.ResolveViews();
  EXPECT_FALSE(item.object.GetVisible());

  /* Releasing either owner first must not restore visibility while the other view still clips the
   * shared descendant. */
  service.BeginTick(LN_Event::OnFixedUpdate, 2, nullptr);
  service.RegisterView(inner);
  service.ResolveViews();
  EXPECT_FALSE(item.object.GetVisible());

  service.BeginTick(LN_Event::OnFixedUpdate, 3, nullptr);
  service.ResolveViews();
  EXPECT_TRUE(item.object.GetVisible());
}

TEST(LN_UIService, SharedScrollContentHasOneDeterministicOwner)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> low_viewport(scene);
  SceneBoundObject<KX_GameObject> high_viewport(scene);
  SceneBoundObject<KX_GameObject> content(scene);
  const LN_RuntimeTree *low_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_RuntimeTree *high_tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(3));

  content.object.NodeSetLocalPosition(MT_Vector3(1.0f, 2.0f, 3.0f));
  content.object.NodeUpdateGS(0.0);
  LN_UIService::ViewRegistration low;
  low.listener = {low_tree, 70};
  low.kind = LN_UIService::ViewKind::Scroll;
  low.viewport = &low_viewport.object;
  low.content = &content.object;
  low.camera = &camera.object;
  low.min_offset = MT_Vector3(0.0f, -10.0f, 0.0f);
  low.max_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  low.wheel_step = MT_Vector3(0.0f, -1.0f, 0.0f);
  low.priority = 1;
  LN_UIService::ViewRegistration high = low;
  high.listener = {high_tree, 71};
  high.viewport = &high_viewport.object;
  high.wheel_step = MT_Vector3(0.0f, -2.0f, 0.0f);
  high.priority = 2;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  device.ConvertWheelEvent(1);
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnFixedUpdate, 1, &input);
  service.RegisterView(low);
  service.RegisterView(high);
  service.ResolveViews();
  EXPECT_FALSE(service.GetViewState({low_tree, 70}).valid);
  EXPECT_TRUE(service.GetViewState({high_tree, 71}).valid);
  EXPECT_NEAR(content.object.NodeGetLocalPosition().y(), 0.0f, 0.0001f);

  /* When the winner disappears, the remaining declaration takes ownership from the authored
   * transform rather than inheriting the former owner's transient offset. */
  service.BeginTick(LN_Event::OnFixedUpdate, 2, &input);
  service.RegisterView(low);
  service.ResolveViews();
  EXPECT_TRUE(service.GetViewState({low_tree, 70}).valid);
  EXPECT_NEAR(content.object.NodeGetLocalPosition().y(), 1.0f, 0.0001f);

  service.BeginTick(LN_Event::OnFixedUpdate, 3, nullptr);
  service.ResolveViews();
  EXPECT_NEAR(content.object.NodeGetLocalPosition().y(), 2.0f, 0.0001f);
}

/* UI Elements: objects whose UI Element panel is enabled. Their listener is keyed by the object
 * and every event carries the element's Action name. */

LN_UIService::ElementSettings MakeElementSettings(const char *action)
{
  LN_UIService::ElementSettings settings;
  settings.action = action;
  return settings;
}

TEST(LN_UIService, ElementsReportClicksWithTheirActionName)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> play(scene);
  SceneBoundObject<KX_GameObject> exit(scene);
  LN_UIService::Registration play_registration = MakeRegistration(
      nullptr, 0, play.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration exit_registration = MakeRegistration(
      nullptr, 0, exit.object, camera.object, 0, LN_UIPropagation::Block);
  exit_registration.world_position = MT_Vector3(4.0f, 0.0f, 0.0f);
  const LN_UIService::ElementSettings play_settings = MakeElementSettings("play");
  const LN_UIService::ElementSettings exit_settings = MakeElementSettings("exit");

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&play.object, play_settings, play_registration);
    service.RegisterElement(&exit.object, exit_settings, exit_registration);
    service.ResolveInteractions();
  };

  CaptureInput(device, input, 100, 100, 1);
  tick(1);
  const LN_UIService::ElementEvent *enter = service.FindElementEvent(LN_UIElementEvent::Enter,
                                                                     std::string());
  ASSERT_NE(enter, nullptr);
  EXPECT_EQ(enter->action, "play");
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::Press, "play"), nullptr);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, std::string()), nullptr);

  CaptureInput(device, input, 100, 100, 0);
  tick(2);
  const LN_UIService::ElementEvent *click = service.FindElementEvent(LN_UIElementEvent::Click,
                                                                     std::string());
  ASSERT_NE(click, nullptr);
  EXPECT_EQ(click->action, "play");
  EXPECT_EQ(manager.ResolveObjectHandle(click->object), &play.object);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Click, "play"), nullptr);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "exit"), nullptr);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Release, "play"), nullptr);

  /* A Logic + Physics step reads no frame events: a click cannot repeat in several steps. */
  service.BeginPassivePhase(LN_Event::OnFixedUpdate, &input);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, std::string()), nullptr);
}

TEST(LN_UIService, PassivePhaseKeepsLevelStatesButDropsOneTickEdges)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 3, button.object, camera.object, 0, LN_UIPropagation::Block);

  service.BeginTick(LN_Event::OnUpdate, 1, nullptr);
  service.RegisterInteractable(registration);
  service.ResolveInteractions();
  EXPECT_TRUE(service.GetInteractionState({tree, 3}).entered);
  EXPECT_TRUE(service.GetInteractionState({tree, 3}).over);

  service.BeginPassivePhase(LN_Event::OnFixedUpdate, nullptr);
  EXPECT_FALSE(service.GetInteractionState({tree, 3}).entered);
  EXPECT_TRUE(service.GetInteractionState({tree, 3}).over);
}

TEST(LN_UIService, ElementWithIgnoredPointerLetsTheElementBehindReceiveTheClick)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> label(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  LN_UIService::ElementSettings label_settings = MakeElementSettings("label");
  label_settings.pointer = LN_UIPropagation::Ignore;
  label_settings.priority = 10;
  const LN_UIService::ElementSettings button_settings = MakeElementSettings("button");
  const LN_UIService::Registration label_registration = MakeRegistration(
      nullptr, 0, label.object, camera.object, 0, LN_UIPropagation::Block);
  const LN_UIService::Registration button_registration = MakeRegistration(
      nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  for (const int value : {1, 0}) {
    CaptureInput(device, input, 100, 100, value);
    service.BeginTick(LN_Event::OnUpdate, uint64_t(value == 1 ? 1 : 2), &input);
    service.RegisterElement(&label.object, label_settings, label_registration);
    service.RegisterElement(&button.object, button_settings, button_registration);
    service.ResolveInteractions();
  }
  const LN_UIService::ElementEvent *click = service.FindElementEvent(LN_UIElementEvent::Click,
                                                                     std::string());
  ASSERT_NE(click, nullptr);
  EXPECT_EQ(click->action, "button");
}

TEST(LN_UIService, ElementFocusModesControlNavigationAndFocusEvents)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> keyboard(scene);
  SceneBoundObject<KX_GameObject> click_only(scene);
  SceneBoundObject<KX_GameObject> never(scene);
  LN_UIService::ElementSettings keyboard_settings = MakeElementSettings("keyboard");
  keyboard_settings.tab_order = 2;
  LN_UIService::ElementSettings click_settings = MakeElementSettings("click");
  click_settings.focus = LN_UIElementFocus::Click;
  click_settings.tab_order = 1;
  LN_UIService::ElementSettings never_settings = MakeElementSettings("never");
  never_settings.focus = LN_UIElementFocus::None;
  LN_UIService::Registration keyboard_registration = MakeRegistration(
      nullptr, 0, keyboard.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration click_registration = MakeRegistration(
      nullptr, 0, click_only.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration never_registration = MakeRegistration(
      nullptr, 0, never.object, camera.object, 0, LN_UIPropagation::Block);
  for (LN_UIService::Registration *registration :
       {&keyboard_registration, &click_registration, &never_registration})
  {
    registration->ray_from = registration->ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);
  }

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&keyboard.object, keyboard_settings, keyboard_registration);
    service.RegisterElement(&click_only.object, click_settings, click_registration);
    service.RegisterElement(&never.object, never_settings, never_registration);
    service.ResolveInteractions();
  };

  /* Set UI Focus may select a click-only element, but a mouse player does not see it: no Focus
   * event yet. */
  tick(1);
  EXPECT_EQ(service.SetFocus(0, &never.object, std::string()), LN_UIResult::InvalidTarget);
  EXPECT_EQ(service.SetFocus(0, &click_only.object, std::string()), LN_UIResult::Success);
  tick(2);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Focus, std::string()), nullptr);

  /* Tab skips the click-only element even though its Tab Order is lower. */
  PressKey(device, input, SCA_IInputDevice::TABKEY, true);
  tick(3);
  EXPECT_EQ(service.GetFocus(0, std::string()), &keyboard.object);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::Focus, "keyboard"), nullptr);

  /* Keyboard play shows focus set by game logic at once. */
  PressKey(device, input, SCA_IInputDevice::TABKEY, false);
  EXPECT_EQ(service.SetFocus(0, &click_only.object, std::string()), LN_UIResult::Success);
  tick(4);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::FocusLost, "keyboard"), nullptr);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::Focus, "click"), nullptr);

  /* Moving the mouse hides the selection again. */
  CaptureInput(device, input, 0, 0);
  CaptureInput(device, input, 40, 0);
  tick(5);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::FocusLost, "click"), nullptr);
  EXPECT_EQ(service.GetFocus(0, std::string()), &click_only.object);
}

TEST(LN_UIService, DraggingAnElementOntoADropTargetElementReportsTheTarget)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.drag_threshold = 1.0f;
  item_settings.drop_group = "Inventory";
  item_settings.priority = 10;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  slot_settings.drop_group = "Inventory";
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::Block);
  const LN_UIService::Registration slot_registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, slot_registration);
    service.ResolveInteractions();
  };

  CaptureInput(device, input, 100, 100, 1);
  tick(1);
  CaptureInput(device, input, 103, 100);
  tick(2);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "potion"), nullptr);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::Drag, "potion"), nullptr);

  CaptureInput(device, input, 103, 100, 0);
  tick(3);
  const LN_UIService::ElementEvent *drop = service.FindElementEvent(LN_UIElementEvent::Drop,
                                                                    std::string());
  ASSERT_NE(drop, nullptr);
  EXPECT_EQ(drop->action, "potion");
  EXPECT_EQ(manager.ResolveObjectHandle(drop->object), &item.object);
  EXPECT_EQ(manager.ResolveObjectHandle(drop->target), &slot.object);
  /* Target Action tells where it was dropped (a bag slot, a chest slot...). */
  EXPECT_EQ(drop->target_action, "slot");
  EXPECT_FALSE(drop->needs_world_target);
  /* A drag ends without a click. */
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, std::string()), nullptr);
  /* The slot hears that something was dropped on it, and what, and where it came from. */
  const LN_UIService::ElementEvent *received = service.FindElementEvent(
      LN_UIElementEvent::DropReceived, std::string());
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->action, "slot");
  EXPECT_EQ(received->target_action, "potion");
  EXPECT_EQ(manager.ResolveObjectHandle(received->object), &slot.object);
  EXPECT_EQ(manager.ResolveObjectHandle(received->target), &item.object);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::DropOutside, std::string()), nullptr);
}

TEST(LN_UIService, ADropWhereNoElementTakesItIsAlsoADropOutside)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  LN_UIService::ElementSettings settings = MakeElementSettings("item");
  settings.draggable = true;
  settings.drag_threshold = 1.0f;
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::Block);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&item.object, settings, registration);
    service.ResolveInteractions();
  };
  CaptureInput(device, input, 100, 100, 1);
  tick(1);
  CaptureInput(device, input, 103, 100);
  tick(2);
  CaptureInput(device, input, 103, 100, 0);
  tick(3);
  const LN_UIService::ElementEvent *drop = service.FindElementEvent(LN_UIElementEvent::Drop,
                                                                    "item");
  const LN_UIService::ElementEvent *outside = service.FindElementEvent(
      LN_UIElementEvent::DropOutside, "item");
  ASSERT_NE(drop, nullptr);
  ASSERT_NE(outside, nullptr);
  EXPECT_TRUE(outside->needs_world_target);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::DropReceived, std::string()), nullptr);
}

TEST(LN_UIService, GrabCarriesAnElementToADropTargetWithKeysOrGamepad)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.tab_order = 0;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  slot_settings.tab_order = 1;
  LN_UIService::ElementSettings button_settings = MakeElementSettings("button");
  button_settings.tab_order = 2;
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration slot_registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration button_registration = MakeRegistration(
      nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::PassThrough);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int key, const bool down) {
    PressKey(device, input, SCA_IInputDevice::SCA_EnumInputs(key), down);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, slot_registration);
    service.RegisterElement(&button.object, button_settings, button_registration);
    service.ResolveInteractions();
  };
  auto press = [&](const int key) {
    tick(key, true);
    tick(key, false);
  };
  auto fired = [&](const LN_UIElementEvent type, const char *action) {
    return service.FindElementEvent(type, action) != nullptr;
  };
  tick(SCA_IInputDevice::GKEY, false);
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);

  /* The first Grab only shows the selection; the second picks the item up. */
  tick(SCA_IInputDevice::GKEY, true);
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart, "potion"));
  tick(SCA_IInputDevice::GKEY, false);
  tick(SCA_IInputDevice::GKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragStart, "potion"));
  EXPECT_TRUE(fired(LN_UIElementEvent::Drag, "potion"));
  tick(SCA_IInputDevice::GKEY, false);
  EXPECT_TRUE(fired(LN_UIElementEvent::Drag, "potion"));

  /* Tab moves the selection to the slot; Accept puts the item down there, without a click. */
  press(SCA_IInputDevice::TABKEY);
  EXPECT_EQ(service.GetFocus(0, std::string()), &slot.object);
  tick(SCA_IInputDevice::RETKEY, true);
  const LN_UIService::ElementEvent *drop = service.FindElementEvent(LN_UIElementEvent::Drop,
                                                                    "potion");
  ASSERT_NE(drop, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(drop->target), &slot.object);
  const LN_UIService::ElementEvent *received = service.FindElementEvent(
      LN_UIElementEvent::DropReceived, "slot");
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(received->target), &item.object);
  EXPECT_FALSE(fired(LN_UIElementEvent::Click, ""));
  EXPECT_FALSE(fired(LN_UIElementEvent::Drag, "potion"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  tick(SCA_IInputDevice::RETKEY, false);

  /* Not carrying, Accept clicks again. */
  tick(SCA_IInputDevice::RETKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::Click, "slot"));
  tick(SCA_IInputDevice::RETKEY, false);
}

/* With the mouse (the selection hidden), keys act on what the mouse points at, not on a selection
 * the player cannot see: Grab picks it up into the mouse and Grab puts it down where the mouse
 * is, Accept clicks it, navigation goes on from it. */
TEST(LN_UIService, KeysActOnWhatTheMousePointsAtWhileTheSelectionIsHidden)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  LN_UIService::ElementSettings first_settings = MakeElementSettings("first");
  first_settings.draggable = true;
  first_settings.tab_order = 0;
  LN_UIService::ElementSettings second_settings = MakeElementSettings("second");
  second_settings.draggable = true;
  second_settings.tab_order = 1;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  slot_settings.tab_order = 2;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  /* The mouse ray hits only the element it is over. */
  KX_GameObject *hovered = nullptr;
  auto registration = [&](SceneBoundObject<KX_GameObject> &element) {
    LN_UIService::Registration result = MakeRegistration(
        nullptr, 0, element.object, camera.object, 0, LN_UIPropagation::Block);
    if (&element.object != hovered) {
      result.ray_from = result.ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);
    }
    return result;
  };
  /* A tenth of a second per frame. */
  auto resolve = [&]() {
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterElement(&first.object, first_settings, registration(first));
    service.RegisterElement(&second.object, second_settings, registration(second));
    service.RegisterElement(&slot.object, slot_settings, registration(slot));
    service.ResolveInteractions();
  };
  int mouse_x = 0;
  auto move_onto = [&](SceneBoundObject<KX_GameObject> &element, const int x) {
    hovered = &element.object;
    mouse_x = x;
    CaptureInput(device, input, x, 100);
    resolve();
  };
  /* The mouse stays where it is. */
  auto key = [&](const SCA_IInputDevice::SCA_EnumInputs code, const bool down) {
    device.ClearInputs();
    device.ConvertMoveEvent(mouse_x, 100);
    device.ConvertEvent(code, down ? 1 : 0, 0);
    input.Capture(&device, nullptr);
    resolve();
  };
  auto fired = [&](const LN_UIElementEvent type, const char *action) {
    return service.FindElementEvent(type, action) != nullptr;
  };
  resolve();
  /* The mouse moved onto the second item before the menu opened (Mouse Look turning), and right
   * after (the pointer report a shown cursor can bring): neither points for the keys, whose
   * first Grab only shows the selection. */
  move_onto(second, 100);
  move_onto(second, 110);
  service.AddMenuForTests(0,
                          LN_UIMenuRouting::Modal,
                          LN_UIMenuBackBehavior::CloseMenu,
                          {&first.object, &second.object, &slot.object});
  ASSERT_EQ(service.SetFocus(0, &first.object, std::string()), LN_UIResult::Success);
  move_onto(second, 120);
  for (int frame = 0; frame < 3; frame++) {
    move_onto(second, 120);
  }
  key(SCA_IInputDevice::GKEY, true);
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart, "second"));
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart, "first"));
  key(SCA_IInputDevice::GKEY, false);

  /* Moved in the menu: Grab picks up the second item at once, into the mouse. */
  move_onto(second, 130);
  key(SCA_IInputDevice::GKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragStart, "second"));
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart, "first"));
  ASSERT_EQ(service.CaptureCountForTests(), 1u);
  key(SCA_IInputDevice::GKEY, false);

  /* It follows the mouse onto the slot; Grab puts it down there and picks nothing up again. */
  move_onto(slot, 140);
  key(SCA_IInputDevice::GKEY, true);
  const LN_UIService::ElementEvent *received = service.FindElementEvent(
      LN_UIElementEvent::DropReceived, "slot");
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(received->target), &second.object);
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart, "slot"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  key(SCA_IInputDevice::GKEY, false);

  /* Enter presses what the mouse points at, not the hidden selection. */
  move_onto(first, 90);
  key(SCA_IInputDevice::RETKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::Click, "first"));
  key(SCA_IInputDevice::RETKEY, false);

  /* Moved onto the second item again (the selection hides), Tab goes on from it. */
  move_onto(second, 110);
  key(SCA_IInputDevice::TABKEY, true);
  EXPECT_EQ(service.GetFocus(0, std::string()), &slot.object);
  key(SCA_IInputDevice::TABKEY, false);
}

/* What is carried follows the device used last: moving the mouse while the keys carry an item
 * hands it to the mouse (a click puts it down where the mouse is), and a navigation key while
 * the mouse holds it hands it to the keys (it goes where the selection goes). */
TEST(LN_UIService, CarriedItemFollowsTheDeviceUsedLast)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  LN_UIService::ElementSettings first_settings = MakeElementSettings("first");
  first_settings.draggable = true;
  first_settings.tab_order = 0;
  LN_UIService::ElementSettings second_settings = MakeElementSettings("second");
  second_settings.draggable = true;
  second_settings.tab_order = 1;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  slot_settings.tab_order = 2;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  /* The mouse ray hits only the element it is over. */
  KX_GameObject *hovered = nullptr;
  auto registration = [&](SceneBoundObject<KX_GameObject> &element) {
    LN_UIService::Registration result = MakeRegistration(
        nullptr, 0, element.object, camera.object, 0, LN_UIPropagation::Block);
    if (&element.object != hovered) {
      result.ray_from = result.ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);
    }
    return result;
  };
  /* A tenth of a second per frame. */
  auto resolve = [&]() {
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterElement(&first.object, first_settings, registration(first));
    service.RegisterElement(&second.object, second_settings, registration(second));
    service.RegisterElement(&slot.object, slot_settings, registration(slot));
    service.ResolveInteractions();
  };
  int mouse_x = 0;
  auto move_onto = [&](SceneBoundObject<KX_GameObject> &element, const int x) {
    hovered = &element.object;
    mouse_x = x;
    CaptureInput(device, input, x, 100);
    resolve();
  };
  /* The mouse stays where it is. */
  auto key = [&](const SCA_IInputDevice::SCA_EnumInputs code, const bool down) {
    device.ClearInputs();
    device.ConvertMoveEvent(mouse_x, 100);
    device.ConvertEvent(code, down ? 1 : 0, 0);
    input.Capture(&device, nullptr);
    resolve();
  };
  auto fired = [&](const LN_UIElementEvent type, const char *action) {
    return service.FindElementEvent(type, action) != nullptr;
  };
  /* In a menu, where the UI has the mouse. */
  service.AddMenuForTests(0,
                          LN_UIMenuRouting::Modal,
                          LN_UIMenuBackBehavior::CloseMenu,
                          {&first.object, &second.object, &slot.object});
  resolve();
  ASSERT_EQ(service.SetFocus(0, &first.object, std::string()), LN_UIResult::Success);

  /* The keys carry the first item (the first Grab shows the selection, the second picks up). */
  key(SCA_IInputDevice::GKEY, true);
  key(SCA_IInputDevice::GKEY, false);
  key(SCA_IInputDevice::GKEY, true);
  ASSERT_TRUE(fired(LN_UIElementEvent::DragStart, "first"));
  key(SCA_IInputDevice::GKEY, false);

  /* The mouse moves onto the slot: it holds the item now, which is not put back nor started
   * again; its click puts it down there. */
  move_onto(slot, 100);
  move_onto(slot, 120);
  EXPECT_FALSE(fired(LN_UIElementEvent::DragCancel, "first"));
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart, "first"));
  EXPECT_TRUE(fired(LN_UIElementEvent::Drag, "first"));
  EXPECT_EQ(service.CaptureCountForTests(), 1u);
  key(SCA_IInputDevice::LEFTMOUSE, true);
  key(SCA_IInputDevice::LEFTMOUSE, false);
  const LN_UIService::ElementEvent *received = service.FindElementEvent(
      LN_UIElementEvent::DropReceived, "slot");
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(received->target), &first.object);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  /* Grab with the mouse over the second item picks it up into the mouse. */
  move_onto(second, 100);
  move_onto(second, 110);
  key(SCA_IInputDevice::GKEY, true);
  ASSERT_TRUE(fired(LN_UIElementEvent::DragStart, "second"));
  key(SCA_IInputDevice::GKEY, false);

  /* Tab: the keys carry it now. The first press shows the selection (on the item), the next
   * moves it on to the slot, and Grab puts it down on the slot, not under the mouse. */
  key(SCA_IInputDevice::TABKEY, true);
  key(SCA_IInputDevice::TABKEY, false);
  EXPECT_FALSE(fired(LN_UIElementEvent::DragCancel, "second"));
  key(SCA_IInputDevice::TABKEY, true);
  key(SCA_IInputDevice::TABKEY, false);
  EXPECT_EQ(service.GetFocus(0, std::string()), &slot.object);
  key(SCA_IInputDevice::GKEY, true);
  received = service.FindElementEvent(LN_UIElementEvent::DropReceived, "slot");
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(received->target), &second.object);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  key(SCA_IInputDevice::GKEY, false);
}

/* While the mouse holds an item, Grab puts it down: before a frame update it is not the game's,
 * also with the selection on a slot that is no drag source. */
TEST(LN_UIService, GrabIsClaimedBeforeFixedStepsWhileTheMouseHoldsAnItem)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("item");
  item_settings.draggable = true;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::Block);
  /* The mouse is over the item only. */
  LN_UIService::Registration slot_registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::Block);
  slot_registration.ray_from = slot_registration.ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto frame = [&](const int x, const SCA_IInputDevice::SCA_EnumInputs code, const int value) {
    device.ClearInputs();
    device.ConvertMoveEvent(x, 100);
    if (value >= 0) {
      device.ConvertEvent(code, value, 0);
    }
    input.Capture(&device, nullptr);
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, slot_registration);
    service.ResolveInteractions();
  };
  service.AddMenuForTests(
      0, LN_UIMenuRouting::Modal, LN_UIMenuBackBehavior::CloseMenu, {&item.object, &slot.object});
  for (int step = 0; step < 4; step++) {
    frame(100, SCA_IInputDevice::GKEY, -1);
  }
  /* Moved over the item, Grab picks it up into the mouse. */
  frame(110, SCA_IInputDevice::GKEY, -1);
  frame(110, SCA_IInputDevice::GKEY, 1);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "item"), nullptr);
  frame(110, SCA_IInputDevice::GKEY, 0);
  ASSERT_EQ(service.SetFocus(0, &slot.object, std::string()), LN_UIResult::Success);
  frame(110, SCA_IInputDevice::GKEY, -1);
  device.ClearInputs();
  device.ConvertMoveEvent(110, 100);
  device.ConvertEvent(SCA_IInputDevice::GKEY, 1, 0);
  input.Capture(&device, nullptr);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_TRUE(manager.GetInputConsumption().IsPressConsumed(SCA_IInputDevice::GKEY));
}

TEST(LN_UIService, GrabIsClaimedBeforeFixedStepsOnlyWhereItPicksUpOrPutsDown)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.tab_order = 0;
  LN_UIService::ElementSettings button_settings = MakeElementSettings("button");
  button_settings.tab_order = 1;
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration button_registration = MakeRegistration(
      nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::PassThrough);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int key, const bool down) {
    PressKey(device, input, SCA_IInputDevice::SCA_EnumInputs(key), down);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&button.object, button_settings, button_registration);
    service.ResolveInteractions();
  };
  const LN_InputConsumption &consumed = manager.GetInputConsumption();
  tick(SCA_IInputDevice::GKEY, false);

  /* A button is selected: G stays with the game's fixed steps. */
  ASSERT_EQ(service.SetFocus(0, &button.object, std::string()), LN_UIResult::Success);
  tick(SCA_IInputDevice::GKEY, false);
  PressKey(device, input, SCA_IInputDevice::GKEY, true);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_FALSE(consumed.IsPressConsumed(SCA_IInputDevice::GKEY));
  tick(SCA_IInputDevice::GKEY, false);

  /* A draggable item is selected: G picks it up, so the fixed steps don't see it. */
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);
  tick(SCA_IInputDevice::GKEY, false);
  PressKey(device, input, SCA_IInputDevice::GKEY, true);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_TRUE(consumed.IsPressConsumed(SCA_IInputDevice::GKEY));
  /* The first Grab only shows the selection; the second picks the item up. */
  tick(SCA_IInputDevice::GKEY, false);
  tick(SCA_IInputDevice::GKEY, true);
  tick(SCA_IInputDevice::GKEY, false);
  tick(SCA_IInputDevice::GKEY, true);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "potion"), nullptr);
  tick(SCA_IInputDevice::GKEY, false);

  /* Carrying it over the button: gamepad Y puts it down, so it is the UI's too. */
  tick(SCA_IInputDevice::TABKEY, true);
  tick(SCA_IInputDevice::TABKEY, false);
  ASSERT_EQ(service.GetFocus(0, std::string()), &button.object);
  LN_GamepadSnapshot gamepad;
  gamepad.connected = true;
  gamepad.buttons[GAME_INPUT_GAMEPAD_Y] = 1;
  gamepad.pressed[GAME_INPUT_GAMEPAD_Y] = 1;
  input.SetGamepadForTests(0, gamepad);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_TRUE(consumed.IsGamepadPressConsumed(0, GAME_INPUT_GAMEPAD_Y));
}

TEST(LN_UIService, AClickOnACarriedElementPutsItBackAndPressesNothingBehindIt)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.priority = 10;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 10, LN_UIPropagation::Block);
  const LN_UIService::Registration slot_registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&]() {
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, slot_registration);
    service.ResolveInteractions();
  };
  auto key = [&](const SCA_IInputDevice::SCA_EnumInputs code, const bool down) {
    PressKey(device, input, code, down);
    tick();
  };
  key(SCA_IInputDevice::GKEY, false);
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);
  key(SCA_IInputDevice::GKEY, true);
  key(SCA_IInputDevice::GKEY, false);
  key(SCA_IInputDevice::GKEY, true);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "potion"), nullptr);
  key(SCA_IInputDevice::GKEY, false);

  /* The pointer is over the floating item, the slot behind it. */
  CaptureInput(device, input, 100, 100, 1);
  tick();
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::DragCancel, "potion"), nullptr);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Press, "potion"), nullptr);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Press, "slot"), nullptr);
  CaptureInput(device, input, 100, 100, 0);
  tick();
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "slot"), nullptr);
}

/* Click to Pick Up: a click picks an element up, it follows the pointer with the button up, and
 * the next click puts it down with the same events as a drag. */
TEST(LN_UIService, AClickPicksUpAndTheNextClickPutsDown)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.click_pick_up = true;
  item_settings.priority = 10;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  /* Pass Through: only the pick-up keeps the clicks from the game. */
  item_settings.pointer = LN_UIPropagation::PassThrough;
  slot_settings.pointer = LN_UIPropagation::PassThrough;
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 10, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration slot_registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::PassThrough);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&]() {
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, slot_registration);
    service.ResolveInteractions();
  };
  auto fired = [&](const LN_UIElementEvent type, const char *action) {
    return service.FindElementEvent(type, action) != nullptr;
  };

  /* A click picks it up instead of clicking it. */
  CaptureInput(device, input, 100, 100, 1);
  tick();
  CaptureInput(device, input, 100, 100, 0);
  tick();
  EXPECT_TRUE(fired(LN_UIElementEvent::DragStart, "potion"));
  EXPECT_FALSE(fired(LN_UIElementEvent::Click, "potion"));
  EXPECT_FALSE(fired(LN_UIElementEvent::Click, "slot"));
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  /* With the button up it stays picked up and follows the pointer. */
  CaptureInput(device, input, 104, 100);
  tick();
  CaptureInput(device, input, 110, 100);
  tick();
  EXPECT_TRUE(fired(LN_UIElementEvent::Drag, "potion"));
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  /* The next click puts it down on the slot: no press or click of the slot, and the click is not
   * the game's. */
  CaptureInput(device, input, 110, 100, 1);
  tick();
  EXPECT_FALSE(fired(LN_UIElementEvent::Press, "slot"));
  EXPECT_TRUE(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::LEFTMOUSE));
  CaptureInput(device, input, 110, 100, 0);
  tick();
  const LN_UIService::ElementEvent *drop = service.FindElementEvent(LN_UIElementEvent::Drop,
                                                                    "potion");
  ASSERT_NE(drop, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(drop->target), &slot.object);
  EXPECT_TRUE(fired(LN_UIElementEvent::DropReceived, "slot"));
  EXPECT_FALSE(fired(LN_UIElementEvent::Click, "slot"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  /* Pressing and moving still drags, and letting go puts it down. */
  CaptureInput(device, input, 100, 100, 1);
  tick();
  CaptureInput(device, input, 120, 100);
  tick();
  EXPECT_TRUE(fired(LN_UIElementEvent::DragStart, "potion"));
  CaptureInput(device, input, 120, 100, 0);
  tick();
  EXPECT_TRUE(fired(LN_UIElementEvent::DropReceived, "slot"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  /* A right click puts back what a click picked up; it is not a Secondary Click. */
  CaptureInput(device, input, 100, 100, 1);
  tick();
  CaptureInput(device, input, 100, 100, 0);
  tick();
  ASSERT_EQ(service.CaptureCountForTests(), 1u);
  device.ClearInputs();
  device.ConvertMoveEvent(100, 100);
  device.ConvertEvent(SCA_IInputDevice::RIGHTMOUSE, 1, 0);
  input.Capture(&device, nullptr);
  tick();
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel, "potion"));
  EXPECT_FALSE(fired(LN_UIElementEvent::SecondaryClick, "slot"));
  EXPECT_FALSE(fired(LN_UIElementEvent::Drop, "potion"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  EXPECT_TRUE(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::RIGHTMOUSE));
  device.ClearInputs();
  device.ConvertMoveEvent(100, 100);
  device.ConvertEvent(SCA_IInputDevice::RIGHTMOUSE, 0, 0);
  input.Capture(&device, nullptr);
  tick();

  /* Without Click to Pick Up a click is a click. */
  item_settings.click_pick_up = false;
  CaptureInput(device, input, 100, 100, 1);
  tick();
  CaptureInput(device, input, 100, 100, 0);
  tick();
  EXPECT_TRUE(fired(LN_UIElementEvent::Click, "potion"));
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart, "potion"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

TEST(LN_UIService, ClickToPickUpEdgeCases)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.click_pick_up = true;
  item_settings.priority = 10;
  item_settings.pointer = LN_UIPropagation::PassThrough;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  slot_settings.pointer = LN_UIPropagation::PassThrough;
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 10, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration slot_registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::PassThrough);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&]() {
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, slot_registration);
    service.ResolveInteractions();
  };
  auto mouse = [&](const int x, const int button) {
    CaptureInput(device, input, x, 100, button);
    tick();
  };
  auto key = [&](const SCA_IInputDevice::SCA_EnumInputs code, const bool down) {
    device.ClearInputs();
    device.ConvertMoveEvent(100, 100);
    device.ConvertEvent(code, down ? 1 : 0, 0);
    input.Capture(&device, nullptr);
    tick();
  };
  auto fired = [&](const LN_UIElementEvent type, const char *action) {
    return service.FindElementEvent(type, action) != nullptr;
  };

  /* A flick past the threshold that lets go on the same frame is a drag, not a pick-up. */
  mouse(100, 1);
  mouse(120, 0);
  EXPECT_TRUE(fired(LN_UIElementEvent::DropReceived, "slot"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  /* Drag Threshold 0 still picks up on a click. */
  item_settings.drag_threshold = 0.0f;
  mouse(100, 1);
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart, "potion"));
  mouse(100, 0);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragStart, "potion"));
  ASSERT_EQ(service.CaptureCountForTests(), 1u);

  /* Enter while it is held does not click it. */
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);
  key(SCA_IInputDevice::RETKEY, true);
  key(SCA_IInputDevice::RETKEY, false);
  key(SCA_IInputDevice::RETKEY, true);
  EXPECT_FALSE(fired(LN_UIElementEvent::Click, "potion"));
  EXPECT_TRUE(manager.GetInputConsumption().IsPressConsumed(SCA_IInputDevice::RETKEY));
  key(SCA_IInputDevice::RETKEY, false);
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  /* Before a frame update, the put-down click is already not the game's. */
  CaptureInput(device, input, 100, 100, 1);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_TRUE(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::LEFTMOUSE));
  tick();
  /* The put-down has no Release: its press was no Press either. */
  mouse(100, 0);
  EXPECT_TRUE(fired(LN_UIElementEvent::DropReceived, "slot"));
  EXPECT_FALSE(fired(LN_UIElementEvent::Release, "potion"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  /* A right click on what Accept picked up puts it back and clicks nothing. After the mouse, the
   * first Enter only shows the selection. */
  key(SCA_IInputDevice::RETKEY, true);
  key(SCA_IInputDevice::RETKEY, false);
  key(SCA_IInputDevice::RETKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragStart, "potion"));
  key(SCA_IInputDevice::RETKEY, false);
  ASSERT_EQ(service.CaptureCountForTests(), 1u);
  key(SCA_IInputDevice::RIGHTMOUSE, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel, "potion"));
  EXPECT_FALSE(fired(LN_UIElementEvent::SecondaryClick, "slot"));
  EXPECT_TRUE(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::RIGHTMOUSE));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

TEST(LN_UIService, AcceptPicksUpWhatAClickPicksUp)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.click_pick_up = true;
  item_settings.tab_order = 0;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  slot_settings.tab_order = 1;
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration slot_registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::PassThrough);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int key, const bool down) {
    PressKey(device, input, SCA_IInputDevice::SCA_EnumInputs(key), down);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, slot_registration);
    service.ResolveInteractions();
  };
  auto fired = [&](const LN_UIElementEvent type, const char *action) {
    return service.FindElementEvent(type, action) != nullptr;
  };
  tick(SCA_IInputDevice::RETKEY, false);
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);
  /* The first press only shows the selection; the next one picks it up instead of clicking. */
  tick(SCA_IInputDevice::RETKEY, true);
  tick(SCA_IInputDevice::RETKEY, false);
  tick(SCA_IInputDevice::RETKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragStart, "potion"));
  EXPECT_FALSE(fired(LN_UIElementEvent::Click, "potion"));
  tick(SCA_IInputDevice::RETKEY, false);
  /* Tab to the slot, Enter puts it down there. */
  tick(SCA_IInputDevice::TABKEY, true);
  tick(SCA_IInputDevice::TABKEY, false);
  EXPECT_EQ(service.GetFocus(0, std::string()), &slot.object);
  tick(SCA_IInputDevice::RETKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DropReceived, "slot"));
  EXPECT_FALSE(fired(LN_UIElementEvent::Click, "slot"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

TEST(LN_UIService, ASecondPlayersBackPutsTheirCarriedElementBack)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  /* A draggable UI Element of player 1. */
  LN_UIService::Registration registration = MakeRegistration(
      tree, 1, item.object, camera.object, 0, LN_UIPropagation::Block, 1);
  registration.role = LN_UIInteractableRole::DragSource;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int button) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    LN_GamepadSnapshot gamepad;
    gamepad.connected = true;
    if (button >= 0) {
      gamepad.buttons[button] = 1;
      gamepad.pressed[button] = 1;
    }
    input.SetGamepadForTests(1, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterInteractable(registration);
    service.ResolveInteractions();
  };
  tick(-1);
  ASSERT_EQ(service.SetFocus(1, &item.object, std::string()), LN_UIResult::Success);
  tick(GAME_INPUT_GAMEPAD_Y);
  ASSERT_TRUE(service.GetInteractionState({tree, 1}).drag_started);
  tick(-1);
  EXPECT_EQ(service.CaptureCountForTests(), 1u);
  tick(GAME_INPUT_GAMEPAD_B);
  EXPECT_TRUE(service.GetInteractionState({tree, 1}).drag_cancelled);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

/* Players' gamepads for a tick: gamepad `pressing` presses A (-1: none). */
static void SetPlayerGamepads(LN_InputSnapshot &input, const int count, const int pressing)
{
  for (int index = 0; index < count; ++index) {
    LN_GamepadSnapshot gamepad;
    gamepad.connected = true;
    if (index == pressing) {
      gamepad.buttons[GAME_INPUT_GAMEPAD_A] = 1;
      gamepad.pressed[GAME_INPUT_GAMEPAD_A] = 1;
    }
    input.SetGamepadForTests(index, gamepad);
  }
}

/* Outside menus, a UI Element with Player 1 is that player's: the mouse (player 0's) does not
 * reach it, and player 1's gamepad presses it once Set UI Focus gave them a focus. */
TEST(LN_UIService, AnElementsPlayerUsesItWithTheirGamepadAndTheMouseDoesNot)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  LN_UIService::ElementSettings settings = MakeElementSettings("join");
  settings.player = 1;
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int mouse_button, const int pressing) {
    CaptureInput(device, input, 100, 100, mouse_button);
    SetPlayerGamepads(input, 2, pressing);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&button.object, settings, registration);
    service.ResolveInteractions();
  };
  tick(1, -1);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Press, "join"), nullptr);
  tick(0, -1);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "join"), nullptr);
  EXPECT_EQ(service.SetFocus(0, &button.object, std::string()), LN_UIResult::InvalidTarget);
  ASSERT_EQ(service.SetFocus(1, &button.object, std::string()), LN_UIResult::Success);
  tick(-1, -1);
  const LN_UIService::ElementEvent *focus = service.FindElementEvent(LN_UIElementEvent::Focus,
                                                                     "join");
  ASSERT_NE(focus, nullptr);
  EXPECT_EQ(focus->user, 1);
  /* Gamepad 0 is player 0's: it does not press player 1's button. */
  tick(-1, 0);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "join"), nullptr);
  tick(-1, 1);
  const LN_UIService::ElementEvent *click = service.FindElementEvent(LN_UIElementEvent::Click,
                                                                     "join");
  ASSERT_NE(click, nullptr);
  EXPECT_EQ(click->user, 1);
}

/* Any Player: each player has their own focus on the element and presses it with their own
 * gamepad; On UI Event's User tells who. The hover look shows any player's focus. */
TEST(LN_UIService, AnyPlayerElementsTakeEachPlayersFocusAndPresses)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> card(scene);
  LN_UIService::ElementSettings settings = MakeElementSettings("pick");
  settings.any_player = true;
  settings.hover_scale = 1.5f;
  settings.look_time = 0.0f;
  LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, card.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int mouse_button, const int pressing) {
    CaptureInput(device, input, 100, 100, mouse_button);
    SetPlayerGamepads(input, 3, pressing);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&card.object, settings, registration);
    service.ResolveInteractions();
    service.UpdateElementPresentation(1.0 / 60.0);
  };
  auto event = [&](const LN_UIElementEvent type, const int32_t user) {
    return service.FindElementEvent(type, "pick", user);
  };
  /* The mouse clicks it for player 0. */
  tick(1, -1);
  tick(0, -1);
  ASSERT_NE(event(LN_UIElementEvent::Click, 0), nullptr);
  EXPECT_EQ(event(LN_UIElementEvent::Click, 1), nullptr);
  /* The mouse moves off it. */
  registration.ray_from = MT_Vector3(5.0f, 5.0f, -2.0f);
  registration.ray_to = MT_Vector3(5.0f, 5.0f, 2.0f);
  tick(-1, -1);
  EXPECT_NE(event(LN_UIElementEvent::Exit, 0), nullptr);
  EXPECT_FLOAT_EQ(card.object.NodeGetLocalScaling().x(), 1.0f);

  /* Players 1 and 2 each get a focus on it. */
  ASSERT_EQ(service.SetFocus(1, &card.object, std::string()), LN_UIResult::Success);
  ASSERT_EQ(service.SetFocus(2, &card.object, std::string()), LN_UIResult::Success);
  tick(-1, -1);
  EXPECT_NE(event(LN_UIElementEvent::Focus, 1), nullptr);
  EXPECT_NE(event(LN_UIElementEvent::Focus, 2), nullptr);
  EXPECT_EQ(event(LN_UIElementEvent::Focus, 0), nullptr);

  /* Player 2's A presses it for player 2 only. */
  tick(-1, 2);
  EXPECT_NE(event(LN_UIElementEvent::Click, 2), nullptr);
  EXPECT_EQ(event(LN_UIElementEvent::Click, 1), nullptr);
  EXPECT_EQ(event(LN_UIElementEvent::Click, 0), nullptr);

  /* Player 1 lets go: their focus is lost, player 2 keeps theirs and the hover look. */
  ASSERT_EQ(service.SetFocus(1, nullptr, std::string()), LN_UIResult::Success);
  tick(-1, -1);
  EXPECT_NE(event(LN_UIElementEvent::FocusLost, 1), nullptr);
  EXPECT_EQ(event(LN_UIElementEvent::FocusLost, 2), nullptr);
  EXPECT_EQ(service.GetFocus(2, std::string()), &card.object);
  EXPECT_FLOAT_EQ(card.object.NodeGetLocalScaling().x(), 1.5f);
  ASSERT_EQ(service.SetFocus(2, nullptr, std::string()), LN_UIResult::Success);
  tick(-1, -1);
  EXPECT_NE(event(LN_UIElementEvent::FocusLost, 2), nullptr);
  EXPECT_FLOAT_EQ(card.object.NodeGetLocalScaling().x(), 1.0f);
}

/* Any Player: while player 0's mouse drags an item, player 2's Grab cannot take it too. */
TEST(LN_UIService, AnyPlayerItemHeldByOnePlayerCannotBeGrabbedByAnother)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  LN_UIService::ElementSettings settings = MakeElementSettings("item");
  settings.any_player = true;
  settings.draggable = true;
  settings.follow_cursor = false;
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int mouse_x, const int mouse_button, const bool grab) {
    CaptureInput(device, input, mouse_x, 100, mouse_button);
    for (int pad = 0; pad < 3; ++pad) {
      LN_GamepadSnapshot gamepad;
      gamepad.connected = true;
      if (pad == 2 && grab) {
        gamepad.buttons[GAME_INPUT_GAMEPAD_Y] = 1;
        gamepad.pressed[GAME_INPUT_GAMEPAD_Y] = 1;
      }
      input.SetGamepadForTests(pad, gamepad);
    }
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, settings, registration);
    service.ResolveInteractions();
  };
  tick(100, 1, false);
  tick(130, -1, false);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "item", 0), nullptr);
  ASSERT_EQ(service.SetFocus(2, &item.object, std::string()), LN_UIResult::Success);
  tick(140, -1, true);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::DragStart, "item", 2), nullptr);
  EXPECT_EQ(service.CaptureCountForTests(), 1u);
  /* Let go: player 2 can take it now. */
  tick(140, 0, false);
  tick(140, -1, true);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "item", 2), nullptr);
}

TEST(LN_UIService, ACarriedElementFloatsOverItsHomeWithoutDrifting)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  camera.object.NodeSetWorldPosition(MT_Vector3(0.0f, 0.0f, 10.0f));
  camera.object.NodeUpdateGS(0.0f);
  SceneBoundObject<KX_GameObject> item(scene);
  item.object.NodeSetWorldPosition(MT_Vector3(1.0f, 0.0f, 0.0f));
  item.object.NodeUpdateGS(0.0f);
  LN_UIService::ElementSettings settings = MakeElementSettings("item");
  settings.draggable = true;
  LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::PassThrough);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int key, const bool down) {
    PressKey(device, input, SCA_IInputDevice::SCA_EnumInputs(key), down);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    /* As in the game, the registration follows the object. */
    registration.world_position = item.object.NodeGetWorldPosition();
    service.RegisterElement(&item.object, settings, registration);
    service.ResolveInteractions();
    service.UpdateElementPresentation(1.0 / 60.0);
  };
  tick(SCA_IInputDevice::GKEY, false);
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);
  tick(SCA_IInputDevice::GKEY, true);
  tick(SCA_IInputDevice::GKEY, false);
  tick(SCA_IInputDevice::GKEY, true);
  tick(SCA_IInputDevice::GKEY, false);
  const MT_Vector3 lifted = item.object.NodeGetWorldPosition();
  /* Lifted a little toward the camera and aside... */
  EXPECT_GT(lifted.z(), 0.0f);
  EXPECT_LT((lifted - MT_Vector3(1.0f, 0.0f, 0.0f)).length(), 0.6f);
  for (int frame = 0; frame < 10; ++frame) {
    tick(SCA_IInputDevice::GKEY, false);
  }
  /* ...and it stays there while the focus is on it. */
  EXPECT_LT((item.object.NodeGetWorldPosition() - lifted).length(), 1.0e-5f);
  /* Cancelled, it goes home. */
  tick(SCA_IInputDevice::ESCKEY, true);
  EXPECT_LT((item.object.NodeGetWorldPosition() - MT_Vector3(1.0f, 0.0f, 0.0f)).length(), 1.0e-5f);
}

TEST(LN_UIService, ACarryPutDownOnWhatDoesNotTakeItOrCancelledGoesBack)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  LN_UIService::ElementSettings button_settings = MakeElementSettings("button");
  button_settings.tab_order = 1;
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration button_registration = MakeRegistration(
      nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::PassThrough);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int key, const bool down) {
    PressKey(device, input, SCA_IInputDevice::SCA_EnumInputs(key), down);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&button.object, button_settings, button_registration);
    service.ResolveInteractions();
  };
  auto fired = [&](const LN_UIElementEvent type) {
    return service.FindElementEvent(type, std::string()) != nullptr;
  };
  tick(SCA_IInputDevice::GKEY, false);
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);
  tick(SCA_IInputDevice::GKEY, true);
  tick(SCA_IInputDevice::GKEY, false);
  tick(SCA_IInputDevice::GKEY, true);
  ASSERT_TRUE(fired(LN_UIElementEvent::DragStart));
  tick(SCA_IInputDevice::GKEY, false);

  /* On a button that is no drop target, Grab puts it back: a cancel, no drop, no click. */
  tick(SCA_IInputDevice::TABKEY, true);
  tick(SCA_IInputDevice::TABKEY, false);
  ASSERT_EQ(service.GetFocus(0, std::string()), &button.object);
  tick(SCA_IInputDevice::GKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel));
  EXPECT_FALSE(fired(LN_UIElementEvent::Drop));
  EXPECT_FALSE(fired(LN_UIElementEvent::Click));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  tick(SCA_IInputDevice::GKEY, false);

  /* Back cancels a carry. */
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);
  tick(SCA_IInputDevice::GKEY, true);
  ASSERT_TRUE(fired(LN_UIElementEvent::DragStart));
  tick(SCA_IInputDevice::GKEY, false);
  tick(SCA_IInputDevice::ESCKEY, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  tick(SCA_IInputDevice::ESCKEY, false);

  /* Grab on something that cannot be dragged does nothing. */
  ASSERT_EQ(service.SetFocus(0, &button.object, std::string()), LN_UIResult::Success);
  tick(SCA_IInputDevice::GKEY, true);
  EXPECT_FALSE(fired(LN_UIElementEvent::DragStart));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

TEST(LN_UIService, FollowCursorMovesTheDraggedElementAndReturnsItWhenCancelled)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  item.object.NodeUpdateGS(0.0f);
  LN_UIService::ElementSettings settings = MakeElementSettings("item");
  settings.draggable = true;
  settings.drag_threshold = 1.0f;
  LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::Block);
  registration.bounds_min = MT_Vector3(-2.0f, -2.0f, -0.5f);
  registration.bounds_max = MT_Vector3(2.0f, 2.0f, 0.5f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    registration.world_position = item.object.NodeGetWorldPosition();
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&item.object, settings, registration);
    service.ResolveInteractions();
    service.UpdateElementPresentation(0.016);
  };

  CaptureInput(device, input, 100, 100, 1);
  tick(1);
  registration.ray_from = MT_Vector3(1.0f, 0.0f, -2.0f);
  registration.ray_to = MT_Vector3(1.0f, 0.0f, 2.0f);
  CaptureInput(device, input, 110, 100);
  tick(2);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "item"), nullptr);
  EXPECT_NEAR(item.object.NodeGetWorldPosition().x(), 1.0f, 1.0e-4f);

  device.ClearInputs();
  device.ConvertEvent(SCA_IInputDevice::ESCKEY, 1, 0);
  input.Capture(&device, nullptr);
  tick(3);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::DragCancel, "item"), nullptr);
  EXPECT_NEAR(item.object.NodeGetWorldPosition().x(), 0.0f, 1.0e-4f);
}

TEST(LN_UIService, HoverLookScalesTheElementButAClicksFocusDoesNotKeepIt)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  LN_UIService::ElementSettings settings = MakeElementSettings("button");
  settings.hover_scale = 1.25f;
  settings.press_scale = 0.9f;
  settings.look_time = 0.0f;
  LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&button.object, settings, registration);
    service.ResolveInteractions();
    service.UpdateElementPresentation(0.016);
  };

  CaptureInput(device, input, 100, 100);
  tick(1);
  EXPECT_NEAR(button.object.NodeGetLocalScaling().x(), 1.25f, 1.0e-4f);
  CaptureInput(device, input, 100, 100, 1);
  tick(2);
  EXPECT_NEAR(button.object.NodeGetLocalScaling().x(), 0.9f, 1.0e-4f);
  CaptureInput(device, input, 100, 100, 0);
  tick(3);

  /* The pointer leaves: the click gave focus, but that focus is not shown. */
  registration.ray_from = registration.ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);
  CaptureInput(device, input, 300, 300);
  tick(4);
  EXPECT_EQ(service.GetFocus(0, std::string()), &button.object);
  EXPECT_NEAR(button.object.NodeGetLocalScaling().x(), 1.0f, 1.0e-4f);

  /* Outside menus the keyboard does nothing until Set UI Focus asks for it. */
  PressKey(device, input, SCA_IInputDevice::TABKEY, true);
  tick(5);
  EXPECT_NEAR(button.object.NodeGetLocalScaling().x(), 1.0f, 1.0e-4f);

  /* Then a key shows the focus with the hover look. */
  ASSERT_EQ(service.SetFocus(0, &button.object, std::string()), LN_UIResult::Success);
  PressKey(device, input, SCA_IInputDevice::TABKEY, false);
  tick(6);
  EXPECT_NEAR(button.object.NodeGetLocalScaling().x(), 1.0f, 1.0e-4f);
  PressKey(device, input, SCA_IInputDevice::TABKEY, true);
  tick(7);
  EXPECT_NEAR(button.object.NodeGetLocalScaling().x(), 1.25f, 1.0e-4f);
}

TEST(LN_UIService, EscapeWithoutMenusIsLeftToTheGame)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  DEV_InputDevice device;
  LN_InputSnapshot input;

  PressKey(device, input, SCA_IInputDevice::ESCKEY, true);
  service.BeginTick(LN_Event::OnUpdate, 1, &input);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_FALSE(manager.GetInputConsumption().IsPressConsumed(SCA_IInputDevice::ESCKEY));
  service.ResolveInteractions();
  /* Reading Back does not use it up: Input Action "Back" and the Exit Key both see the press. */
  EXPECT_TRUE(service.GetActionState(
      LN_UIAction::Cancel, LN_UIActionState::Triggered, 0, std::string()));
  EXPECT_TRUE(device.HasUnconsumedPressTransition(SCA_IInputDevice::ESCKEY));
}

TEST(LN_UIService, BackCustomLeavesTheMenuOpenAndThePressToTheGame)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  DEV_InputDevice device;
  LN_InputSnapshot input;
  service.AddMenuForTests(0, LN_UIMenuRouting::Modal, LN_UIMenuBackBehavior::Custom);
  device.ClearInputs();
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnUpdate, 1, &input);
  service.ResolveInteractions();

  PressKey(device, input, SCA_IInputDevice::ESCKEY, true);
  service.BeginTick(LN_Event::OnUpdate, 2, &input);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_FALSE(manager.GetInputConsumption().IsPressConsumed(SCA_IInputDevice::ESCKEY));
  service.ResolveInteractions();
  EXPECT_TRUE(device.HasUnconsumedPressTransition(SCA_IInputDevice::ESCKEY));
  EXPECT_EQ(service.MenuCountForTests(), 1u);
}

/* One frame without input, so the menus know whether they have anything to select. */
static void ResolveWithoutInput(LN_UIService &service,
                                DEV_InputDevice &device,
                                LN_InputSnapshot &input,
                                const uint64_t frame)
{
  device.ClearInputs();
  input.Capture(&device, nullptr);
  service.BeginTick(LN_Event::OnUpdate, frame, &input);
  service.ConsumeInputBeforeResolve(input);
  service.ResolveInteractions();
}

TEST(LN_UIService, AHudOpenedAsAMenuLeavesBackToTheGame)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  DEV_InputDevice device;
  LN_InputSnapshot input;
  /* Split screen gives each player a HUD by opening it as a menu: UI + Game World, Back not Close
   * Menu, nothing to select. */
  service.AddMenuForTests(0, LN_UIMenuRouting::PassThrough, LN_UIMenuBackBehavior::Ignore);
  service.AddMenuForTests(1, LN_UIMenuRouting::PassThrough, LN_UIMenuBackBehavior::Custom);
  ResolveWithoutInput(service, device, input, 1);

  PressKey(device, input, SCA_IInputDevice::ESCKEY, true);
  service.BeginTick(LN_Event::OnUpdate, 2, &input);
  service.ConsumeInputBeforeResolve(input);
  /* Game logic (an Input Action opening the pause menu) sees Esc... */
  EXPECT_FALSE(manager.GetInputConsumption().IsPressConsumed(SCA_IInputDevice::ESCKEY));
  service.ResolveInteractions();
  /* ...and so does the Exit Key, as without the HUD; the HUD stays. */
  EXPECT_TRUE(device.HasUnconsumedPressTransition(SCA_IInputDevice::ESCKEY));
  EXPECT_EQ(service.MenuCountForTests(), 2u);
}

TEST(LN_UIService, AMenuAboveAHudStillTakesBack)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  DEV_InputDevice device;
  LN_InputSnapshot input;
  service.AddMenuForTests(0, LN_UIMenuRouting::PassThrough, LN_UIMenuBackBehavior::Ignore);
  /* An inventory over the HUD, which Back closes. */
  service.AddMenuForTests(0, LN_UIMenuRouting::Modal, LN_UIMenuBackBehavior::CloseMenu);
  ResolveWithoutInput(service, device, input, 1);

  PressKey(device, input, SCA_IInputDevice::ESCKEY, true);
  service.BeginTick(LN_Event::OnUpdate, 2, &input);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_TRUE(manager.GetInputConsumption().IsPressConsumed(SCA_IInputDevice::ESCKEY));
  service.ResolveInteractions();
  EXPECT_FALSE(device.HasUnconsumedPressTransition(SCA_IInputDevice::ESCKEY));
  EXPECT_EQ(service.MenuCountForTests(), 1u);
}

TEST(LN_UIService, AUIAndGameWorldMenuThatClosesOnBackIsNoHud)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  DEV_InputDevice device;
  LN_InputSnapshot input;
  /* A popup that Back closes takes Back, even with nothing to select. */
  service.AddMenuForTests(0, LN_UIMenuRouting::PassThrough, LN_UIMenuBackBehavior::CloseMenu);
  ResolveWithoutInput(service, device, input, 1);

  PressKey(device, input, SCA_IInputDevice::ESCKEY, true);
  service.BeginTick(LN_Event::OnUpdate, 2, &input);
  service.ConsumeInputBeforeResolve(input);
  EXPECT_TRUE(manager.GetInputConsumption().IsPressConsumed(SCA_IInputDevice::ESCKEY));
}

/* On UI Sound hears the UI's sound moments: the highlight moving onto a button (Move), a click
 * (Accept), each with the button, its Action and its player. */
TEST(LN_UIService, UISoundsAreToldWithTheirElementActionAndPlayer)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  const LN_UIService::ElementSettings settings = MakeElementSettings("play");
  const LN_UIService::Registration over = MakeRegistration(
      nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration away = over;
  away.ray_from = MT_Vector3(5.0f, 5.0f, -2.0f);
  away.ray_to = MT_Vector3(5.0f, 5.0f, 2.0f);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 0;
  auto frame = [&](const bool on_button, const int mouse_button) {
    CaptureInput(device, input, on_button ? 100 : 300, 100, mouse_button);
    service.BeginTick(LN_Event::OnUpdate, ++index, &input);
    service.RegisterElement(&button.object, settings, on_button ? over : away);
    service.ResolveInteractions();
    service.UpdateElementPresentation(0.0);
  };
  frame(false, -1);
  frame(false, -1);

  /* The mouse comes over the button. */
  frame(true, -1);
  const LN_UIService::SoundEvent *move = service.FindSoundEvent(-1);
  ASSERT_NE(move, nullptr);
  EXPECT_EQ(move->sound, LN_UISound::Move);
  EXPECT_EQ(manager.ResolveObjectHandle(move->object), &button.object);
  EXPECT_EQ(move->action, "play");
  EXPECT_EQ(move->user, 0);
  EXPECT_EQ(move->menu, "");
  EXPECT_EQ(FrameSound(service), LN_UISound::Move);

  /* A click: Accept, one sound for the player in that frame. */
  frame(true, 1);
  frame(true, 0);
  size_t count = 0;
  const LN_UIService::SoundEvent *accept = service.FindSoundEvent(0, 0, &count);
  ASSERT_NE(accept, nullptr);
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(accept->sound, LN_UISound::Accept);
  EXPECT_EQ(manager.ResolveObjectHandle(accept->object), &button.object);
  EXPECT_EQ(accept->action, "play");
  EXPECT_EQ(FrameSound(service), LN_UISound::Accept);
  /* Player 1 made no sound. */
  EXPECT_EQ(service.FindSoundEvent(1), nullptr);

  /* Nothing happens: no sound, and last frame's is gone. */
  frame(true, -1);
  EXPECT_EQ(service.FindSoundEvent(-1), nullptr);
  EXPECT_EQ(FrameSound(service), LN_UISound::None);
}

/* Back closing a menu is told for its player, before the menu is gone. (A test menu has no
 * collection, so no name: closing a real one needs a scene.) */
TEST(LN_UIService, BackClosingAMenuIsToldForItsPlayer)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  DEV_InputDevice device;
  LN_InputSnapshot input;
  service.AddMenuForTests(0, LN_UIMenuRouting::Modal, LN_UIMenuBackBehavior::CloseMenu);
  ResolveWithoutInput(service, device, input, 1);
  service.UpdateElementPresentation(0.0);

  PressKey(device, input, SCA_IInputDevice::ESCKEY, true);
  service.BeginTick(LN_Event::OnUpdate, 2, &input);
  service.ConsumeInputBeforeResolve(input);
  service.ResolveInteractions();
  service.UpdateElementPresentation(0.0);
  EXPECT_EQ(service.MenuCountForTests(), 0u);
  const LN_UIService::SoundEvent *back = service.FindSoundEvent(0);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->sound, LN_UISound::Back);
  EXPECT_EQ(back->user, 0);
  EXPECT_EQ(back->menu, "");
  EXPECT_FALSE(back->object.IsValid());
  EXPECT_EQ(FrameSound(service), LN_UISound::Back);
}

TEST(LN_UIService, AConsumedPressIsHiddenFromGameLogicButTheKeyStaysHeld)
{
  DEV_InputDevice device;
  LN_InputSnapshot input;
  PressKey(device, input, SCA_IInputDevice::ESCKEY, true);
  ASSERT_TRUE(input.GetStatus(SCA_IInputDevice::ESCKEY, SCA_InputEvent::JUSTACTIVATED));
  input.ConsumePress(SCA_IInputDevice::ESCKEY);
  EXPECT_FALSE(input.GetStatus(SCA_IInputDevice::ESCKEY, SCA_InputEvent::JUSTACTIVATED));
  EXPECT_TRUE(input.GetStatus(SCA_IInputDevice::ESCKEY, SCA_InputEvent::ACTIVE));
  /* Out-of-range codes are ignored. */
  input.ConsumePress(-1);
  input.ConsumePress(1 << 20);
  input.ConsumeGamepadPress(LN_MAX_GAMEPADS, 0);
}

LN_UIService::ViewRegistration MakeGridView(const LN_RuntimeTree *tree,
                                            KX_GameObject &viewport,
                                            KX_Camera &camera,
                                            const std::vector<KX_GameObject *> &slots,
                                            const int item_count,
                                            const int columns)
{
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 60};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport;
  view.camera = &camera;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.slots = slots;
  view.columns = columns;
  for (int index = 0; index < item_count; ++index) {
    LN_Value item;
    item.type = LN_ValueType::Int;
    item.exists = true;
    item.int_value = index;
    view.items.push_back(item);
  }
  return view;
}

TEST(LN_UIService, GridScrollsWholeRowsSoItemsNeverChangeColumn)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slots[4] = {
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene)};
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  /* 7 items in 2 columns shown by 4 slots (two rows). */
  LN_UIService::ViewRegistration view = MakeGridView(
      tree,
      viewport.object,
      camera.object,
      {&slots[0].object, &slots[1].object, &slots[2].object, &slots[3].object},
      7,
      2);
  /* A requested first item inside a row starts at that row. */
  view.requested_first_index = 3;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const int wheel) {
    device.ClearInputs();
    if (wheel != 0) {
      device.ConvertWheelEvent(wheel);
    }
    input.Capture(&device, nullptr);
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    return service.GetViewState(view.listener);
  };
  EXPECT_EQ(tick(1, 0).first_visible_index, 2);
  const LN_UIService::ViewState scrolled = tick(2, -1);
  EXPECT_EQ(scrolled.first_visible_index, 4);
  EXPECT_EQ(scrolled.last_visible_index, 6);
  EXPECT_TRUE(scrolled.at_end);
  /* The last row is partly empty: item 6 keeps its column and slot 3 shows nothing. */
  LN_Value item;
  int32_t item_index = -1;
  ASSERT_TRUE(service.GetBoundItem(&slots[2].object, item, item_index));
  EXPECT_EQ(item_index, 6);
  EXPECT_FALSE(service.GetBoundItem(&slots[3].object, item, item_index));
  EXPECT_FALSE(slots[3].object.GetVisible());
  /* Further wheel steps stay at the last row. */
  EXPECT_EQ(tick(3, -1).first_visible_index, 4);
  EXPECT_EQ(tick(4, 1).first_visible_index, 2);
}

TEST(LN_UIService, GridPagingMovesFocusToTheSlotOfTheNextItem)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slots[4] = {
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene)};
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::ViewRegistration view = MakeGridView(
      tree,
      viewport.object,
      camera.object,
      {&slots[0].object, &slots[1].object, &slots[2].object, &slots[3].object},
      6,
      2);
  std::vector<LN_UIService::Registration> registrations;
  for (int index = 0; index < 4; ++index) {
    LN_UIService::Registration registration = MakeRegistration(
        tree, uint32_t(70 + index), slots[index].object, camera.object, 0,
        LN_UIPropagation::PassThrough);
    registration.tab_order = index;
    registrations.push_back(registration);
  }

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const bool tab_down) {
    PressKey(device, input, SCA_IInputDevice::TABKEY, tab_down);
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    for (const LN_UIService::Registration &registration : registrations) {
      service.RegisterInteractable(registration);
    }
    service.ResolveInteractions();
  };
  tick(1, false);
  ASSERT_EQ(service.SetFocus(0, &slots[3].object, std::string()), LN_UIResult::Success);
  tick(2, true);
  tick(3, false);
  /* Tab from item 3 in the last slot pages one row; item 4 is now in slot 2. */
  tick(4, true);
  EXPECT_EQ(service.GetViewState(view.listener).first_visible_index, 2);
  EXPECT_EQ(service.GetFocus(0, std::string()), &slots[2].object);
  LN_Value item;
  int32_t item_index = -1;
  ASSERT_TRUE(service.GetBoundItem(&slots[2].object, item, item_index));
  EXPECT_EQ(item_index, 4);
}

TEST(LN_UIService, HorizontalGridScrollsWholeColumnsSoItemsNeverChangeRow)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slots[6] = {SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene)};
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  /* 10 items in columns of 3 (6 slots in 2 visible columns), slots listed column by column. A
   * vertical grid with 2 columns would scroll by 2 items instead. */
  LN_UIService::ViewRegistration view = MakeGridView(tree,
                                                     viewport.object,
                                                     camera.object,
                                                     {&slots[0].object,
                                                      &slots[1].object,
                                                      &slots[2].object,
                                                      &slots[3].object,
                                                      &slots[4].object,
                                                      &slots[5].object},
                                                     10,
                                                     2);
  view.horizontal = true;
  view.vertical = false;
  /* A requested first item inside a column starts at that column. */
  view.requested_first_index = 4;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const int wheel) {
    device.ClearInputs();
    if (wheel != 0) {
      device.ConvertWheelEvent(wheel);
    }
    input.Capture(&device, nullptr);
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    return service.GetViewState(view.listener);
  };
  EXPECT_EQ(tick(1, 0).first_visible_index, 3);
  const LN_UIService::ViewState scrolled = tick(2, -1);
  EXPECT_EQ(scrolled.first_visible_index, 6);
  EXPECT_EQ(scrolled.last_visible_index, 9);
  EXPECT_TRUE(scrolled.at_end);
  /* The last column is partly empty: item 9 keeps its row (the top) and slots 4 and 5 show
   * nothing. */
  LN_Value item;
  int32_t item_index = -1;
  ASSERT_TRUE(service.GetBoundItem(&slots[3].object, item, item_index));
  EXPECT_EQ(item_index, 9);
  EXPECT_FALSE(service.GetBoundItem(&slots[4].object, item, item_index));
  EXPECT_FALSE(slots[5].object.GetVisible());
  EXPECT_EQ(tick(3, -1).first_visible_index, 6);
  EXPECT_EQ(tick(4, 1).first_visible_index, 3);
  EXPECT_EQ(tick(5, 1).first_visible_index, 0);
}

TEST(LN_UIService, HorizontalGridPagesWithRightAndLeftAtItsEdgeColumns)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slots[4] = {SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene)};
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  /* 6 items in columns of 2: slots 0-1 are the first visible column, slots 2-3 the second. */
  LN_UIService::ViewRegistration view = MakeGridView(
      tree,
      viewport.object,
      camera.object,
      {&slots[0].object, &slots[1].object, &slots[2].object, &slots[3].object},
      6,
      2);
  view.horizontal = true;
  view.vertical = false;
  std::vector<LN_UIService::Registration> registrations;
  for (int index = 0; index < 4; ++index) {
    registrations.push_back(MakeRegistration(
        tree, uint32_t(80 + index), slots[index].object, camera.object, 0,
        LN_UIPropagation::PassThrough));
  }

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const int key, const bool down) {
    PressKey(device, input, SCA_IInputDevice::SCA_EnumInputs(key), down);
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    for (const LN_UIService::Registration &registration : registrations) {
      service.RegisterInteractable(registration);
    }
    service.ResolveInteractions();
  };
  auto press = [&](uint64_t &index, const int key) {
    tick(index++, key, true);
    tick(index++, key, false);
  };
  uint64_t index = 1;
  tick(index++, SCA_IInputDevice::RIGHTARROWKEY, false);
  /* Item 3, bottom of the last visible column. The first press only shows the selection. */
  ASSERT_EQ(service.SetFocus(0, &slots[3].object, std::string()), LN_UIResult::Success);
  press(index, SCA_IInputDevice::RIGHTARROWKEY);
  EXPECT_EQ(service.GetViewState(view.listener).first_visible_index, 0);
  /* Right in the last column pages one column; the same slot now shows item 5. */
  press(index, SCA_IInputDevice::RIGHTARROWKEY);
  EXPECT_EQ(service.GetViewState(view.listener).first_visible_index, 2);
  EXPECT_EQ(service.GetFocus(0, std::string()), &slots[3].object);
  LN_Value item;
  int32_t item_index = -1;
  ASSERT_TRUE(service.GetBoundItem(&slots[3].object, item, item_index));
  EXPECT_EQ(item_index, 5);
  /* Down does not page a horizontal grid. */
  press(index, SCA_IInputDevice::DOWNARROWKEY);
  EXPECT_EQ(service.GetViewState(view.listener).first_visible_index, 2);
  /* Left in the first column pages back; slot 1 shows item 1. */
  ASSERT_EQ(service.SetFocus(0, &slots[1].object, std::string()), LN_UIResult::Success);
  press(index, SCA_IInputDevice::LEFTARROWKEY);
  EXPECT_EQ(service.GetViewState(view.listener).first_visible_index, 0);
  EXPECT_EQ(service.GetFocus(0, std::string()), &slots[1].object);
  ASSERT_TRUE(service.GetBoundItem(&slots[1].object, item, item_index));
  EXPECT_EQ(item_index, 1);
}

TEST(LN_UIService, ACarriedGridSlotKeepsItsItemWhileTheGridScrolls)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slots[4] = {SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene)};
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::ViewRegistration view = MakeGridView(
      tree,
      viewport.object,
      camera.object,
      {&slots[0].object, &slots[1].object, &slots[2].object, &slots[3].object},
      6,
      2);
  std::vector<LN_UIService::Registration> registrations;
  for (int slot = 0; slot < 4; ++slot) {
    LN_UIService::Registration registration = MakeRegistration(
        tree, uint32_t(90 + slot), slots[slot].object, camera.object, 0,
        LN_UIPropagation::PassThrough);
    registration.role = LN_UIInteractableRole::DragSource;
    registration.drop_target = true;
    registration.tab_order = slot;
    registrations.push_back(registration);
  }

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto resolve = [&]() {
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterView(view);
    service.ResolveViews();
    for (const LN_UIService::Registration &registration : registrations) {
      service.RegisterInteractable(registration);
    }
    service.ResolveInteractions();
  };
  auto tick = [&](const int key, const bool down) {
    PressKey(device, input, SCA_IInputDevice::SCA_EnumInputs(key), down);
    resolve();
  };
  auto bound_index = [&](KX_GameObject &object) {
    LN_Value item;
    int32_t item_index = -1;
    return service.GetBoundItem(&object, item, item_index) ? item_index : -1;
  };
  tick(SCA_IInputDevice::GKEY, false);
  ASSERT_EQ(service.SetFocus(0, &slots[0].object, std::string()), LN_UIResult::Success);
  tick(SCA_IInputDevice::GKEY, true);
  tick(SCA_IInputDevice::GKEY, false);
  tick(SCA_IInputDevice::GKEY, true);
  ASSERT_TRUE(service.GetInteractionState({tree, 90}).drag_started);
  tick(SCA_IInputDevice::GKEY, false);

  /* The wheel scrolls one row: the other slots show items 2-5, the carried one keeps item 0. */
  device.ClearInputs();
  device.ConvertWheelEvent(-1);
  input.Capture(&device, nullptr);
  resolve();
  EXPECT_EQ(service.GetViewState(view.listener).first_visible_index, 2);
  EXPECT_EQ(bound_index(slots[0].object), 0);
  EXPECT_EQ(bound_index(slots[1].object), 3);
  EXPECT_TRUE(service.GetInteractionState({tree, 90}).dragging);

  /* The wheel hid the selection: one Tab shows it, the next moves it; Accept puts the item down
   * on slot 2 (item 4). */
  tick(SCA_IInputDevice::TABKEY, true);
  tick(SCA_IInputDevice::TABKEY, false);
  tick(SCA_IInputDevice::TABKEY, true);
  tick(SCA_IInputDevice::TABKEY, false);
  tick(SCA_IInputDevice::TABKEY, true);
  tick(SCA_IInputDevice::TABKEY, false);
  ASSERT_EQ(service.GetFocus(0, std::string()), &slots[2].object);
  tick(SCA_IInputDevice::RETKEY, true);
  EXPECT_TRUE(service.GetInteractionState({tree, 90}).drop_accepted);
  EXPECT_TRUE(service.GetInteractionState({tree, 92}).dropped);
  EXPECT_EQ(bound_index(slots[0].object), 0);
  EXPECT_EQ(bound_index(slots[2].object), 4);
  tick(SCA_IInputDevice::RETKEY, false);
  /* Put down, it shows its cell's item again. */
  EXPECT_EQ(bound_index(slots[0].object), 2);
}

TEST(LN_UIService, ViewClippingKeepsTheVisibilityGameLogicSets)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> used_slot(scene);
  SceneBoundObject<KX_GameObject> empty_slot(scene);
  SceneBoundObject<KX_GameObject> empty_slot_icon(scene);
  empty_slot.node.AddChild(&empty_slot_icon.node);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::ViewRegistration view = MakeGridView(
      tree, viewport.object, camera.object, {&used_slot.object, &empty_slot.object}, 1, 1);

  auto tick = [&](const uint64_t index, const bool declare) {
    service.BeginTick(LN_Event::OnUpdate, index, nullptr);
    if (declare) {
      service.RegisterView(view);
    }
    service.ResolveViews();
  };
  tick(1, true);
  EXPECT_TRUE(used_slot.object.GetVisible());
  /* An empty slot hides with its icon. */
  EXPECT_FALSE(empty_slot.object.GetVisible());
  EXPECT_FALSE(empty_slot_icon.object.GetVisible());

  /* Game logic hides the used slot: the view does not show it again. */
  used_slot.object.SetVisible(false, false);
  tick(2, true);
  EXPECT_FALSE(used_slot.object.GetVisible());
  used_slot.object.SetVisible(true, false);
  tick(3, true);
  EXPECT_TRUE(used_slot.object.GetVisible());

  /* Releasing the view shows what it clipped, and only that. */
  used_slot.object.SetVisible(false, false);
  tick(4, false);
  EXPECT_TRUE(empty_slot.object.GetVisible());
  EXPECT_TRUE(empty_slot_icon.object.GetVisible());
  EXPECT_FALSE(used_slot.object.GetVisible());
  empty_slot.node.RemoveChild(&empty_slot_icon.node);
}

TEST(LN_UIService, ADraggedSlotStaysVisibleAndPressedOutsideItsView)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::ViewRegistration view = MakeGridView(
      tree, viewport.object, camera.object, {&slot.object}, 1, 1);
  LN_UIService::Registration registration = MakeRegistration(
      tree, 80, slot.object, camera.object, 0, LN_UIPropagation::Block);
  registration.role = LN_UIInteractableRole::DragSource;
  registration.drag_threshold = 1.0f;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    if (service.ObjectPassesViewClipping(&slot.object)) {
      service.RegisterInteractable(registration);
    }
    service.ResolveInteractions();
  };
  CaptureInput(device, input, 100, 100, 1);
  tick(1);
  CaptureInput(device, input, 110, 100);
  tick(2);
  ASSERT_TRUE(service.GetInteractionState({tree, 80}).dragging);

  /* Follow Cursor carried it out of the grid. */
  slot.object.NodeSetWorldPosition(MT_Vector3(5.0f, 0.0f, 0.0f));
  slot.object.NodeUpdateGS(0.0f);
  CaptureInput(device, input, 160, 100);
  tick(3);
  EXPECT_TRUE(slot.object.GetVisible());
  EXPECT_TRUE(service.GetInteractionState({tree, 80}).dragging);
  EXPECT_EQ(service.CaptureCountForTests(), 1u);

  /* Dropped out there: no longer dragged, so the grid clips it again. */
  CaptureInput(device, input, 160, 100, 0);
  tick(4);
  tick(5);
  EXPECT_FALSE(slot.object.GetVisible());
}

TEST(LN_UIService, TheHoverLookKeepsAScaleThatGameLogicSetDuringIt)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  LN_UIService::ElementSettings settings = MakeElementSettings("button");
  settings.hover_scale = 1.5f;
  settings.look_time = 0.0f;
  LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::Block);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&button.object, settings, registration);
    service.ResolveInteractions();
    service.UpdateElementPresentation(0.016);
  };
  CaptureInput(device, input, 100, 100);
  tick(1);
  ASSERT_NEAR(button.object.NodeGetLocalScaling().x(), 1.5f, 1.0e-4f);

  /* A Set Scale while hovered (for example a "selected" size) is kept. */
  button.object.NodeSetLocalScale(MT_Vector3(3.0f, 3.0f, 3.0f));
  tick(2);
  EXPECT_NEAR(button.object.NodeGetLocalScaling().x(), 3.0f, 1.0e-4f);

  /* Leaving returns to that scale without the look, not to the old one. */
  registration.ray_from = registration.ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);
  CaptureInput(device, input, 300, 300);
  tick(3);
  EXPECT_NEAR(button.object.NodeGetLocalScaling().x(), 2.0f, 1.0e-4f);
}

TEST(LN_UIService, AClickOnBlockingUIIsHiddenFromGameLogic)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  /* Each case: what the pointer is over when the left button goes down. */
  struct Case {
    LN_UIPropagation propagation;
    bool over_button;
    bool consumed;
  };
  for (const Case &test_case : {Case{LN_UIPropagation::Block, true, true},
                                Case{LN_UIPropagation::PassThrough, true, false},
                                Case{LN_UIPropagation::Block, false, false}})
  {
    LN_Manager manager(*scene);
    LN_UIService &service = manager.GetUIService();
    LN_UIService::Registration registration = MakeRegistration(
        tree, 90, button.object, camera.object, 0, test_case.propagation);
    if (!test_case.over_button) {
      registration.ray_from = registration.ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);
    }
    DEV_InputDevice device;
    LN_InputSnapshot input;
    CaptureInput(device, input, 10, 10, 1);
    service.BeginTick(LN_Event::OnUpdate, 1, &input);
    service.RegisterInteractable(registration);
    service.ResolveInteractions();
    EXPECT_EQ(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::LEFTMOUSE),
              test_case.consumed);
  }
}

/** The pointer at (x, y) with the middle, right and side buttons going down and the wheel
 * turning. */
void CapturePointerButtonsAndWheel(DEV_InputDevice &device,
                                   LN_InputSnapshot &input,
                                   const int x,
                                   const int y)
{
  device.ClearInputs();
  device.ConvertMoveEvent(x, y);
  for (const SCA_IInputDevice::SCA_EnumInputs code : {SCA_IInputDevice::MIDDLEMOUSE,
                                                      SCA_IInputDevice::RIGHTMOUSE,
                                                      SCA_IInputDevice::BUTTON4MOUSE})
  {
    device.ConvertEvent(code, 1, 0);
  }
  device.ConvertWheelEvent(-1);
  input.Capture(&device, nullptr);
}

TEST(LN_UIService, BlockingUITakesEveryMouseButtonAndTheWheelFromGameLogic)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> panel(scene);

  /* Each case: the UI Element the pointer is over (or not). A disabled one with Block still keeps
   * the pointer from the game, as it keeps clicks. */
  struct Case {
    LN_UIPropagation pointer;
    bool interactable;
    bool over_panel;
    bool consumed;
  };
  for (const Case &test_case : {Case{LN_UIPropagation::Block, true, true, true},
                                Case{LN_UIPropagation::Block, false, true, true},
                                Case{LN_UIPropagation::PassThrough, true, true, false},
                                Case{LN_UIPropagation::Ignore, true, true, false},
                                Case{LN_UIPropagation::Block, true, false, false}})
  {
    LN_Manager manager(*scene);
    LN_UIService &service = manager.GetUIService();
    LN_UIService::Registration registration = MakeRegistration(
        nullptr, 0, panel.object, camera.object, 0, test_case.pointer);
    registration.enabled = test_case.interactable;
    if (!test_case.over_panel) {
      registration.ray_from = registration.ray_to = MT_Vector3(9.0f, 9.0f, 9.0f);
    }
    LN_UIService::ElementSettings settings = MakeElementSettings("");
    settings.pointer = test_case.pointer;
    DEV_InputDevice device;
    LN_InputSnapshot input;
    CapturePointerButtonsAndWheel(device, input, 10, 10);
    service.BeginTick(LN_Event::OnUpdate, 1, &input);
    service.RegisterElement(&panel.object, settings, registration);
    service.ResolveInteractions();
    const LN_InputConsumption &consumed = manager.GetInputConsumption();
    for (const SCA_IInputDevice::SCA_EnumInputs code : {SCA_IInputDevice::MIDDLEMOUSE,
                                                        SCA_IInputDevice::RIGHTMOUSE,
                                                        SCA_IInputDevice::BUTTON4MOUSE})
    {
      EXPECT_EQ(consumed.IsStrokeConsumed(code), test_case.consumed) << int(code);
    }
    EXPECT_EQ(consumed.IsWheelConsumed(), test_case.consumed);
  }
}

/** The UI is under the pointer left of x 50: the camera's pointer ray there runs through the
 * origin, elsewhere it misses everything. */
bool TestPointerRay(KX_Camera * /*camera*/,
                    const float x,
                    const float /*y*/,
                    MT_Vector3 &r_from,
                    MT_Vector3 &r_to)
{
  if (x < 50.0f) {
    r_from = MT_Vector3(0.0f, 0.0f, -2.0f);
    r_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  }
  else {
    r_from = r_to = MT_Vector3(9.0f, 9.0f, 9.0f);
  }
  return true;
}

/** Input that reaches a Logic + Physics step before the next frame: the pointer was at `frame_x`
 * in the last frame and is at `sample_x` now. */
struct PointerMoveCase {
  int frame_x;
  int sample_x;
  bool over_ui_now;
};
constexpr PointerMoveCase kPointerMoveCases[] = {
    /* Resting on the UI. */
    {10, 11, true},
    /* Moving over it while scrolling. */
    {10, 40, true},
    /* Moving onto it from the game. */
    {80, 20, true},
    /* Moving off it onto the game. */
    {10, 80, false},
};

TEST(LN_UIService, PointerInputBeforeTheNextFrameIsTheUIsWhereThePointerIsNow)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> panel(scene);

  /* A Logic + Physics step can come before the next frame resolves the UI: input that reaches it
   * first is judged against the UI as the last frame laid it out, where its pointer is now. */
  for (const PointerMoveCase &test_case : kPointerMoveCases) {
    LN_Manager manager(*scene);
    LN_UIService &service = manager.GetUIService();
    service.SetPointerRayForTests(TestPointerRay);
    LN_UIService::Registration registration = MakeRegistration(
        nullptr, 0, panel.object, camera.object, 0, LN_UIPropagation::Block);
    TestPointerRay(&camera.object,
                   float(test_case.frame_x),
                   10.0f,
                   registration.ray_from,
                   registration.ray_to);
    LN_UIService::ElementSettings settings = MakeElementSettings("");
    settings.pointer = LN_UIPropagation::Block;
    DEV_InputDevice device;
    LN_InputSnapshot input;
    CaptureInput(device, input, test_case.frame_x, 10);
    service.BeginTick(LN_Event::OnUpdate, 1, &input);
    service.RegisterElement(&panel.object, settings, registration);
    service.ResolveInteractions();
    ASSERT_FALSE(manager.GetInputConsumption().IsWheelConsumed());

    LN_InputSnapshot sample;
    CapturePointerButtonsAndWheel(device, sample, test_case.sample_x, 10);
    service.ConsumeInputBeforeResolve(sample);
    const LN_InputConsumption &consumed = manager.GetInputConsumption();
    const std::string label = std::to_string(test_case.frame_x) + " -> " +
                              std::to_string(test_case.sample_x);
    EXPECT_EQ(consumed.IsStrokeConsumed(SCA_IInputDevice::MIDDLEMOUSE), test_case.over_ui_now)
        << label;
    EXPECT_EQ(consumed.IsStrokeConsumed(SCA_IInputDevice::BUTTON4MOUSE), test_case.over_ui_now)
        << label;
    EXPECT_EQ(consumed.IsWheelConsumed(), test_case.over_ui_now) << label;
  }

  /* A panel hidden since the last frame takes nothing. */
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  service.SetPointerRayForTests(TestPointerRay);
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, panel.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::ElementSettings settings = MakeElementSettings("");
  settings.pointer = LN_UIPropagation::Block;
  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 10, 10);
  service.BeginTick(LN_Event::OnUpdate, 1, &input);
  service.RegisterElement(&panel.object, settings, registration);
  service.ResolveInteractions();
  panel.object.SetVisible(false, false);
  LN_InputSnapshot sample;
  CapturePointerButtonsAndWheel(device, sample, 10, 10);
  service.ConsumeInputBeforeResolve(sample);
  EXPECT_FALSE(manager.GetInputConsumption().IsWheelConsumed());
  panel.object.SetVisible(true, false);
}

TEST(LN_UIService, PointerInputBeforeTheNextFrameHitsTheUIWhereItIsNow)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> panel(scene);
  LN_UIService::ElementSettings settings = MakeElementSettings("");
  settings.pointer = LN_UIPropagation::Block;

  /* A menu that slid since the last frame (or a button on a moving object) is hit where it is
   * now, where the player sees it: moved away from the pointer, or under it. */
  for (const bool moves_away : {true, false}) {
    LN_Manager manager(*scene);
    LN_UIService &service = manager.GetUIService();
    service.SetPointerRayForTests(TestPointerRay);
    const MT_Vector3 before = moves_away ? MT_Vector3(0.0f, 0.0f, 0.0f) :
                                           MT_Vector3(5.0f, 0.0f, 0.0f);
    const MT_Vector3 after = moves_away ? MT_Vector3(5.0f, 0.0f, 0.0f) :
                                          MT_Vector3(0.0f, 0.0f, 0.0f);
    panel.object.NodeSetWorldPosition(before);
    panel.object.NodeUpdateGS(0.0);
    LN_UIService::Registration registration = MakeRegistration(
        nullptr, 0, panel.object, camera.object, 0, LN_UIPropagation::Block);
    registration.world_position = before;
    DEV_InputDevice device;
    LN_InputSnapshot input;
    CaptureInput(device, input, 10, 10);
    service.BeginTick(LN_Event::OnUpdate, 1, &input);
    service.RegisterElement(&panel.object, settings, registration);
    service.ResolveInteractions();

    panel.object.NodeSetWorldPosition(after);
    panel.object.NodeUpdateGS(0.0);
    LN_InputSnapshot sample;
    CapturePointerButtonsAndWheel(device, sample, 10, 10);
    service.ConsumeInputBeforeResolve(sample);
    EXPECT_EQ(manager.GetInputConsumption().IsWheelConsumed(), !moves_away);
  }
  panel.object.NodeSetWorldPosition(MT_Vector3(0.0f, 0.0f, 0.0f));
  panel.object.NodeUpdateGS(0.0);

  /* A hovered button grown by its hover look is still hit at its normal size, as in the frame. */
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  /* Left of x 50 the ray runs through the button's middle, right of it 1 unit to its side: inside
   * the grown button, outside the normal one. */
  service.SetPointerRayForTests(
      [](KX_Camera * /*camera*/, const float x, const float /*y*/, MT_Vector3 &r_from,
         MT_Vector3 &r_to) {
        const float side = x < 50.0f ? 0.0f : 1.0f;
        r_from = MT_Vector3(side, 0.0f, -2.0f);
        r_to = MT_Vector3(side, 0.0f, 2.0f);
        return true;
      });
  LN_UIService::ElementSettings button = MakeElementSettings("button");
  button.pointer = LN_UIPropagation::Block;
  button.hover_scale = 3.0f;
  button.look_time = 0.0f;
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, panel.object, camera.object, 0, LN_UIPropagation::Block);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 10, 10);
  service.BeginTick(LN_Event::OnUpdate, 1, &input);
  service.RegisterElement(&panel.object, button, registration);
  service.ResolveInteractions();
  service.UpdateElementPresentation(0.016);
  ASSERT_NEAR(panel.object.NodeGetWorldScaling().x(), 3.0f, 1.0e-4f);
  LN_InputSnapshot sample;
  CapturePointerButtonsAndWheel(device, sample, 70, 10);
  service.ConsumeInputBeforeResolve(sample);
  EXPECT_FALSE(manager.GetInputConsumption().IsWheelConsumed());
  CapturePointerButtonsAndWheel(device, sample, 10, 10);
  service.ConsumeInputBeforeResolve(sample);
  EXPECT_TRUE(manager.GetInputConsumption().IsWheelConsumed());
  panel.object.NodeSetLocalScale(MT_Vector3(1.0f, 1.0f, 1.0f));
  panel.object.NodeUpdateGS(0.0);
}

TEST(LN_UIService, TheWheelBeforeTheNextFrameScrollsTheViewUnderThePointerNotTheGame)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));

  /* A grid with no UI Element that blocks over it: the view itself takes the wheel. */
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 40};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.slots = {&slot.object};
  for (int index = 0; index < 4; ++index) {
    LN_Value item;
    item.type = LN_ValueType::Int;
    item.exists = true;
    item.int_value = index;
    view.items.push_back(item);
  }
  for (const PointerMoveCase &test_case : kPointerMoveCases) {
    LN_Manager manager(*scene);
    LN_UIService &service = manager.GetUIService();
    service.SetPointerRayForTests(TestPointerRay);
    LN_UIService::ViewRegistration registered = view;
    TestPointerRay(&camera.object,
                   float(test_case.frame_x),
                   10.0f,
                   registered.ray_from,
                   registered.ray_to);
    DEV_InputDevice device;
    LN_InputSnapshot input;
    CaptureInput(device, input, test_case.frame_x, 10);
    service.BeginTick(LN_Event::OnUpdate, 1, &input);
    service.RegisterView(registered);
    service.ResolveViews();
    ASSERT_FALSE(manager.GetInputConsumption().IsWheelConsumed());

    LN_InputSnapshot sample;
    CapturePointerButtonsAndWheel(device, sample, test_case.sample_x, 10);
    service.ConsumeInputBeforeResolve(sample);
    const std::string label = std::to_string(test_case.frame_x) + " -> " +
                              std::to_string(test_case.sample_x);
    EXPECT_EQ(manager.GetInputConsumption().IsWheelConsumed(), test_case.over_ui_now) << label;
    /* A view takes only the wheel. */
    EXPECT_FALSE(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::MIDDLEMOUSE))
        << label;
  }
}

TEST(LN_UIService, KeysAMenuUsesAreHiddenFromGameLogicButGameplayKeysAreNot)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration first_registration = MakeRegistration(
      tree, 1, first.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration second_registration = MakeRegistration(
      tree, 2, second.object, camera.object, 0, LN_UIPropagation::Block);
  first_registration.tab_order = 1;
  second_registration.tab_order = 2;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterInteractable(first_registration);
    service.RegisterInteractable(second_registration);
    service.ResolveInteractions();
  };
  const LN_InputConsumption &consumed = manager.GetInputConsumption();

  /* No menu and no Set UI Focus: Space and Tab are the game's. */
  PressKey(device, input, SCA_IInputDevice::SPACEKEY, true);
  tick(1);
  EXPECT_FALSE(consumed.IsPressConsumed(SCA_IInputDevice::SPACEKEY));
  PressKey(device, input, SCA_IInputDevice::SPACEKEY, false);
  tick(2);

  /* Set UI Focus gives the keyboard to this UI: the press that shows the selection and the one
   * that moves it are the UI's. */
  ASSERT_EQ(service.SetFocus(0, &first.object, std::string()), LN_UIResult::Success);
  PressKey(device, input, SCA_IInputDevice::TABKEY, true);
  tick(3);
  EXPECT_TRUE(consumed.IsPressConsumed(SCA_IInputDevice::TABKEY));
  PressKey(device, input, SCA_IInputDevice::TABKEY, false);
  tick(4);
  PressKey(device, input, SCA_IInputDevice::RETKEY, true);
  tick(5);
  EXPECT_TRUE(consumed.IsPressConsumed(SCA_IInputDevice::RETKEY));
  EXPECT_TRUE(service.GetInteractionState({tree, 1}).clicked);
}

LN_GamepadSnapshot MakeStickGamepad(const int32_t left_x, const int32_t left_y)
{
  LN_GamepadSnapshot gamepad;
  gamepad.connected = true;
  gamepad.axes[0] = left_x;
  gamepad.axes[1] = left_y;
  return gamepad;
}

TEST(LN_UIService, TheLeftStickNavigatesAndIgnoresDrift)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> top(scene);
  SceneBoundObject<KX_GameObject> bottom(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration top_registration = MakeRegistration(
      tree, 1, top.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration bottom_registration = MakeRegistration(
      tree, 2, bottom.object, camera.object, 0, LN_UIPropagation::Block);
  top_registration.screen_position = MT_Vector3(0.5f, 0.2f, 0.0f);
  bottom_registration.screen_position = MT_Vector3(0.5f, 0.8f, 0.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const int32_t left_y) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    input.SetGamepadForTests(0, MakeStickGamepad(0, left_y));
    service.BeginTick(LN_Event::OnUpdate, index, &input, 1.0 / 60.0);
    service.RegisterInteractable(top_registration);
    service.RegisterInteractable(bottom_registration);
    service.ResolveInteractions();
  };
  tick(1, 0);
  ASSERT_EQ(service.SetFocus(0, &top.object, std::string()), LN_UIResult::Success);

  /* A drifting stick does nothing. */
  tick(2, 9000);
  EXPECT_FALSE(service.GetInteractionState({tree, 1}).focus_visible);

  /* Down shows the selection, back to the centre, down again moves it. */
  tick(3, 30000);
  EXPECT_EQ(service.GetFocus(0, std::string()), &top.object);
  EXPECT_TRUE(service.GetInteractionState({tree, 1}).focus_visible);
  EXPECT_TRUE(service.GetActionState(
      LN_UIAction::NavigateDown, LN_UIActionState::Active, 0, std::string()));
  tick(4, 0);
  tick(5, 30000);
  EXPECT_EQ(service.GetFocus(0, std::string()), &bottom.object);
  /* Up the same way, with the stick going straight from down to up. */
  tick(6, -30000);
  EXPECT_EQ(service.GetFocus(0, std::string()), &top.object);
}

TEST(LN_UIService, AnElementsOwnNeighbourWinsOverTheNearestOne)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> left(scene);
  SceneBoundObject<KX_GameObject> middle(scene);
  SceneBoundObject<KX_GameObject> right(scene);
  left.object.SetName("Left");
  middle.object.SetName("Middle");
  right.object.SetName("Right");
  /* The authored object the neighbour setting points at: found by name, so copies work. */
  blender::Object right_source = {};
  std::snprintf(right_source.id.name, sizeof(right_source.id.name), "OBRight");
  LN_UIService::Registration left_registration = MakeRegistration(
      nullptr, 0, left.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration middle_registration = MakeRegistration(
      nullptr, 0, middle.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration right_registration = MakeRegistration(
      nullptr, 0, right.object, camera.object, 0, LN_UIPropagation::Block);
  left_registration.world_position = MT_Vector3(-10.0f, 0.0f, 0.0f);
  middle_registration.world_position = MT_Vector3(0.0f, 0.0f, 0.0f);
  right_registration.world_position = MT_Vector3(10.0f, 0.0f, 0.0f);
  left_registration.screen_position = MT_Vector3(0.2f, 0.5f, 0.0f);
  middle_registration.screen_position = MT_Vector3(0.5f, 0.5f, 0.0f);
  right_registration.screen_position = MT_Vector3(0.8f, 0.5f, 0.0f);
  LN_UIService::ElementSettings left_settings = MakeElementSettings("left");
  left_settings.navigation[3] = &right_source;
  const LN_UIService::ElementSettings middle_settings = MakeElementSettings("middle");
  const LN_UIService::ElementSettings right_settings = MakeElementSettings("right");
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 0;
  auto tick = [&]() {
    service.BeginTick(LN_Event::OnUpdate, ++index, &input);
    service.RegisterElement(&left.object, left_settings, left_registration);
    service.RegisterElement(&middle.object, middle_settings, middle_registration);
    service.RegisterElement(&right.object, right_settings, right_registration);
    service.ResolveInteractions();
  };
  auto press = [&](const SCA_IInputDevice::SCA_EnumInputs key) {
    PressKey(device, input, key, true);
    tick();
    PressKey(device, input, key, false);
    tick();
  };
  CaptureInput(device, input, 100, 100);
  tick();
  ASSERT_EQ(service.SetFocus(0, &left.object, std::string()), LN_UIResult::Success);
  press(SCA_IInputDevice::RIGHTARROWKEY); /* The first press only shows the focus. */
  press(SCA_IInputDevice::RIGHTARROWKEY);
  /* Right from Left goes to its neighbour, past the nearer Middle. */
  EXPECT_EQ(service.GetFocus(0, std::string()), &right.object);
  /* Without a neighbour the nearest one that way. */
  press(SCA_IInputDevice::LEFTARROWKEY);
  EXPECT_EQ(service.GetFocus(0, std::string()), &middle.object);
}

TEST(LN_UIService, HoldingADirectionRepeatsAfterAShortDelay)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> items[4] = {SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene),
                                              SceneBoundObject<KX_GameObject>(scene)};
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  std::vector<LN_UIService::Registration> registrations;
  for (int index = 0; index < 4; ++index) {
    LN_UIService::Registration registration = MakeRegistration(
        tree, uint32_t(index + 1), items[index].object, camera.object, 0,
        LN_UIPropagation::Block);
    registration.screen_position = MT_Vector3(0.5f, 0.1f + 0.2f * float(index), 0.0f);
    registrations.push_back(registration);
  }

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const double delta_time) {
    service.BeginTick(LN_Event::OnUpdate, index, &input, delta_time);
    for (const LN_UIService::Registration &registration : registrations) {
      service.RegisterInteractable(registration);
    }
    service.ResolveInteractions();
  };
  auto focused_index = [&]() {
    for (int index = 0; index < 4; ++index) {
      if (service.GetFocus(0, std::string()) == &items[index].object) {
        return index;
      }
    }
    return -1;
  };
  tick(1, 0.1);
  ASSERT_EQ(service.SetFocus(0, &items[0].object, std::string()), LN_UIResult::Success);
  /* Show the selection, then press and hold Down. */
  PressKey(device, input, SCA_IInputDevice::DOWNARROWKEY, true);
  tick(2, 0.1);
  PressKey(device, input, SCA_IInputDevice::DOWNARROWKEY, false);
  tick(3, 0.1);
  PressKey(device, input, SCA_IInputDevice::DOWNARROWKEY, true);
  tick(4, 0.1);
  ASSERT_EQ(focused_index(), 1);
  /* Held: nothing more for 0.4 s, then a step every 0.1 s. */
  uint64_t tick_index = 5;
  auto hold = [&](const double delta_time) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    tick(tick_index++, delta_time);
  };
  hold(0.1);
  hold(0.1);
  hold(0.1);
  EXPECT_EQ(focused_index(), 1);
  hold(0.1);
  EXPECT_EQ(focused_index(), 2);
  hold(0.1);
  EXPECT_EQ(focused_index(), 3);
  /* Releasing stops it. */
  PressKey(device, input, SCA_IInputDevice::DOWNARROWKEY, false);
  tick(tick_index++, 0.5);
  EXPECT_EQ(focused_index(), 3);
}

}  // namespace

TEST(LN_UIService, GridSlotsInFrontOfAFlatViewportCanBeDragged)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> panel(scene);
  SceneBoundObject<KX_GameObject> first_slot(scene);
  SceneBoundObject<KX_GameObject> second_slot(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  /* A menu as usually built: a flat panel plane, the slots a little in front of it. */
  first_slot.object.NodeSetLocalPosition(MT_Vector3(0.0f, 0.0f, 0.05f));
  second_slot.object.NodeSetLocalPosition(MT_Vector3(1.5f, 0.0f, 0.05f));
  for (SG_Node *node : {&panel.node, &first_slot.node, &second_slot.node}) {
    node->UpdateWorldData(0.0);
  }

  LN_UIService::ViewRegistration view;
  view.listener = {tree, 80};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &panel.object;
  view.camera = &camera.object;
  view.bounds_min = MT_Vector3(-3.0f, -3.0f, 0.0f);
  view.bounds_max = MT_Vector3(3.0f, 3.0f, 0.0f);
  view.slots = {&first_slot.object, &second_slot.object};
  view.columns = 2;
  view.cull_outside = false;
  for (const char *name : {"Apple", "Gem"}) {
    LN_Value item;
    item.type = LN_ValueType::String;
    item.exists = true;
    item.string_value = name;
    view.items.push_back(item);
  }

  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.draggable = true;
  LN_UIService::ElementSettings panel_settings = MakeElementSettings("");
  panel_settings.focus = LN_UIElementFocus::None;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto element = [&](KX_GameObject &object, const MT_Vector3 &pointer, const float half_size) {
    LN_UIService::Registration registration;
    registration.object = &object;
    registration.camera = &camera.object;
    registration.ray_from = pointer + MT_Vector3(0.0f, 0.0f, 5.0f);
    registration.ray_to = pointer - MT_Vector3(0.0f, 0.0f, 5.0f);
    registration.world_position = object.NodeGetWorldPosition();
    registration.bounds_min = MT_Vector3(-half_size, -half_size, 0.0f);
    registration.bounds_max = MT_Vector3(half_size, half_size, 0.0f);
    registration.interaction_domain = LN_InteractionDomain::Overlay;
    return registration;
  };
  /* The engine's frame order: views, then UI Elements that pass the view's clipping, then the
   * pointer, then the drag look. */
  auto tick = [&](const uint64_t index, const int x, const MT_Vector3 &pointer, const int button) {
    CaptureInput(device, input, x, 100, button);
    view.ray_from = pointer + MT_Vector3(0.0f, 0.0f, 5.0f);
    view.ray_to = pointer - MT_Vector3(0.0f, 0.0f, 5.0f);
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterView(view);
    service.ResolveViews();
    service.RegisterElement(&panel.object, panel_settings, element(panel.object, pointer, 3.0f));
    for (KX_GameObject *slot : {&first_slot.object, &second_slot.object}) {
      if (slot->GetVisible() && service.ObjectPassesViewClipping(slot)) {
        service.RegisterElement(slot, slot_settings, element(*slot, pointer, 0.5f));
      }
    }
    service.ResolveInteractions();
    service.UpdateElementPresentation(1.0 / 60.0);
  };

  tick(1, 100, MT_Vector3(0.0f, 0.0f, 0.0f), -1);
  EXPECT_TRUE(service.ObjectPassesViewClipping(&first_slot.object));
  EXPECT_TRUE(service.ObjectPassesViewClipping(&second_slot.object));
  tick(2, 100, MT_Vector3(0.0f, 0.0f, 0.0f), 1);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Press, "slot"), nullptr);
  tick(3, 110, MT_Vector3(0.5f, 0.0f, 0.0f), -1);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "slot"), nullptr);
  EXPECT_NEAR(first_slot.object.NodeGetWorldPosition().x(), 0.5f, 0.0001f);
  /* The dragged slot keeps following the pointer outside the panel. */
  tick(4, 160, MT_Vector3(5.0f, 0.0f, 0.0f), -1);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Drag, "slot"), nullptr);
  EXPECT_NEAR(first_slot.object.NodeGetWorldPosition().x(), 5.0f, 0.0001f);
  /* Beyond the panel's face, an undragged slot is clipped. */
  second_slot.object.NodeSetLocalPosition(MT_Vector3(4.0f, 0.0f, 0.05f));
  second_slot.node.UpdateWorldData(0.0);
  EXPECT_FALSE(service.ObjectPassesViewClipping(&second_slot.object));
}

/* The menus read their controls from the game's Input Map. */

/** Gives the engine's default Input Map back when a test that changed it ends. */
struct DefaultInputMapGuard {
  ~DefaultInputMapGuard()
  {
    LN_InputMap::Get().Load(nullptr);
  }
};

static LN_InputBinding MakeKeyBinding(const int16_t event,
                                      const SCA_IInputDevice::SCA_EnumInputs input)
{
  LN_InputBinding binding;
  binding.device = event == blender::WHEELUPMOUSE || event == blender::WHEELDOWNMOUSE ?
                       LN_InputDevice::Mouse :
                       LN_InputDevice::Keyboard;
  binding.code = event;
  binding.input = int16_t(input);
  return binding;
}

TEST(LN_UIService, MenusUseTheKeysOfTheInputMap)
{
  DefaultInputMapGuard guard;
  LN_InputMap &map = LN_InputMap::Get();
  /* Accept on K instead of Return (Space stays). */
  map.Rebind(map.FindBuiltin(LN_UIAction::Accept),
             MakeKeyBinding(blender::EVT_KKEY, SCA_IInputDevice::KKEY));

  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> object(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  const LN_UIService::Registration registration = MakeRegistration(
      tree, 12, object.object, camera.object, 0, LN_UIPropagation::Block);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterInteractable(registration);
    service.ResolveInteractions();
  };
  tick(1);
  ASSERT_EQ(service.SetFocus(0, &object.object, std::string()), LN_UIResult::Success);
  /* Shows the focus. */
  PressKey(device, input, SCA_IInputDevice::KKEY, true);
  tick(2);
  PressKey(device, input, SCA_IInputDevice::KKEY, false);
  tick(3);
  PressKey(device, input, SCA_IInputDevice::RETKEY, true);
  tick(4);
  EXPECT_FALSE(service.GetInteractionState({tree, 12}).clicked);
  EXPECT_FALSE(service.GetActionState(
      LN_UIAction::Accept, LN_UIActionState::Triggered, 0, std::string()));
  PressKey(device, input, SCA_IInputDevice::RETKEY, false);
  tick(5);
  PressKey(device, input, SCA_IInputDevice::KKEY, true);
  tick(6);
  EXPECT_TRUE(service.GetInteractionState({tree, 12}).clicked);
}

TEST(LN_UIService, SecondaryClicksTheFocusedElementOrTheOneUnderThePointer)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::Block);
  const LN_UIService::ElementSettings settings = MakeElementSettings("slot");
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&slot.object, settings, registration);
    service.ResolveInteractions();
  };
  auto gamepad_x = [&](const bool down) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    LN_GamepadSnapshot gamepad;
    gamepad.connected = true;
    gamepad.buttons[GAME_INPUT_GAMEPAD_X] = down ? 1 : 0;
    gamepad.pressed[GAME_INPUT_GAMEPAD_X] = down ? 1 : 0;
    gamepad.released[GAME_INPUT_GAMEPAD_X] = down ? 0 : 1;
    input.SetGamepadForTests(0, gamepad);
  };
  tick(1);
  ASSERT_EQ(service.SetFocus(0, &slot.object, std::string()), LN_UIResult::Success);
  /* The first press only shows the focus. */
  gamepad_x(true);
  tick(2);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::SecondaryClick, "slot"), nullptr);
  gamepad_x(false);
  tick(3);
  gamepad_x(true);
  tick(4);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::SecondaryClick, "slot"), nullptr);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "slot"), nullptr);
  gamepad_x(false);
  tick(5);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::SecondaryClick, "slot"), nullptr);

  /* A right click on it. */
  device.ClearInputs();
  device.ConvertMoveEvent(100, 100);
  device.ConvertEvent(SCA_IInputDevice::RIGHTMOUSE, 1, 0);
  input.Capture(&device, nullptr);
  input.SetGamepadForTests(0, LN_GamepadSnapshot());
  tick(6);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::SecondaryClick, "slot"), nullptr);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Press, "slot"), nullptr);
  /* The UI took the right click: game logic does not see it. */
  EXPECT_TRUE(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::RIGHTMOUSE));
}

TEST(LN_UIService, TheRightStickScrollsAGridAndTheWheelCanBeInverted)
{
  DefaultInputMapGuard guard;
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slots[4] = {
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene),
      SceneBoundObject<KX_GameObject>(scene)};
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  /* 8 items in 2 columns shown by 4 slots (two rows). */
  const LN_UIService::ViewRegistration view = MakeGridView(
      tree,
      viewport.object,
      camera.object,
      {&slots[0].object, &slots[1].object, &slots[2].object, &slots[3].object},
      8,
      2);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const int wheel, const float right_up, const double dt) {
    device.ClearInputs();
    if (wheel != 0) {
      device.ConvertWheelEvent(wheel);
    }
    input.Capture(&device, nullptr);
    LN_GamepadSnapshot gamepad;
    gamepad.connected = true;
    gamepad.axes[3] = int32_t(-right_up * 32767.0f);
    input.SetGamepadForTests(0, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index, &input, dt);
    service.RegisterView(view);
    for (int slot = 0; slot < 4; ++slot) {
      service.RegisterInteractable(MakeRegistration(
          tree, uint32_t(70 + slot), slots[slot].object, camera.object, 0,
          LN_UIPropagation::Block));
    }
    service.ResolveViews();
    service.ResolveInteractions();
    return service.GetViewState(view.listener).first_visible_index;
  };
  EXPECT_EQ(tick(1, 0, 0.0f, 0.1), 0);
  /* A HUD grid: with no menu and no focus the right stick turns the camera, not the grid. */
  EXPECT_EQ(tick(2, 0, -0.9f, 0.1), 0);
  EXPECT_EQ(tick(3, 0, 0.0f, 0.1), 0);
  ASSERT_EQ(service.SetFocus(0, &slots[0].object, std::string()), LN_UIResult::Success);
  /* Down on the right stick scrolls one row down; held, it waits for the repeat delay. */
  EXPECT_EQ(tick(2, 0, -0.9f, 0.1), 2);
  EXPECT_EQ(tick(3, 0, -0.9f, 0.1), 2);
  EXPECT_EQ(tick(4, 0, -0.9f, 0.3), 4);
  EXPECT_EQ(tick(5, 0, 0.0f, 0.1), 4);
  EXPECT_EQ(tick(6, 0, 0.9f, 0.1), 2);
  EXPECT_EQ(tick(7, 0, 0.0f, 0.1), 2);

  /* Scroll Up on the wheel's down turn and Scroll Down on its up turn: inverted scrolling. */
  LN_InputMap &map = LN_InputMap::Get();
  map.Rebind(map.FindBuiltin(LN_UIAction::ScrollUp),
             MakeKeyBinding(blender::WHEELDOWNMOUSE, SCA_IInputDevice::WHEELDOWNMOUSE));
  map.Rebind(map.FindBuiltin(LN_UIAction::ScrollDown),
             MakeKeyBinding(blender::WHEELUPMOUSE, SCA_IInputDevice::WHEELUPMOUSE));
  EXPECT_EQ(tick(8, 1, 0.0f, 0.1), 4);
  EXPECT_EQ(tick(9, -1, 0.0f, 0.1), 2);
}

TEST(LN_UIService, AStickHeldWhileTheUIHadNoInputWaitsForTheCentre)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> top(scene);
  SceneBoundObject<KX_GameObject> bottom(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::Registration top_registration = MakeRegistration(
      tree, 1, top.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration bottom_registration = MakeRegistration(
      tree, 2, bottom.object, camera.object, 0, LN_UIPropagation::Block);
  top_registration.screen_position = MT_Vector3(0.5f, 0.2f, 0.0f);
  bottom_registration.screen_position = MT_Vector3(0.5f, 0.8f, 0.0f);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const int32_t left_y, const bool blocked) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    input.SetGamepadForTests(0, MakeStickGamepad(0, left_y));
    service.BeginTick(LN_Event::OnUpdate, index, blocked ? nullptr : &input, 1.0 / 60.0);
    service.RegisterInteractable(top_registration);
    service.RegisterInteractable(bottom_registration);
    service.ResolveInteractions();
  };
  tick(1, 0, false);
  ASSERT_EQ(service.SetFocus(0, &top.object, std::string()), LN_UIResult::Success);
  /* Show the focus with a first push, then centre the stick. */
  tick(2, 30000, false);
  tick(3, 0, false);
  ASSERT_EQ(service.GetFocus(0, std::string()), &top.object);
  /* A rebind takes the push (the UI has no input), the stick is still held afterwards. */
  tick(4, 30000, true);
  tick(5, 30000, false);
  tick(6, 30000, false);
  EXPECT_EQ(service.GetFocus(0, std::string()), &top.object);
  /* Back to the centre, a new push navigates again. */
  tick(7, 0, false);
  tick(8, 30000, false);
  EXPECT_EQ(service.GetFocus(0, std::string()), &bottom.object);
}

/* The Highlight event: what a player points at, for every device. */

TEST(LN_UIService, HighlightFollowsThePointerAndClearsWhenItLeaves)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  SceneBoundObject<KX_GameObject> recipe(scene);
  LN_UIService::Registration first_registration = MakeRegistration(
      nullptr, 0, first.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration second_registration = MakeRegistration(
      nullptr, 0, second.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration recipe_registration = MakeRegistration(
      nullptr, 0, recipe.object, camera.object, 0, LN_UIPropagation::Block);
  const LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  const LN_UIService::ElementSettings recipe_settings = MakeElementSettings("recipe");
  /* The pointer ray goes through the origin: whichever element sits there is pointed at. */
  auto place = [&](const float first_x, const float second_x, const float recipe_x) {
    first_registration.world_position = MT_Vector3(first_x, 0.0f, 0.0f);
    second_registration.world_position = MT_Vector3(second_x, 0.0f, 0.0f);
    recipe_registration.world_position = MT_Vector3(recipe_x, 0.0f, 0.0f);
  };
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    CaptureInput(device, input, 100, 100);
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&first.object, slot_settings, first_registration);
    service.RegisterElement(&second.object, slot_settings, second_registration);
    service.RegisterElement(&recipe.object, recipe_settings, recipe_registration);
    service.ResolveInteractions();
  };

  place(0.0f, 4.0f, 8.0f);
  tick(1);
  const LN_UIService::ElementEvent *highlight = service.FindElementEvent(
      LN_UIElementEvent::Highlight, "slot");
  ASSERT_NE(highlight, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(highlight->object), &first.object);
  EXPECT_EQ(highlight->user, 0);
  EXPECT_EQ(highlight->index, -1);
  EXPECT_FALSE(highlight->item.exists);

  /* Still pointing at it: no new event. */
  tick(2);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Highlight, std::string()), nullptr);

  /* A focus the mouse player has not seen (set by logic) leaves the highlight to the pointer. */
  ASSERT_EQ(service.SetFocus(0, &recipe.object, std::string()), LN_UIResult::Success);
  tick(3);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Highlight, std::string()), nullptr);

  /* To another slot: only the new one is named (a node sees one event of a type a frame). */
  place(20.0f, 0.0f, 8.0f);
  tick(4);
  highlight = service.FindElementEvent(LN_UIElementEvent::Highlight, "slot");
  ASSERT_NE(highlight, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(highlight->object), &second.object);

  /* To a recipe: the slots hear that the highlight left them, then the recipes get it the next
   * frame, so a text line both write shows the recipe whatever the node order. */
  place(20.0f, 24.0f, 0.0f);
  tick(5);
  highlight = service.FindElementEvent(LN_UIElementEvent::Highlight, "slot");
  ASSERT_NE(highlight, nullptr);
  EXPECT_FALSE(highlight->object.IsValid());
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Highlight, "recipe"), nullptr);
  tick(6);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Highlight, "slot"), nullptr);
  highlight = service.FindElementEvent(LN_UIElementEvent::Highlight, "recipe");
  ASSERT_NE(highlight, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(highlight->object), &recipe.object);
  highlight = service.FindElementEvent(LN_UIElementEvent::Highlight, std::string());
  ASSERT_NE(highlight, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(highlight->object), &recipe.object);
  tick(7);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Highlight, std::string()), nullptr);

  /* Off every element: the recipes hear it left. */
  place(20.0f, 24.0f, 28.0f);
  tick(8);
  highlight = service.FindElementEvent(LN_UIElementEvent::Highlight, "recipe");
  ASSERT_NE(highlight, nullptr);
  EXPECT_FALSE(highlight->object.IsValid());
}

TEST(LN_UIService, HighlightFollowsTheFocusAKeyboardPlayerSees)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  /* The pointer misses both: only the focus can highlight them. */
  LN_UIService::Registration first_registration = MakeRegistration(
      nullptr, 0, first.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration second_registration = MakeRegistration(
      nullptr, 0, second.object, camera.object, 0, LN_UIPropagation::Block);
  first_registration.world_position = MT_Vector3(10.0f, 0.0f, 0.0f);
  second_registration.world_position = MT_Vector3(14.0f, 0.0f, 0.0f);
  first_registration.tab_order = 0;
  second_registration.tab_order = 1;
  LN_UIService::ElementSettings first_settings = MakeElementSettings("button");
  LN_UIService::ElementSettings second_settings = MakeElementSettings("button");
  first_settings.tab_order = 0;
  second_settings.tab_order = 1;
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&first.object, first_settings, first_registration);
    service.RegisterElement(&second.object, second_settings, second_registration);
    service.ResolveInteractions();
  };
  CaptureInput(device, input, 100, 100);
  tick(1);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Highlight, std::string()), nullptr);
  ASSERT_EQ(service.SetFocus(0, &first.object, std::string()), LN_UIResult::Success);
  /* The first Tab only shows the focus: the highlight is the first button. */
  PressKey(device, input, SCA_IInputDevice::TABKEY, true);
  tick(2);
  const LN_UIService::ElementEvent *highlight = service.FindElementEvent(
      LN_UIElementEvent::Highlight, "button");
  ASSERT_NE(highlight, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(highlight->object), &first.object);
  PressKey(device, input, SCA_IInputDevice::TABKEY, false);
  tick(3);
  /* The next Tab moves it, and the highlight with it. */
  PressKey(device, input, SCA_IInputDevice::TABKEY, true);
  tick(4);
  highlight = service.FindElementEvent(LN_UIElementEvent::Highlight, "button");
  ASSERT_NE(highlight, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(highlight->object), &second.object);
}

/* Widgets change a game property by themselves and report it with Changed. */

TEST(LN_UIService, AToggleWidgetFlipsItsPropertyOnEachClick)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> toggle(scene);
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, toggle.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::ElementSettings settings = MakeElementSettings("music");
  settings.widget = LN_UIWidget::Toggle;
  settings.widget_property = "music_on";
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index, const int button) {
    CaptureInput(device, input, 100, 100, button);
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&toggle.object, settings, registration);
    service.ResolveInteractions();
  };
  tick(1, 1);
  EXPECT_EQ(toggle.object.GetProperty("music_on"), nullptr);
  tick(2, 0);
  EXP_Value *value = toggle.object.GetProperty("music_on");
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->GetNumber(), 1.0);
  const LN_UIService::ElementEvent *changed = service.FindElementEvent(LN_UIElementEvent::Changed,
                                                                       "music");
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(changed->item.type, LN_ValueType::Bool);
  EXPECT_TRUE(changed->item.bool_value);

  tick(3, 1);
  tick(4, 0);
  value = toggle.object.GetProperty("music_on");
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->GetNumber(), 0.0);
  changed = service.FindElementEvent(LN_UIElementEvent::Changed, "music");
  ASSERT_NE(changed, nullptr);
  EXPECT_FALSE(changed->item.bool_value);
}

TEST(LN_UIService, ATextFieldTakesTypedTextUntilBack)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> field(scene);
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, field.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::ElementSettings settings = MakeElementSettings("name");
  settings.widget = LN_UIWidget::Text;
  settings.widget_property = "player_name";
  settings.widget_max = 5.0f;
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 0;
  auto tick = [&]() {
    service.BeginTick(LN_Event::OnUpdate, ++index, &input);
    service.RegisterElement(&field.object, settings, registration);
    service.ResolveInteractions();
  };
  auto type = [&](const SCA_IInputDevice::SCA_EnumInputs key, const unsigned int character) {
    device.ClearInputs();
    device.ConvertMoveEvent(100, 100);
    device.ConvertEvent(key, 1, character);
    input.Capture(&device, nullptr);
    tick();
    device.ClearInputs();
    device.ConvertMoveEvent(100, 100);
    device.ConvertEvent(key, 0, 0);
    input.Capture(&device, nullptr);
    tick();
  };
  auto text = [&]() {
    EXP_Value *value = field.object.GetProperty("player_name");
    return value != nullptr ? value->GetText() : std::string("<none>");
  };
  /* A click starts typing. */
  CaptureInput(device, input, 100, 100, 1);
  tick();
  CaptureInput(device, input, 100, 100, 0);
  tick();
  type(SCA_IInputDevice::AKEY, 'A');
  type(SCA_IInputDevice::BKEY, 'b');
  EXPECT_EQ(text(), "Ab");
  type(SCA_IInputDevice::BACKSPACEKEY, 0);
  EXPECT_EQ(text(), "A");
  /* At most Max Length characters (5), also UTF-8 ones. */
  for (const unsigned int character : {0xE9u, 0x63u, 0x64u, 0x65u, 0x66u} /* é c d e f */) {
    type(SCA_IInputDevice::CKEY, character);
  }
  EXPECT_EQ(text(), "A\xC3\xA9" "cde");
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Changed, "name"), nullptr);
  /* Back finishes typing: the text is told once. */
  PressKey(device, input, SCA_IInputDevice::ESCKEY, true);
  tick();
  const LN_UIService::ElementEvent *changed = service.FindElementEvent(LN_UIElementEvent::Changed,
                                                                       "name");
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(changed->item.type, LN_ValueType::String);
  EXPECT_EQ(changed->item.string_value, "A\xC3\xA9" "cde");
  /* No longer typing: keys type nothing. */
  PressKey(device, input, SCA_IInputDevice::ESCKEY, false);
  tick();
  type(SCA_IInputDevice::XKEY, 'x');
  EXPECT_EQ(text(), "A\xC3\xA9" "cde");
}

/* Keys sampled for the game's fixed steps before a UI frame (ConsumeInputBeforeResolve) are the
 * text's too while typing: a letter is not also a game action. */
TEST(LN_UIService, TypedKeysNeverReachTheGameFirst)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> field(scene);
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, field.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::ElementSettings settings = MakeElementSettings("name");
  settings.widget = LN_UIWidget::Text;
  settings.widget_property = "player_name";
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 0;
  auto tick = [&]() {
    service.BeginTick(LN_Event::OnUpdate, ++index, &input);
    service.RegisterElement(&field.object, settings, registration);
    service.ResolveInteractions();
  };
  auto key = [&](const SCA_IInputDevice::SCA_EnumInputs code,
                 const bool down,
                 const unsigned int character) {
    device.ClearInputs();
    device.ConvertMoveEvent(100, 100);
    device.ConvertEvent(code, down ? 1 : 0, down ? character : 0);
    input.Capture(&device, nullptr);
  };
  const LN_InputConsumption &consumed = manager.GetInputConsumption();
  /* A click starts typing. */
  CaptureInput(device, input, 100, 100, 1);
  tick();
  CaptureInput(device, input, 100, 100, 0);
  tick();
  key(SCA_IInputDevice::MKEY, true, 'm');
  service.ConsumeInputBeforeResolve(input);
  EXPECT_TRUE(consumed.IsStrokeConsumed(SCA_IInputDevice::MKEY));
  tick();
  key(SCA_IInputDevice::MKEY, false, 0);
  tick();
  EXP_Value *name = field.object.GetProperty("player_name");
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(name->GetText(), "m");
  /* Enter finishes typing: the next key is the game's again. */
  key(SCA_IInputDevice::RETKEY, true, 0);
  tick();
  key(SCA_IInputDevice::RETKEY, false, 0);
  tick();
  key(SCA_IInputDevice::NKEY, true, 'n');
  service.ConsumeInputBeforeResolve(input);
  EXPECT_FALSE(consumed.IsStrokeConsumed(SCA_IInputDevice::NKEY));
}

TEST(LN_UIService, ASliderWidgetStepsWithLeftAndRightWhileFocused)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> slider(scene);
  SceneBoundObject<KX_GameObject> next(scene);
  LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, slider.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration next_registration = MakeRegistration(
      nullptr, 0, next.object, camera.object, 0, LN_UIPropagation::Block);
  registration.world_position = MT_Vector3(10.0f, 0.0f, 0.0f);
  next_registration.world_position = MT_Vector3(14.0f, 0.0f, 0.0f);
  /* Arrow keys navigate on screen: "next" lies to the right of the slider. */
  registration.screen_position = MT_Vector3(0.3f, 0.5f, 0.0f);
  next_registration.screen_position = MT_Vector3(0.7f, 0.5f, 0.0f);
  LN_UIService::ElementSettings settings = MakeElementSettings("volume");
  settings.widget = LN_UIWidget::Slider;
  settings.widget_property = "volume";
  settings.widget_min = 0.0f;
  settings.widget_max = 1.0f;
  settings.widget_step = 0.25f;
  const LN_UIService::ElementSettings next_settings = MakeElementSettings("next");
  slider.object.SetProperty("volume", new EXP_FloatValue(0.5f));
  DEV_InputDevice device;
  LN_InputSnapshot input;
  auto tick = [&](const uint64_t index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&slider.object, settings, registration);
    service.RegisterElement(&next.object, next_settings, next_registration);
    service.ResolveInteractions();
  };
  auto press = [&](const SCA_IInputDevice::SCA_EnumInputs key, const uint64_t index) {
    PressKey(device, input, key, true);
    tick(index);
    PressKey(device, input, key, false);
    tick(index + 1);
  };
  CaptureInput(device, input, 100, 100);
  tick(1);
  ASSERT_EQ(service.SetFocus(0, &slider.object, std::string()), LN_UIResult::Success);
  /* The first press only shows the focus. */
  press(SCA_IInputDevice::RIGHTARROWKEY, 2);
  EXPECT_FLOAT_EQ(float(slider.object.GetProperty("volume")->GetNumber()), 0.5f);
  press(SCA_IInputDevice::RIGHTARROWKEY, 4);
  EXPECT_FLOAT_EQ(float(slider.object.GetProperty("volume")->GetNumber()), 0.75f);
  /* Left and Right never move the focus away from a slider. */
  EXPECT_EQ(service.GetFocus(0, std::string()), &slider.object);
  press(SCA_IInputDevice::RIGHTARROWKEY, 6);
  press(SCA_IInputDevice::RIGHTARROWKEY, 8);
  EXPECT_FLOAT_EQ(float(slider.object.GetProperty("volume")->GetNumber()), 1.0f);
  press(SCA_IInputDevice::LEFTARROWKEY, 10);
  EXPECT_FLOAT_EQ(float(slider.object.GetProperty("volume")->GetNumber()), 0.75f);
}

TEST(LN_UIService, ArrowKeysMeasureFromEdgesAndKeepToTheirDirection)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  /* An identity camera: world (x, y) shows at ((x + 1) / 2, (1 - y) / 2) on screen. */
  MT_Matrix4x4 identity;
  identity.setIdentity();
  camera.object.SetProjectionMatrix(identity);
  struct Element {
    const char *action;
    MT_Vector3 position;
    MT_Vector3 scale;
  };
  /* A wide row, a checkbox under its left end, a second row, and a button far up-right. */
  const MT_Vector3 wide(0.8f, 0.1f, 1.0f);
  const MT_Vector3 small(0.1f, 0.1f, 1.0f);
  const std::array<Element, 4> layout = {{{"row", MT_Vector3(0.0f, 0.6f, 0.0f), wide},
                                          {"box", MT_Vector3(-0.35f, 0.3f, 0.0f), small},
                                          {"below", MT_Vector3(0.0f, -0.1f, 0.0f), wide},
                                          {"corner", MT_Vector3(0.9f, 0.95f, 0.0f), small}}};
  SceneBoundObject<KX_GameObject> row(scene);
  SceneBoundObject<KX_GameObject> box(scene);
  SceneBoundObject<KX_GameObject> below(scene);
  SceneBoundObject<KX_GameObject> corner(scene);
  const std::array<KX_GameObject *, 4> objects = {
      &row.object, &box.object, &below.object, &corner.object};
  std::array<LN_UIService::Registration, 4> registrations;
  std::array<LN_UIService::ElementSettings, 4> settings;
  for (size_t index = 0; index < layout.size(); ++index) {
    registrations[index] = MakeRegistration(
        nullptr, 0, *objects[index], camera.object, 0, LN_UIPropagation::Block);
    registrations[index].world_position = layout[index].position;
    registrations[index].world_scale = layout[index].scale;
    registrations[index].screen_position = MT_Vector3((layout[index].position.x() + 1.0f) * 0.5f,
                                                      (1.0f - layout[index].position.y()) * 0.5f,
                                                      0.0f);
    settings[index] = MakeElementSettings(layout[index].action);
  }
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t frame = 0;
  auto tick = [&]() {
    service.BeginTick(LN_Event::OnUpdate, ++frame, &input);
    for (size_t index = 0; index < layout.size(); ++index) {
      service.RegisterElement(objects[index], settings[index], registrations[index]);
    }
    service.ResolveInteractions();
  };
  auto press = [&](const SCA_IInputDevice::SCA_EnumInputs key) {
    PressKey(device, input, key, true);
    tick();
    PressKey(device, input, key, false);
    tick();
  };
  CaptureInput(device, input, 100, 100);
  tick();
  ASSERT_EQ(service.SetFocus(0, objects[0], std::string()), LN_UIResult::Success);
  press(SCA_IInputDevice::DOWNARROWKEY); /* The first press only shows the focus. */
  /* The checkbox lies within the row's width: straight below, and nearer than the next row. */
  press(SCA_IInputDevice::DOWNARROWKEY);
  EXPECT_EQ(service.GetFocus(0, std::string()), objects[1]);
  press(SCA_IInputDevice::DOWNARROWKEY);
  EXPECT_EQ(service.GetFocus(0, std::string()), objects[2]);
  /* The far button is much more up than right: Right has nowhere to go. */
  press(SCA_IInputDevice::RIGHTARROWKEY);
  EXPECT_EQ(service.GetFocus(0, std::string()), objects[2]);
}

/* A row of cells under a row of tabs that sit closer: Left and Right stay in the row they are in,
 * Up goes to the tab over the cell. */
TEST(LN_UIService, ArrowKeysStayInTheRowTheyAreIn)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  MT_Matrix4x4 identity;
  identity.setIdentity();
  camera.object.SetProjectionMatrix(identity);
  const MT_Vector3 cell(0.12f, 0.12f, 1.0f);
  const MT_Vector3 tab(0.07f, 0.05f, 1.0f);
  const std::array<std::pair<MT_Vector3, MT_Vector3>, 5> layout = {
      {{MT_Vector3(-0.6f, 0.0f, 0.0f), cell},
       {MT_Vector3(0.0f, 0.0f, 0.0f), cell},
       {MT_Vector3(0.6f, 0.0f, 0.0f), cell},
       {MT_Vector3(-0.4f, 0.2f, 0.0f), tab},
       {MT_Vector3(0.2f, 0.2f, 0.0f), tab}}};
  std::deque<SceneBoundObject<KX_GameObject>> objects;
  std::vector<LN_UIService::Registration> registrations;
  for (const auto &[position, scale] : layout) {
    KX_GameObject &object = objects.emplace_back(scene).object;
    LN_UIService::Registration registration = MakeRegistration(
        nullptr, 0, object, camera.object, 0, LN_UIPropagation::Block);
    registration.world_position = position;
    registration.world_scale = scale;
    registration.screen_position = MT_Vector3(
        (position.x() + 1.0f) * 0.5f, (1.0f - position.y()) * 0.5f, 0.0f);
    registrations.push_back(registration);
  }
  const LN_UIService::ElementSettings settings = MakeElementSettings("ui");
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t frame = 0;
  const auto tick = [&]() {
    service.BeginTick(LN_Event::OnUpdate, ++frame, &input);
    for (size_t index = 0; index < registrations.size(); ++index) {
      service.RegisterElement(registrations[index].object, settings, registrations[index]);
    }
    service.ResolveInteractions();
  };
  const auto press = [&](const SCA_IInputDevice::SCA_EnumInputs key) {
    PressKey(device, input, key, true);
    tick();
    PressKey(device, input, key, false);
    tick();
    return service.GetFocus(0, std::string());
  };
  CaptureInput(device, input, 100, 100);
  tick();
  ASSERT_EQ(service.SetFocus(0, &objects[0].object, std::string()), LN_UIResult::Success);
  press(SCA_IInputDevice::RIGHTARROWKEY); /* The first press only shows the focus. */
  /* The first tab is nearer to the right, but above the row. */
  EXPECT_EQ(press(SCA_IInputDevice::RIGHTARROWKEY), &objects[1].object);
  EXPECT_EQ(press(SCA_IInputDevice::RIGHTARROWKEY), &objects[2].object);
  EXPECT_EQ(press(SCA_IInputDevice::LEFTARROWKEY), &objects[1].object);
  /* Nothing straight above the middle cell: the nearest tab up there. */
  EXPECT_EQ(press(SCA_IInputDevice::UPARROWKEY), &objects[4].object);
  /* Along the tabs, then down into the cell under the first one. */
  EXPECT_EQ(press(SCA_IInputDevice::LEFTARROWKEY), &objects[3].object);
  EXPECT_EQ(press(SCA_IInputDevice::DOWNARROWKEY), &objects[0].object);
}

TEST(LN_UIService, AnUntouchedSliderLeavesItsPropertyAlone)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> slider(scene);
  SceneBoundObject<KX_GameObject> count(scene);
  LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, slider.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::Registration count_registration = MakeRegistration(
      nullptr, 0, count.object, camera.object, 0, LN_UIPropagation::Block);
  registration.world_position = MT_Vector3(10.0f, 0.0f, 0.0f);
  count_registration.world_position = MT_Vector3(14.0f, 0.0f, 0.0f);
  /* 0.73 is no multiple of the step 0.1, and 12 lies outside 0..10. */
  LN_UIService::ElementSettings settings = MakeElementSettings("volume");
  settings.widget = LN_UIWidget::Slider;
  settings.widget_property = "volume";
  settings.widget_min = 0.0f;
  settings.widget_max = 1.0f;
  settings.widget_step = 0.1f;
  LN_UIService::ElementSettings count_settings = MakeElementSettings("count");
  count_settings.widget = LN_UIWidget::Slider;
  count_settings.widget_property = "count";
  count_settings.widget_min = 0.0f;
  count_settings.widget_max = 10.0f;
  count_settings.widget_step = 0.1f;
  slider.object.SetProperty("volume", new EXP_FloatValue(0.73f));
  count.object.SetProperty("count", new EXP_IntValue(12));
  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100);
  for (uint64_t index = 1; index <= 3; ++index) {
    service.BeginTick(LN_Event::OnUpdate, index, &input);
    service.RegisterElement(&slider.object, settings, registration);
    service.RegisterElement(&count.object, count_settings, count_registration);
    service.ResolveInteractions();
    EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Changed, std::string()), nullptr);
  }
  EXPECT_FLOAT_EQ(float(slider.object.GetProperty("volume")->GetNumber()), 0.73f);
  EXPECT_EQ(count.object.GetProperty("count")->GetNumber(), 12.0);

  /* An Int property steps by whole numbers, whatever the Step: 12 is clamped to 10, then 9. */
  ASSERT_EQ(service.SetFocus(0, &count.object, std::string()), LN_UIResult::Success);
  uint64_t index = 4;
  auto press = [&](const SCA_IInputDevice::SCA_EnumInputs key) {
    for (const bool down : {true, false}) {
      PressKey(device, input, key, down);
      service.BeginTick(LN_Event::OnUpdate, index++, &input);
      service.RegisterElement(&slider.object, settings, registration);
      service.RegisterElement(&count.object, count_settings, count_registration);
      service.ResolveInteractions();
    }
  };
  press(SCA_IInputDevice::LEFTARROWKEY); /* The first press only shows the focus. */
  press(SCA_IInputDevice::LEFTARROWKEY);
  EXPECT_EQ(count.object.GetProperty("count")->GetNumber(), 10.0);
  press(SCA_IInputDevice::LEFTARROWKEY);
  EXPECT_EQ(count.object.GetProperty("count")->GetNumber(), 9.0);
}

TEST(LN_UIService, ASelectorWidgetCyclesANumberThroughItsRange)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> selector(scene);
  const LN_UIService::Registration registration = MakeRegistration(
      nullptr, 0, selector.object, camera.object, 0, LN_UIPropagation::Block);
  LN_UIService::ElementSettings settings = MakeElementSettings("difficulty");
  settings.widget = LN_UIWidget::Selector;
  settings.widget_property = "difficulty";
  settings.widget_min = 1.0f;
  settings.widget_max = 3.0f;
  settings.widget_step = 1.0f;
  selector.object.SetProperty("difficulty", new EXP_IntValue(2));
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 0;
  auto click = [&]() {
    for (const int button : {1, 0}) {
      CaptureInput(device, input, 100, 100, button);
      service.BeginTick(LN_Event::OnUpdate, ++index, &input);
      service.RegisterElement(&selector.object, settings, registration);
      service.ResolveInteractions();
    }
  };
  click();
  EXPECT_EQ(selector.object.GetProperty("difficulty")->GetNumber(), 3.0);
  const LN_UIService::ElementEvent *changed = service.FindElementEvent(LN_UIElementEvent::Changed,
                                                                       "difficulty");
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(changed->item.type, LN_ValueType::Int);
  EXPECT_EQ(changed->item.int_value, 3);
  click();
  EXPECT_EQ(selector.object.GetProperty("difficulty")->GetNumber(), 1.0);
}

/* A slot whose Accept Categories refuse the dragged item still hears it come and go (Drop Enter
 * and Drop Exit, not accepted), and letting go there puts the item back. A slot that takes it
 * gets the drop, and its Drop Exit comes in the same frame. */
TEST(LN_UIService, AcceptCategoriesRefuseOtherItemsAndDropEnterAndExitTellIt)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  EXP_StringValue *category = new EXP_StringValue("Potion", "category");
  item.object.SetProperty("category", category);
  category->Release();
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.drag_threshold = 1.0f;
  item_settings.priority = 10;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  slot_settings.accept_categories = ",weapon,shield,";
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::Block);
  const LN_UIService::Registration slot_hit = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::Block);
  const LN_UIService::ListenerKey slot_listener{nullptr, LN_INVALID_INDEX, &slot.object};
  LN_UIService::Registration slot_missed = slot_hit;
  slot_missed.ray_from = MT_Vector3(5.0f, 5.0f, -2.0f);
  slot_missed.ray_to = MT_Vector3(5.0f, 5.0f, 2.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int x, const int button, const bool over_slot) {
    CaptureInput(device, input, x, 100, button);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, over_slot ? slot_hit : slot_missed);
    service.ResolveInteractions();
    service.UpdateElementPresentation(0.0);
  };
  auto find = [&](const LN_UIElementEvent type, const char *action) {
    return service.FindElementEvent(type, action);
  };

  tick(100, 1, true);
  tick(103, -1, true);
  ASSERT_NE(find(LN_UIElementEvent::DragStart, "potion"), nullptr);
  const LN_UIService::ElementEvent *enter = find(LN_UIElementEvent::DropEnter, "slot");
  ASSERT_NE(enter, nullptr);
  EXPECT_FALSE(enter->accepted);
  EXPECT_EQ(manager.ResolveObjectHandle(enter->object), &slot.object);
  EXPECT_EQ(manager.ResolveObjectHandle(enter->target), &item.object);
  EXPECT_EQ(enter->target_action, "potion");
  EXPECT_TRUE(service.GetInteractionState(slot_listener).drop_refused);
  /* Still over it: no new Drop Enter. */
  tick(104, -1, true);
  EXPECT_EQ(find(LN_UIElementEvent::DropEnter, "slot"), nullptr);
  EXPECT_EQ(find(LN_UIElementEvent::DropExit, "slot"), nullptr);
  /* Off it: Drop Exit, still not accepted. */
  tick(105, -1, false);
  const LN_UIService::ElementEvent *exit = find(LN_UIElementEvent::DropExit, "slot");
  ASSERT_NE(exit, nullptr);
  EXPECT_FALSE(exit->accepted);
  EXPECT_EQ(manager.ResolveObjectHandle(exit->target), &item.object);
  tick(104, -1, true);
  ASSERT_NE(find(LN_UIElementEvent::DropEnter, "slot"), nullptr);
  /* Let go over it: it goes back with the Denied sound, nothing is dropped, and the slot hears
   * the drag leave. */
  tick(104, 0, true);
  EXPECT_NE(find(LN_UIElementEvent::DragCancel, "potion"), nullptr);
  EXPECT_EQ(find(LN_UIElementEvent::Drop, "potion"), nullptr);
  EXPECT_EQ(find(LN_UIElementEvent::DropOutside, "potion"), nullptr);
  EXPECT_EQ(find(LN_UIElementEvent::DropReceived, "slot"), nullptr);
  EXPECT_NE(find(LN_UIElementEvent::DropExit, "slot"), nullptr);
  EXPECT_EQ(FrameSound(service), LN_UIService::UISound::Denied);
  /* On UI Sound hears it for the dragged element. */
  const LN_UIService::SoundEvent *denied = service.FindSoundEvent(0);
  ASSERT_NE(denied, nullptr);
  EXPECT_EQ(denied->sound, LN_UISound::Denied);
  EXPECT_EQ(manager.ResolveObjectHandle(denied->object), &item.object);
  EXPECT_EQ(denied->action, "potion");
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  /* An item may have several categories ("Potion, Consumable"): one taken is enough. A slot that
   * takes it gets Drop Enter accepted, then the drop with its Drop Exit. */
  category = new EXP_StringValue("Potion, Consumable", "category");
  item.object.SetProperty("category", category);
  category->Release();
  slot_settings.accept_categories = ",weapon,consumable,";
  tick(100, 1, true);
  tick(103, -1, true);
  enter = find(LN_UIElementEvent::DropEnter, "slot");
  ASSERT_NE(enter, nullptr);
  EXPECT_TRUE(enter->accepted);
  EXPECT_FALSE(service.GetInteractionState(slot_listener).drop_refused);
  tick(103, 0, true);
  const LN_UIService::ElementEvent *drop = find(LN_UIElementEvent::Drop, "potion");
  ASSERT_NE(drop, nullptr);
  EXPECT_TRUE(drop->accepted);
  EXPECT_EQ(manager.ResolveObjectHandle(drop->target), &slot.object);
  const LN_UIService::ElementEvent *received = find(LN_UIElementEvent::DropReceived, "slot");
  ASSERT_NE(received, nullptr);
  EXPECT_TRUE(received->accepted);
  exit = find(LN_UIElementEvent::DropExit, "slot");
  ASSERT_NE(exit, nullptr);
  EXPECT_TRUE(exit->accepted);
  tick(103, -1, true);
  EXPECT_EQ(find(LN_UIElementEvent::DropExit, "slot"), nullptr);

  /* An item with no category is refused by a slot that names categories. */
  item.object.RemoveProperty("category");
  tick(100, 1, true);
  tick(103, -1, true);
  enter = find(LN_UIElementEvent::DropEnter, "slot");
  ASSERT_NE(enter, nullptr);
  EXPECT_FALSE(enter->accepted);
  tick(103, 0, true);
  EXPECT_NE(find(LN_UIElementEvent::DragCancel, "potion"), nullptr);
  EXPECT_EQ(find(LN_UIElementEvent::DropReceived, "slot"), nullptr);
}

/* Carried with Grab, a refused item is shown over the slot (Drop Enter, not accepted), and putting
 * it down there puts it back. */
TEST(LN_UIService, ACarriedItemPutDownWhereItIsRefusedGoesBack)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  EXP_StringValue *category = new EXP_StringValue("potion", "category");
  item.object.SetProperty("category", category);
  category->Release();
  LN_UIService::ElementSettings item_settings = MakeElementSettings("potion");
  item_settings.draggable = true;
  item_settings.tab_order = 0;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.drop_target = true;
  slot_settings.tab_order = 1;
  slot_settings.accept_categories = ",weapon,";
  const LN_UIService::Registration item_registration = MakeRegistration(
      nullptr, 0, item.object, camera.object, 0, LN_UIPropagation::PassThrough);
  const LN_UIService::Registration slot_registration = MakeRegistration(
      nullptr, 0, slot.object, camera.object, 0, LN_UIPropagation::PassThrough);
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int key, const bool down) {
    PressKey(device, input, SCA_IInputDevice::SCA_EnumInputs(key), down);
    service.BeginTick(LN_Event::OnUpdate, index++, &input);
    service.RegisterElement(&item.object, item_settings, item_registration);
    service.RegisterElement(&slot.object, slot_settings, slot_registration);
    service.ResolveInteractions();
  };
  auto press = [&](const int key) {
    tick(key, true);
    tick(key, false);
  };
  tick(SCA_IInputDevice::GKEY, false);
  ASSERT_EQ(service.SetFocus(0, &item.object, std::string()), LN_UIResult::Success);
  press(SCA_IInputDevice::GKEY);
  tick(SCA_IInputDevice::GKEY, true);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::DragStart, "potion"), nullptr);
  tick(SCA_IInputDevice::GKEY, false);
  tick(SCA_IInputDevice::TABKEY, true);
  EXPECT_EQ(service.GetFocus(0, std::string()), &slot.object);
  const LN_UIService::ElementEvent *enter = service.FindElementEvent(LN_UIElementEvent::DropEnter,
                                                                     "slot");
  ASSERT_NE(enter, nullptr);
  EXPECT_FALSE(enter->accepted);
  tick(SCA_IInputDevice::TABKEY, false);
  tick(SCA_IInputDevice::RETKEY, true);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::DragCancel, "potion"), nullptr);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Drop, "potion"), nullptr);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::DropReceived, "slot"), nullptr);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::DropExit, "slot"), nullptr);
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
  tick(SCA_IInputDevice::RETKEY, false);
}

/* Several events of one kind in a frame are all there, in order, for On UI Event to run once for
 * each. */
TEST(LN_UIService, EveryMatchingEventOfAFrameCanBeReadInTurn)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> first(scene);
  SceneBoundObject<KX_GameObject> second(scene);
  SceneBoundObject<KX_GameObject> other(scene);
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.pointer = LN_UIPropagation::PassThrough;
  LN_UIService::ElementSettings other_settings = MakeElementSettings("other");
  other_settings.pointer = LN_UIPropagation::PassThrough;
  DEV_InputDevice device;
  LN_InputSnapshot input;
  CaptureInput(device, input, 100, 100);
  service.BeginTick(LN_Event::OnUpdate, 1, &input);
  service.RegisterElement(
      &first.object,
      slot_settings,
      MakeRegistration(nullptr, 0, first.object, camera.object, 0, LN_UIPropagation::PassThrough));
  service.RegisterElement(&second.object,
                          slot_settings,
                          MakeRegistration(nullptr,
                                           0,
                                           second.object,
                                           camera.object,
                                           0,
                                           LN_UIPropagation::PassThrough));
  service.RegisterElement(
      &other.object,
      other_settings,
      MakeRegistration(nullptr, 0, other.object, camera.object, 0, LN_UIPropagation::PassThrough));
  service.ResolveInteractions();

  size_t count = 0;
  const LN_UIService::ElementEvent *one = service.FindElementEvent(
      LN_UIElementEvent::Enter, "slot", -1, 0, &count);
  EXPECT_EQ(count, 2u);
  const LN_UIService::ElementEvent *two = service.FindElementEvent(
      LN_UIElementEvent::Enter, "slot", -1, 1, &count);
  ASSERT_NE(one, nullptr);
  ASSERT_NE(two, nullptr);
  EXPECT_NE(manager.ResolveObjectHandle(one->object), manager.ResolveObjectHandle(two->object));
  for (const LN_UIService::ElementEvent *event : {one, two}) {
    EXPECT_NE(manager.ResolveObjectHandle(event->object), &other.object);
  }
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Enter, "slot", -1, 2, &count), nullptr);
  EXPECT_EQ(count, 2u);
  /* Any Action: all three. */
  service.FindElementEvent(LN_UIElementEvent::Enter, std::string(), -1, 0, &count);
  EXPECT_EQ(count, 3u);
  /* Another player has none. */
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Enter, "slot", 1, 0, &count), nullptr);
  EXPECT_EQ(count, 0u);
}

/* Scroll Object View on a Grid View: show an item, top, bottom, pages and lines, with the page
 * in view and the page count. */
TEST(LN_UIService, ScrollRequestsMoveAGridViewByItemsPagesAndLines)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> slot0(scene);
  SceneBoundObject<KX_GameObject> slot1(scene);
  SceneBoundObject<KX_GameObject> slot2(scene);
  SceneBoundObject<KX_GameObject> slot3(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 9};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.cull_outside = false;
  view.columns = 2;
  view.slots = {&slot0.object, &slot1.object, &slot2.object, &slot3.object};
  /* Ten items in rows of two, two rows in view: rows 0-1, 2-3 and 4 are the three pages. */
  for (int item_index = 0; item_index < 10; ++item_index) {
    LN_Value item;
    item.type = LN_ValueType::String;
    item.exists = true;
    item.string_value = "item" + std::to_string(item_index);
    view.items.push_back(item);
  }
  uint64_t tick = 0;
  auto resolve = [&]() {
    service.BeginTick(LN_Event::OnFixedUpdate, ++tick, nullptr);
    service.RegisterView(view);
    service.ResolveViews();
    return service.GetViewState(view.listener);
  };
  /* Show Item by index, Lines by `amount`. */
  auto scroll = [&](const LN_UIViewScrollMode mode, const int amount = 0) {
    LN_UIService::ScrollTarget target;
    target.has_index = true;
    target.index = amount;
    service.RequestViewScroll(&viewport.object, mode, amount, target);
    return resolve();
  };
  LN_UIService::ViewState state = resolve();
  EXPECT_EQ(state.first_visible_index, 0);
  EXPECT_EQ(state.page, 1);
  EXPECT_EQ(state.page_count, 3);

  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_EQ(state.first_visible_index, 4);
  EXPECT_EQ(state.page, 2);
  EXPECT_TRUE(state.scrolled);
  /* The last page starts where the last item is still in view: rows 3-4, page 3. */
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_EQ(state.first_visible_index, 6);
  EXPECT_EQ(state.page, 3);
  EXPECT_TRUE(state.at_end);
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_EQ(state.first_visible_index, 6);
  EXPECT_FALSE(state.scrolled);
  /* A page back from where the view is: two rows up, none skipped. Page names the nearest page. */
  state = scroll(LN_UIViewScrollMode::PreviousPage);
  EXPECT_EQ(state.first_visible_index, 2);
  EXPECT_EQ(state.page, 2);
  state = scroll(LN_UIViewScrollMode::PreviousPage);
  EXPECT_EQ(state.first_visible_index, 0);
  EXPECT_EQ(state.page, 1);
  state = scroll(LN_UIViewScrollMode::Top);
  EXPECT_EQ(state.first_visible_index, 0);
  EXPECT_TRUE(state.at_start);
  /* Top works every time, also after the wheel moved the view (First Item only acts on a change).
   */
  state = scroll(LN_UIViewScrollMode::Bottom);
  EXPECT_EQ(state.first_visible_index, 6);
  state = scroll(LN_UIViewScrollMode::Top);
  EXPECT_EQ(state.first_visible_index, 0);

  /* Show Item scrolls the least: the item's row at the bottom, or at the top. */
  state = scroll(LN_UIViewScrollMode::ShowItem, 3);
  EXPECT_EQ(state.first_visible_index, 0);
  EXPECT_FALSE(state.scrolled);
  state = scroll(LN_UIViewScrollMode::ShowItem, 5);
  EXPECT_EQ(state.first_visible_index, 2);
  state = scroll(LN_UIViewScrollMode::ShowItem, 9);
  EXPECT_EQ(state.first_visible_index, 6);
  state = scroll(LN_UIViewScrollMode::ShowItem, 1);
  EXPECT_EQ(state.first_visible_index, 0);
  /* Past the end shows the last item; below 0 the first. */
  state = scroll(LN_UIViewScrollMode::ShowItem, 99);
  EXPECT_EQ(state.first_visible_index, 6);
  state = scroll(LN_UIViewScrollMode::ShowItem, -4);
  EXPECT_EQ(state.first_visible_index, 0);
  /* By item: the first one named like it; one that is not there does nothing. */
  LN_UIService::ScrollTarget named;
  named.item = "item7";
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::ShowItem, 0, named);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 4);
  named.item = "item1";
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::ShowItem, 0, named);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 0);
  /* Two items alike: one in view is enough, else the nearest one. */
  view.items[0].string_value = "twin";
  view.items[9].string_value = "twin";
  named.item = "twin";
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::Bottom, 0);
  resolve();
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::ShowItem, 0, named);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 6);
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::Lines, -1);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 4);
  /* Rows 2-3 in view: item 9 is one row below, item 0 two rows above. */
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::ShowItem, 0, named);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 6);
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::Top, 0);
  resolve();
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::ShowItem, 0, named);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 0);
  view.items[0].string_value = "item0";
  view.items[9].string_value = "item9";
  named.item = "missing";
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::Bottom, 0);
  state = resolve();
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::ShowItem, 0, named);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 6);
  EXPECT_FALSE(state.scrolled);
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::Top, 0);
  state = resolve();

  /* Lines: whole rows, clamped. */
  state = scroll(LN_UIViewScrollMode::Lines, 1);
  EXPECT_EQ(state.first_visible_index, 2);
  EXPECT_EQ(state.page, 2);
  state = scroll(LN_UIViewScrollMode::Lines, 10);
  EXPECT_EQ(state.first_visible_index, 6);
  state = scroll(LN_UIViewScrollMode::Lines, -1);
  EXPECT_EQ(state.first_visible_index, 4);
  state = scroll(LN_UIViewScrollMode::Lines, -10);
  EXPECT_EQ(state.first_visible_index, 0);

  /* A request for another view, or one whose view does not lay out within two frames, does
   * nothing. */
  SceneBoundObject<KX_GameObject> elsewhere(scene);
  service.RequestViewScroll(&elsewhere.object, LN_UIViewScrollMode::Bottom, 0);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 0);
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::Bottom, 0);
  for (int frame = 0; frame < 3; ++frame) {
    service.BeginTick(LN_Event::OnFixedUpdate, ++tick, nullptr);
    service.ResolveViews();
  }
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 0);

  /* Filtered (Search "food"): the five food items in view order, two pages counting what the grid
   * shows. Show Item takes an index in the list: the food at place 8 is the fifth shown, in row 2;
   * a hidden item does nothing; by name too. */
  for (int item_index = 0; item_index < 10; ++item_index) {
    view.items[size_t(item_index)].string_value = (item_index % 2 == 0 ? "food" : "rock") +
                                                  std::to_string(item_index);
  }
  service.SetItemLibraryForTests([](const std::string & /*item*/) -> KX_GameObject * {
    return nullptr;
  });
  view.search = "food";
  state = scroll(LN_UIViewScrollMode::ShowItem, 8);
  EXPECT_EQ(state.first_visible_index, 2);
  EXPECT_EQ(state.page_count, 2);
  state = scroll(LN_UIViewScrollMode::ShowItem, 3);
  EXPECT_EQ(state.first_visible_index, 2);
  EXPECT_FALSE(state.scrolled);
  state = scroll(LN_UIViewScrollMode::ShowItem, 0);
  EXPECT_EQ(state.first_visible_index, 0);
  LN_UIService::ScrollTarget food;
  food.item = "food8";
  service.RequestViewScroll(&viewport.object, LN_UIViewScrollMode::ShowItem, 0, food);
  state = resolve();
  EXPECT_EQ(state.first_visible_index, 2);
  view.search.clear();
  state = scroll(LN_UIViewScrollMode::Top);

  /* Everything fits: one page. */
  view.items.resize(3);
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_EQ(state.first_visible_index, 0);
  EXPECT_EQ(state.page, 1);
  EXPECT_EQ(state.page_count, 1);
  view.items.clear();
  state = resolve();
  EXPECT_EQ(state.page, 1);
  EXPECT_EQ(state.page_count, 1);
}

/* Pages count whole rows of slots only, and turn from wherever the view is. */
TEST(LN_UIService, GridPagesCountWholeRowsAndTurnFromTheView)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  std::vector<std::unique_ptr<SceneBoundObject<KX_GameObject>>> slots;
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 11};
  view.kind = LN_UIService::ViewKind::Grid;
  view.viewport = &viewport.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(0.0f, 0.0f, -2.0f);
  view.ray_to = MT_Vector3(0.0f, 0.0f, 2.0f);
  view.cull_outside = false;
  view.columns = 4;
  auto set_up = [&](const int slot_count, const int item_count) {
    view.slots.clear();
    while (int(slots.size()) < slot_count) {
      slots.push_back(std::make_unique<SceneBoundObject<KX_GameObject>>(scene));
    }
    for (int index = 0; index < slot_count; ++index) {
      view.slots.push_back(&slots[size_t(index)]->object);
    }
    view.items.clear();
    for (int index = 0; index < item_count; ++index) {
      LN_Value item;
      item.type = LN_ValueType::String;
      item.exists = true;
      item.string_value = "item" + std::to_string(index);
      view.items.push_back(item);
    }
  };
  uint64_t tick = 0;
  auto scroll = [&](const LN_UIViewScrollMode mode, const int lines = 0) {
    service.RequestViewScroll(&viewport.object, mode, lines);
    service.BeginTick(LN_Event::OnFixedUpdate, ++tick, nullptr);
    service.RegisterView(view);
    service.ResolveViews();
    return service.GetViewState(view.listener);
  };
  /* 10 slots in rows of 4: two whole rows make a page. 12 items: the view goes 0 or 4. */
  set_up(10, 12);
  LN_UIService::ViewState state = scroll(LN_UIViewScrollMode::Top);
  EXPECT_EQ(state.page, 1);
  EXPECT_EQ(state.page_count, 2);
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_EQ(state.first_visible_index, 4);
  EXPECT_EQ(state.page, 2);
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_EQ(state.first_visible_index, 4);
  state = scroll(LN_UIViewScrollMode::PreviousPage);
  EXPECT_EQ(state.first_visible_index, 0);
  EXPECT_EQ(state.page, 1);
  /* 12 slots (3 rows of 4), 40 items, the view on row 5 after the wheel or the D-pad: a page back
   * shows rows 2 to 4, none skipped; a page on stops at the end. */
  set_up(12, 40);
  state = scroll(LN_UIViewScrollMode::Top);
  EXPECT_EQ(state.page_count, 4);
  state = scroll(LN_UIViewScrollMode::Lines, 5);
  EXPECT_EQ(state.first_visible_index, 20);
  EXPECT_EQ(state.page, 3);
  state = scroll(LN_UIViewScrollMode::PreviousPage);
  EXPECT_EQ(state.first_visible_index, 8);
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_EQ(state.first_visible_index, 20);
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_EQ(state.first_visible_index, 28);
  EXPECT_EQ(state.page, 4);
  EXPECT_TRUE(state.at_end);
}

/* Scroll Object View on a Scroll View: start and end as At Start and At End say, a page is the
 * viewport, and lines are Wheel Steps. */
TEST(LN_UIService, ScrollRequestsMoveAScrollViewByPagesAndLines)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> viewport(scene);
  SceneBoundObject<KX_GameObject> content(scene);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  LN_UIService::ViewRegistration view;
  view.listener = {tree, 10};
  view.kind = LN_UIService::ViewKind::Scroll;
  view.viewport = &viewport.object;
  view.content = &content.object;
  view.camera = &camera.object;
  view.ray_from = MT_Vector3(5.0f, 5.0f, -2.0f);
  view.ray_to = MT_Vector3(5.0f, 5.0f, 2.0f);
  view.cull_outside = false;
  view.focus_follow = false;
  view.min_offset = MT_Vector3(0.0f, 0.0f, 0.0f);
  view.max_offset = MT_Vector3(0.0f, 10.0f, 0.0f);
  view.wheel_step = MT_Vector3(0.0f, 0.5f, 0.0f);
  /* The viewport's bounds (-0.5 to 0.5) scaled by 2: a page is 2. */
  view.world_scale = MT_Vector3(2.0f, 2.0f, 2.0f);
  uint64_t tick = 0;
  auto scroll = [&](const LN_UIViewScrollMode mode, const int amount = 0) {
    service.RequestViewScroll(&viewport.object, mode, amount);
    service.BeginTick(LN_Event::OnFixedUpdate, ++tick, nullptr);
    service.RegisterView(view);
    service.ResolveViews();
    return service.GetViewState(view.listener);
  };
  LN_UIService::ViewState state = scroll(LN_UIViewScrollMode::Bottom);
  EXPECT_FLOAT_EQ(state.offset.y(), 10.0f);
  EXPECT_TRUE(state.at_end);
  EXPECT_FLOAT_EQ(content.object.NodeGetLocalPosition().y(), 10.0f);
  state = scroll(LN_UIViewScrollMode::Top);
  EXPECT_FLOAT_EQ(state.offset.y(), 0.0f);
  EXPECT_TRUE(state.at_start);
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_FLOAT_EQ(state.offset.y(), 2.0f);
  state = scroll(LN_UIViewScrollMode::Lines, 3);
  EXPECT_FLOAT_EQ(state.offset.y(), 3.5f);
  state = scroll(LN_UIViewScrollMode::PreviousPage);
  EXPECT_FLOAT_EQ(state.offset.y(), 1.5f);
  state = scroll(LN_UIViewScrollMode::Lines, -10);
  EXPECT_FLOAT_EQ(state.offset.y(), 0.0f);
  /* The Content Object names the view too. A negative Wheel Step turns start and end around. */
  view.wheel_step = MT_Vector3(0.0f, -0.5f, 0.0f);
  service.RequestViewScroll(&content.object, LN_UIViewScrollMode::Top, 0);
  service.BeginTick(LN_Event::OnFixedUpdate, ++tick, nullptr);
  service.RegisterView(view);
  service.ResolveViews();
  state = service.GetViewState(view.listener);
  EXPECT_FLOAT_EQ(state.offset.y(), 10.0f);
  EXPECT_TRUE(state.at_start);
  state = scroll(LN_UIViewScrollMode::NextPage);
  EXPECT_FLOAT_EQ(state.offset.y(), 8.0f);
  /* Released while the content still exists. */
  service.BeginTick(LN_Event::OnFixedUpdate, ++tick, nullptr);
  service.ResolveViews();
}

/* A computer for Set Player Pointer and Take UI Control: a 4 x 4 screen at the origin facing +Z
 * (thin along Z), seen by every player's camera 10 above it (looking down -Z: right is +X, up is
 * +Y), with buttons parented to it. */
struct PointerComputer {
  explicit PointerComputer(KX_Scene *scene) : camera(scene), screen(scene), cursor(scene) {}

  void AddButton(SceneBoundObject<KX_GameObject> &button, const MT_Vector3 &position)
  {
    screen.node.AddChild(&button.node);
    button.node.SetParentRelation(new KX_NormalParentRelation());
    button.object.NodeSetLocalPosition(position);
    buttons.push_back(&button);
  }

  void Setup(LN_UIService &service)
  {
    camera.object.NodeSetWorldPosition(MT_Vector3(0.0f, 0.0f, 10.0f));
    camera.object.NodeUpdateGS(0.0f);
    screen.object.NodeUpdateGS(0.0f);
    service.SetPlayerCameraForTests([this](int32_t) { return &camera.object; });
    service.SetObjectBoundsForTests(
        [this](KX_GameObject *object, MT_Vector3 &r_min, MT_Vector3 &r_max) {
          if (object == &screen.object) {
            r_min = MT_Vector3(-2.0f, -2.0f, -0.01f);
            r_max = MT_Vector3(2.0f, 2.0f, 0.01f);
            return true;
          }
          for (SceneBoundObject<KX_GameObject> *button : buttons) {
            if (object == &button->object) {
              r_min = MT_Vector3(-0.5f, -0.5f, -0.05f);
              r_max = MT_Vector3(0.5f, 0.5f, 0.05f);
              return true;
            }
          }
          return false;
        });
  }

  /* The registration a button gets in the game: where it is, its bounds. */
  LN_UIService::Registration ButtonRegistration(SceneBoundObject<KX_GameObject> &button)
  {
    LN_UIService::Registration registration = MakeRegistration(
        nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::Block);
    registration.world_position = button.object.NodeGetWorldPosition();
    registration.bounds_min = MT_Vector3(-0.5f, -0.5f, -0.05f);
    registration.bounds_max = MT_Vector3(0.5f, 0.5f, 0.05f);
    return registration;
  }

  SceneBoundObject<KX_Camera> camera;
  SceneBoundObject<KX_GameObject> screen;
  SceneBoundObject<KX_GameObject> cursor;
  std::vector<SceneBoundObject<KX_GameObject> *> buttons;
};

/* Gamepad `index` with its left stick at x, y (-1..1, y up) and A held or not. */
static LN_GamepadSnapshot PointerGamepad(const float x, const float y, const bool a)
{
  LN_GamepadSnapshot gamepad = MakeStickGamepad(int32_t(x * 32767.0f), int32_t(-y * 32767.0f));
  gamepad.buttons[GAME_INPUT_GAMEPAD_A] = a ? 1 : 0;
  return gamepad;
}

/* Player 1's cursor: the left stick moves it on the screen, A presses and clicks the button
 * under it for player 1, the cursor shows where it is, in front of the button, for everyone. */
TEST(LN_UIService, APlayerPointerMovesWithTheStickAndClicksWithAccept)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  PointerComputer computer(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  computer.AddButton(button, MT_Vector3(1.0f, 0.0f, 0.1f));
  computer.Setup(service);
  LN_UIService::ElementSettings settings = MakeElementSettings("ok");
  settings.any_player = true;
  service.SetPlayerPointer(1, true, &computer.screen.object, &computer.cursor.object, "", 1.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  bool held = false;
  auto tick = [&](const float x, const float y, const bool a) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    LN_GamepadSnapshot gamepad = PointerGamepad(x, y, a);
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = a && !held ? 1 : 0;
    gamepad.released[GAME_INPUT_GAMEPAD_A] = !a && held ? 1 : 0;
    held = a;
    input.SetGamepadForTests(1, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterElement(&button.object, settings, computer.ButtonRegistration(button));
    service.ResolveInteractions();
    computer.cursor.object.NodeUpdateGS(0.0f);
  };
  /* It starts in the middle: nothing under it. */
  tick(0.0f, 0.0f, false);
  MT_Vector3 point;
  ASSERT_TRUE(service.GetPlayerPointerPoint(1, point));
  EXPECT_NEAR(point.x(), 0.0f, 1.0e-4f);
  EXPECT_TRUE(computer.cursor.object.GetVisible());
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Enter, "ok"), nullptr);
  /* Full tilt right: a screen width (4) a second, 0.4 a frame. */
  tick(1.0f, 0.0f, false);
  tick(1.0f, 0.0f, false);
  ASSERT_TRUE(service.GetPlayerPointerPoint(1, point));
  EXPECT_NEAR(point.x(), 0.8f, 1.0e-3f);
  const LN_UIService::ElementEvent *enter = service.FindElementEvent(LN_UIElementEvent::Enter,
                                                                     "ok");
  ASSERT_NE(enter, nullptr);
  EXPECT_EQ(enter->user, 1);
  /* The cursor is in front of the button (its top is at 0.15). */
  const MT_Vector3 cursor = computer.cursor.object.NodeGetWorldPosition();
  EXPECT_NEAR(cursor.x(), 0.8f, 1.0e-3f);
  EXPECT_GT(cursor.z(), 0.15f);
  EXPECT_LT(cursor.z(), 0.2f);
  /* A presses and clicks it for player 1 only. */
  tick(0.0f, 0.0f, true);
  const LN_UIService::ElementEvent *press = service.FindElementEvent(LN_UIElementEvent::Press,
                                                                     "ok");
  ASSERT_NE(press, nullptr);
  EXPECT_EQ(press->user, 1);
  tick(0.0f, 0.0f, false);
  const LN_UIService::ElementEvent *click = service.FindElementEvent(LN_UIElementEvent::Click,
                                                                     "ok");
  ASSERT_NE(click, nullptr);
  EXPECT_EQ(click->user, 1);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "ok", 0), nullptr);
  /* Pushed far past the edge, it stays on the screen. */
  for (int frame = 0; frame < 10; ++frame) {
    tick(1.0f, 1.0f, false);
  }
  ASSERT_TRUE(service.GetPlayerPointerPoint(1, point));
  EXPECT_NEAR(point.x(), 2.0f, 1.0e-3f);
  EXPECT_NEAR(point.y(), 2.0f, 1.0e-3f);
  /* Off: the cursor hides and the pointer is gone. */
  service.SetPlayerPointer(1, false, nullptr, nullptr, "", 1.0f);
  tick(0.0f, 0.0f, false);
  EXPECT_FALSE(computer.cursor.object.GetVisible());
  EXPECT_FALSE(service.GetPlayerPointerPoint(1, point));
}

/* Player 2 drags an item onto a slot with the cursor (A held); a drag ends as a cancel when the
 * pointer is turned off. */
TEST(LN_UIService, APlayerPointerDragsAndDropsAndCancelsWhenTurnedOff)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  PointerComputer computer(scene);
  SceneBoundObject<KX_GameObject> item(scene);
  SceneBoundObject<KX_GameObject> slot(scene);
  computer.AddButton(item, MT_Vector3(-1.0f, 0.0f, 0.1f));
  computer.AddButton(slot, MT_Vector3(1.0f, 0.0f, 0.1f));
  computer.Setup(service);
  LN_UIService::ElementSettings item_settings = MakeElementSettings("item");
  item_settings.any_player = true;
  item_settings.draggable = true;
  item_settings.follow_cursor = false;
  LN_UIService::ElementSettings slot_settings = MakeElementSettings("slot");
  slot_settings.any_player = true;
  slot_settings.drop_target = true;
  service.SetPlayerPointer(2, true, &computer.screen.object, nullptr, "", 1.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  bool held = false;
  auto tick = [&](const float x, const bool a) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    LN_GamepadSnapshot gamepad = PointerGamepad(x, 0.0f, a);
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = a && !held ? 1 : 0;
    gamepad.released[GAME_INPUT_GAMEPAD_A] = !a && held ? 1 : 0;
    held = a;
    input.SetGamepadForTests(2, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterElement(&item.object, item_settings, computer.ButtonRegistration(item));
    service.RegisterElement(&slot.object, slot_settings, computer.ButtonRegistration(slot));
    service.ResolveInteractions();
  };
  auto fired = [&](const LN_UIElementEvent type, const char *action) {
    const LN_UIService::ElementEvent *event = service.FindElementEvent(type, action);
    return event != nullptr && event->user == 2;
  };
  /* To the item (x -0.8), press A there and drag it right to the slot (x 0.8). */
  tick(0.0f, false);
  tick(-1.0f, false);
  tick(-1.0f, false);
  tick(0.0f, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::Press, "item"));
  for (int frame = 0; frame < 4; ++frame) {
    tick(1.0f, true);
  }
  EXPECT_TRUE(fired(LN_UIElementEvent::Drag, "item"));
  tick(0.0f, false);
  EXPECT_TRUE(fired(LN_UIElementEvent::DropReceived, "slot"));
  EXPECT_TRUE(fired(LN_UIElementEvent::Drop, "item"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);

  /* Back to the item, a drag started, then the pointer is turned off: Drag Cancel. */
  for (int frame = 0; frame < 4; ++frame) {
    tick(-1.0f, false);
  }
  tick(0.0f, true);
  tick(1.0f, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragStart, "item"));
  service.SetPlayerPointer(2, false, nullptr, nullptr, "", 1.0f);
  tick(0.0f, true);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel, "item"));
  EXPECT_EQ(service.CaptureCountForTests(), 0u);
}

/* Take UI Control: one player has the computer; the others (and the mouse) can't use it until
 * it is given back. The player's gamepad starts on its first button. */
TEST(LN_UIService, TakeUIControlGivesOnePlayerTheComputerUntilGivenBack)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  PointerComputer computer(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  computer.AddButton(button, MT_Vector3(0.0f, 0.0f, 0.1f));
  computer.Setup(service);
  LN_UIService::ElementSettings settings = MakeElementSettings("ok");
  settings.any_player = true;

  EXPECT_EQ(service.TakeControl(&computer.screen.object, 1, true), LN_UIResult::Success);
  EXPECT_EQ(service.TakeControl(&computer.screen.object, 1, true), LN_UIResult::NoChange);
  EXPECT_EQ(service.TakeControl(&computer.screen.object, 2, true),
            LN_UIResult::ControlledByOtherPlayer);
  /* Nor a part of it, nor give it back for player 1. */
  EXPECT_EQ(service.TakeControl(&button.object, 2, true), LN_UIResult::ControlledByOtherPlayer);
  EXPECT_EQ(service.TakeControl(&computer.screen.object, 2, false),
            LN_UIResult::ControlledByOtherPlayer);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const int mouse_button, const bool a) {
    /* The mouse ray (MakeRegistration's) goes through the button. */
    CaptureInput(device, input, 100, 100, mouse_button);
    LN_GamepadSnapshot gamepad;
    gamepad.connected = true;
    gamepad.buttons[GAME_INPUT_GAMEPAD_A] = a ? 1 : 0;
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = a ? 1 : 0;
    input.SetGamepadForTests(1, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterElement(&button.object, settings, computer.ButtonRegistration(button));
    service.ResolveInteractions();
  };
  /* The A press that took it (Use is A too) selects the first button but does not press it. */
  tick(-1, true);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "ok"), nullptr);
  tick(-1, false);
  /* Player 1's gamepad is on its first button, without Set UI Focus. */
  EXPECT_EQ(service.GetFocus(1, std::string()), &button.object);
  EXPECT_EQ(service.ControllerOf(&button.object), 1);
  /* The mouse (player 0) clicks nothing there, and can't select it. */
  tick(1, false);
  tick(0, false);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "ok"), nullptr);
  EXPECT_EQ(service.SetFocus(0, &button.object, std::string()), LN_UIResult::InvalidTarget);
  /* Player 1's A clicks it. */
  tick(-1, true);
  const LN_UIService::ElementEvent *click = service.FindElementEvent(LN_UIElementEvent::Click,
                                                                     "ok");
  ASSERT_NE(click, nullptr);
  EXPECT_EQ(click->user, 1);

  /* Given back: free for everyone again, and player 1's selection there goes. */
  EXPECT_EQ(service.TakeControl(&computer.screen.object, 1, false), LN_UIResult::Success);
  EXPECT_EQ(service.GetFocus(1, std::string()), nullptr);
  EXPECT_EQ(service.ControllerOf(&button.object), -1);
  tick(1, false);
  tick(0, false);
  click = service.FindElementEvent(LN_UIElementEvent::Click, "ok");
  ASSERT_NE(click, nullptr);
  EXPECT_EQ(click->user, 0);
  /* Taken again, then the computer is removed: free. */
  EXPECT_EQ(service.TakeControl(&computer.screen.object, 2, true), LN_UIResult::Success);
  service.NotifyObjectRemoved(&computer.screen.object);
  EXPECT_EQ(service.ControllerOf(&button.object), -1);
}

/* Player 0: the cursor follows the mouse on the screen, and the stick takes over (A clicks
 * under the cursor then); moving the mouse gives it back to the mouse. */
TEST(LN_UIService, PlayerZerosPointerFollowsTheMouseUntilTheStickMovesIt)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  PointerComputer computer(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  computer.AddButton(button, MT_Vector3(-1.0f, 0.0f, 0.1f));
  computer.Setup(service);
  /* A pixel is a hundredth of a unit: pixel (150, 50) is right above (1.5, 0.5). */
  service.SetPointerRayForTests([](KX_Camera *, const float x, const float y, MT_Vector3 &r_from,
                                   MT_Vector3 &r_to) {
    r_from = MT_Vector3(x / 100.0f, y / 100.0f, 10.0f);
    r_to = MT_Vector3(x / 100.0f, y / 100.0f, -10.0f);
    return true;
  });
  LN_UIService::ElementSettings settings = MakeElementSettings("ok");
  service.SetPlayerPointer(0, true, &computer.screen.object, &computer.cursor.object, "", 1.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  bool held = false;
  auto tick = [&](const int mouse_x, const float stick_x, const bool a) {
    CaptureInput(device, input, mouse_x, 50);
    LN_GamepadSnapshot gamepad = PointerGamepad(stick_x, 0.0f, a);
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = a && !held ? 1 : 0;
    gamepad.released[GAME_INPUT_GAMEPAD_A] = !a && held ? 1 : 0;
    held = a;
    input.SetGamepadForTests(0, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    LN_UIService::Registration registration = computer.ButtonRegistration(button);
    registration.ray_from = MT_Vector3(float(mouse_x) / 100.0f, 0.5f, 10.0f);
    registration.ray_to = MT_Vector3(float(mouse_x) / 100.0f, 0.5f, -10.0f);
    service.RegisterElement(&button.object, settings, registration);
    service.ResolveInteractions();
  };
  MT_Vector3 point;
  tick(100, 0.0f, false);
  tick(150, 0.0f, false);
  ASSERT_TRUE(service.GetPlayerPointerPoint(0, point));
  EXPECT_NEAR(point.x(), 1.5f, 1.0e-3f);
  EXPECT_NEAR(point.y(), 0.5f, 1.0e-3f);
  /* The stick moves it left from there (the mouse stays still), 0.4 a frame: onto the button
   * (-1.5 to -0.5) at 1.5 - 6 * 0.4 = -0.9. */
  for (int frame = 0; frame < 6; ++frame) {
    tick(150, -1.0f, false);
  }
  ASSERT_TRUE(service.GetPlayerPointerPoint(0, point));
  EXPECT_NEAR(point.x(), -0.9f, 1.0e-3f);
  /* A clicks under the cursor, not where the mouse is. */
  tick(150, 0.0f, true);
  tick(150, 0.0f, false);
  const LN_UIService::ElementEvent *click = service.FindElementEvent(LN_UIElementEvent::Click,
                                                                     "ok");
  ASSERT_NE(click, nullptr);
  EXPECT_EQ(click->user, 0);
  /* Moving the mouse brings the cursor back under it. */
  tick(120, 0.0f, false);
  ASSERT_TRUE(service.GetPlayerPointerPoint(0, point));
  EXPECT_NEAR(point.x(), 1.2f, 1.0e-3f);
}

/* Player 0 puts the cursor on a button with the mouse, then presses Accept (A, Enter, Space)
 * without moving the stick: the press clicks under the cursor, where the mouse left it. */
TEST(LN_UIService, PlayerZerosAcceptClicksWhereTheMouseLeftThePointer)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  PointerComputer computer(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  computer.AddButton(button, MT_Vector3(1.0f, 0.0f, 0.1f));
  computer.Setup(service);
  service.SetPointerRayForTests([](KX_Camera *, const float x, const float y, MT_Vector3 &r_from,
                                   MT_Vector3 &r_to) {
    r_from = MT_Vector3(x / 100.0f, y / 100.0f, 10.0f);
    r_to = MT_Vector3(x / 100.0f, y / 100.0f, -10.0f);
    return true;
  });
  LN_UIService::ElementSettings settings = MakeElementSettings("ok");
  service.SetPlayerPointer(0, true, &computer.screen.object, &computer.cursor.object, "", 1.0f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  bool held = false;
  auto tick = [&](const int mouse_x, const bool a) {
    CaptureInput(device, input, mouse_x, 20);
    LN_GamepadSnapshot gamepad = PointerGamepad(0.0f, 0.0f, a);
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = a && !held ? 1 : 0;
    gamepad.released[GAME_INPUT_GAMEPAD_A] = !a && held ? 1 : 0;
    held = a;
    input.SetGamepadForTests(0, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    LN_UIService::Registration registration = computer.ButtonRegistration(button);
    registration.ray_from = MT_Vector3(float(mouse_x) / 100.0f, 0.2f, 10.0f);
    registration.ray_to = MT_Vector3(float(mouse_x) / 100.0f, 0.2f, -10.0f);
    service.RegisterElement(&button.object, settings, registration);
    service.ResolveInteractions();
  };
  /* The mouse moves the cursor onto the button (0.5 to 1.5). */
  tick(-100, false);
  tick(100, false);
  MT_Vector3 point;
  ASSERT_TRUE(service.GetPlayerPointerPoint(0, point));
  EXPECT_NEAR(point.x(), 1.0f, 1.0e-3f);
  EXPECT_NEAR(point.y(), 0.2f, 1.0e-3f);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "ok"), nullptr);
  /* Its press seen by Logic + Physics steps before the next frame is the UI's, not the game's. */
  {
    LN_InputSnapshot early;
    CaptureInput(device, early, 100, 20);
    LN_GamepadSnapshot gamepad = PointerGamepad(0.0f, 0.0f, true);
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = 1;
    early.SetGamepadForTests(0, gamepad);
    service.ConsumeInputBeforeResolve(early);
    EXPECT_TRUE(manager.GetInputConsumption().IsGamepadPressConsumed(0, GAME_INPUT_GAMEPAD_A));
  }
  /* A, with the mouse and the stick still. */
  tick(100, true);
  tick(100, false);
  const LN_UIService::ElementEvent *click = service.FindElementEvent(LN_UIElementEvent::Click,
                                                                     "ok");
  ASSERT_NE(click, nullptr);
  EXPECT_EQ(click->user, 0);
  /* The cursor stayed where the mouse left it. */
  ASSERT_TRUE(service.GetPlayerPointerPoint(0, point));
  EXPECT_NEAR(point.x(), 1.0f, 1.0e-3f);
}

/* A monitor stands up facing -Y (rotated like the demo's), seen by player 2 from in front: the
 * stick moves the pointer as the player sees it (right is +X, up is +Z), at the node's Speed. It
 * reaches player 2's elements, not another player's, and none once player 1 takes the computer. */
TEST(LN_UIService, APlayerPointerOnAStandingScreenReachesOnlyWhatItsPlayerMayUse)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  SceneBoundObject<KX_Camera> camera(scene);
  SceneBoundObject<KX_GameObject> screen(scene);
  SceneBoundObject<KX_GameObject> mine(scene);
  SceneBoundObject<KX_GameObject> theirs(scene);
  /* Rotated 90 degrees about X: the screen's local +Z faces world -Y, its local +Y is world +Z. */
  screen.object.NodeSetLocalOrientation(MT_Matrix3x3(MT_Vector3(float(M_PI_2), 0.0f, 0.0f)));
  for (auto [button, x] : {std::pair{&mine, 1.0f}, std::pair{&theirs, -1.0f}}) {
    screen.node.AddChild(&button->node);
    button->node.SetParentRelation(new KX_NormalParentRelation());
    button->object.NodeSetLocalPosition(MT_Vector3(x, 0.0f, 0.1f));
  }
  screen.object.NodeUpdateGS(0.0f);
  /* The camera 10 in front, looking along +Y. */
  camera.object.NodeSetLocalPosition(MT_Vector3(0.0f, -10.0f, 0.0f));
  camera.object.NodeSetLocalOrientation(MT_Matrix3x3(MT_Vector3(float(M_PI_2), 0.0f, 0.0f)));
  camera.object.NodeUpdateGS(0.0f);
  service.SetPlayerCameraForTests([&](int32_t) { return &camera.object; });
  service.SetObjectBoundsForTests(
      [&](KX_GameObject *object, MT_Vector3 &r_min, MT_Vector3 &r_max) {
        const float half = object == &screen.object ? 2.0f : 0.5f;
        const float depth = object == &screen.object ? 0.01f : 0.05f;
        r_min = MT_Vector3(-half, -half, -depth);
        r_max = MT_Vector3(half, half, depth);
        return true;
      });
  LN_UIService::ElementSettings mine_settings = MakeElementSettings("mine");
  mine_settings.player = 2;
  LN_UIService::ElementSettings theirs_settings = MakeElementSettings("theirs");
  theirs_settings.player = 3;
  auto registration = [&](SceneBoundObject<KX_GameObject> &button, const int32_t user) {
    LN_UIService::Registration item = MakeRegistration(
        nullptr, 0, button.object, camera.object, 0, LN_UIPropagation::Block, user);
    item.world_position = button.object.NodeGetWorldPosition();
    item.world_orientation = button.object.NodeGetWorldOrientation();
    item.bounds_min = MT_Vector3(-0.5f, -0.5f, -0.05f);
    item.bounds_max = MT_Vector3(0.5f, 0.5f, 0.05f);
    return item;
  };
  /* Half speed: half a screen width (2) a second at full tilt, 0.2 a frame. */
  service.SetPlayerPointer(2, true, &screen.object, nullptr, "", 0.5f);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const float x, const float y) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    input.SetGamepadForTests(2, PointerGamepad(x, y, false));
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterElement(&mine.object, mine_settings, registration(mine, 2));
    service.RegisterElement(&theirs.object, theirs_settings, registration(theirs, 3));
    service.ResolveInteractions();
  };
  MT_Vector3 point;
  tick(0.0f, 0.0f);
  ASSERT_TRUE(service.GetPlayerPointerPoint(2, point));
  /* The front face is the one facing the camera (-Y). */
  EXPECT_NEAR(point.y(), -0.01f, 1.0e-4f);
  tick(0.0f, 1.0f);
  ASSERT_TRUE(service.GetPlayerPointerPoint(2, point));
  EXPECT_NEAR(point.z(), 0.2f, 1.0e-3f);
  EXPECT_NEAR(point.x(), 0.0f, 1.0e-3f);
  tick(0.0f, -1.0f);
  /* Right, onto player 2's own element (0.5 to 1.5). */
  const LN_UIService::ElementEvent *entered = nullptr;
  for (int frame = 0; frame < 6; ++frame) {
    tick(1.0f, 0.0f);
    if (const LN_UIService::ElementEvent *event = service.FindElementEvent(
            LN_UIElementEvent::Enter, "mine"))
    {
      entered = event->user == 2 ? event : entered;
    }
  }
  ASSERT_TRUE(service.GetPlayerPointerPoint(2, point));
  EXPECT_NEAR(point.x(), 1.2f, 1.0e-3f);
  EXPECT_NE(entered, nullptr);
  bool entered_theirs = false;
  for (int frame = 0; frame < 12; ++frame) {
    tick(-1.0f, 0.0f);
    entered_theirs = entered_theirs ||
                     service.FindElementEvent(LN_UIElementEvent::Enter, "theirs") != nullptr;
  }
  ASSERT_TRUE(service.GetPlayerPointerPoint(2, point));
  EXPECT_NEAR(point.x(), -1.2f, 1.0e-3f);
  /* Over player 3's element: nothing for player 2. */
  EXPECT_FALSE(entered_theirs);

  /* Player 1 takes the computer: player 2's pointer reaches nothing there any more. */
  EXPECT_EQ(service.TakeControl(&screen.object, 1, true), LN_UIResult::Success);
  bool entered_mine = false;
  for (int frame = 0; frame < 12; ++frame) {
    tick(1.0f, 0.0f);
    entered_mine = entered_mine ||
                   service.FindElementEvent(LN_UIElementEvent::Enter, "mine") != nullptr;
  }
  ASSERT_TRUE(service.GetPlayerPointerPoint(2, point));
  EXPECT_NEAR(point.x(), 1.2f, 1.0e-3f);
  EXPECT_FALSE(entered_mine);
}

/* While player 1's pointer is on: a menu of theirs freezes it (the stick drives the menu), and the
 * mouse ray a tree gave one of their listeners reaches nothing. A new cursor replaces the old. */
TEST(LN_UIService, APlayerPointerWaitsInMenusAndOnlyItsOwnRayHits)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  PointerComputer computer(scene);
  SceneBoundObject<KX_GameObject> other_cursor(scene);
  SceneBoundObject<KX_GameObject> hud(scene);
  computer.Setup(service);
  service.SetPlayerPointer(1, true, &computer.screen.object, &computer.cursor.object, "", 1.0f);
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  /* A player 1 listener in the menu domain, under the mouse ray (MakeRegistration's). */
  LN_UIService::Registration listener = MakeRegistration(
      tree, 7, hud.object, computer.camera.object, 0, LN_UIPropagation::Block, 1);
  listener.interaction_domain = LN_InteractionDomain::Overlay;

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const float x) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    input.SetGamepadForTests(1, PointerGamepad(x, 0.0f, false));
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterInteractable(listener);
    service.ResolveInteractions();
  };
  tick(0.0f);
  EXPECT_FALSE(service.GetInteractionState({tree, 7}).over);
  tick(1.0f);
  MT_Vector3 point;
  ASSERT_TRUE(service.GetPlayerPointerPoint(1, point));
  EXPECT_NEAR(point.x(), 0.4f, 1.0e-3f);
  /* Player 1 opens a menu: the stick no longer moves the pointer; closed, it does again. */
  const LN_UIService::MenuHandle menu = service.AddMenuForTests(
      1, LN_UIMenuRouting::Modal, LN_UIMenuBackBehavior::CloseMenu);
  tick(1.0f);
  tick(1.0f);
  ASSERT_TRUE(service.GetPlayerPointerPoint(1, point));
  EXPECT_NEAR(point.x(), 0.4f, 1.0e-3f);
  EXPECT_TRUE(computer.cursor.object.GetVisible());
  service.CloseMenu(menu);
  tick(1.0f);
  ASSERT_TRUE(service.GetPlayerPointerPoint(1, point));
  EXPECT_NEAR(point.x(), 0.8f, 1.0e-3f);
  /* Another cursor: the old one hides, the new one shows. */
  service.SetPlayerPointer(1, true, &computer.screen.object, &other_cursor.object, "", 1.0f);
  EXPECT_FALSE(computer.cursor.object.GetVisible());
  other_cursor.object.SetVisible(false, false);
  tick(0.0f);
  EXPECT_TRUE(other_cursor.object.GetVisible());
  EXPECT_FALSE(service.GetInteractionState({tree, 7}).over);
}

/* A player who took a UI presses only on it: a HUD button they selected outside it is not
 * pressed by their Accept. */
TEST(LN_UIService, TakeUIControlKeepsAPlayersAcceptOnTheUI)
{
  LN_InputMap::Get().Load(nullptr);
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  PointerComputer computer(scene);
  SceneBoundObject<KX_GameObject> button(scene);
  SceneBoundObject<KX_GameObject> hud(scene);
  computer.AddButton(button, MT_Vector3(0.0f, 0.0f, 0.1f));
  computer.Setup(service);
  LN_UIService::ElementSettings button_settings = MakeElementSettings("ok");
  button_settings.any_player = true;
  LN_UIService::ElementSettings hud_settings = MakeElementSettings("hud");
  hud_settings.player = 1;
  LN_UIService::Registration hud_registration = MakeRegistration(
      nullptr, 0, hud.object, computer.camera.object, 0, LN_UIPropagation::Block);
  hud_registration.world_position = MT_Vector3(5.0f, 0.0f, 0.0f);
  ASSERT_EQ(service.TakeControl(&computer.screen.object, 1, true), LN_UIResult::Success);

  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  auto tick = [&](const bool a) {
    device.ClearInputs();
    input.Capture(&device, nullptr);
    LN_GamepadSnapshot gamepad;
    gamepad.connected = true;
    gamepad.buttons[GAME_INPUT_GAMEPAD_A] = a ? 1 : 0;
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = a ? 1 : 0;
    input.SetGamepadForTests(1, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    service.RegisterElement(&button.object, button_settings, computer.ButtonRegistration(button));
    service.RegisterElement(&hud.object, hud_settings, hud_registration);
    service.ResolveInteractions();
  };
  tick(false);
  ASSERT_EQ(service.GetFocus(1, std::string()), &button.object);
  ASSERT_EQ(service.SetFocus(1, &hud.object, std::string()), LN_UIResult::Success);
  tick(true);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "hud"), nullptr);
  /* Given back, Accept presses it as before. */
  ASSERT_EQ(service.TakeControl(&computer.screen.object, 1, false), LN_UIResult::Success);
  ASSERT_EQ(service.SetFocus(1, &hud.object, std::string()), LN_UIResult::Success);
  tick(false);
  tick(true);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Click, "hud"), nullptr);
}

/* The defaults plus Use (E, gamepad A): what View Center pointers press with below. */
static void LoadUseInputMap()
{
  blender::GameData game;
  blender::BKE_game_input_map_ensure_builtins(&game);
  blender::GameInputAction *use = blender::BKE_game_input_action_add(
      &game, "Use", GAME_INPUT_ACTION_BUTTON);
  blender::BKE_game_input_binding_add(use, GAME_INPUT_KEYBOARD)->code = blender::EVT_EKEY;
  blender::BKE_game_input_binding_add(use, GAME_INPUT_GAMEPAD)->code = GAME_INPUT_GAMEPAD_A;
  LN_InputMap::Get().Load(&game);
  blender::BKE_game_input_map_free(&game);
}

/* A public computer's button at the origin facing +Z (1 x 1, 0.1 thick), Any Player, and a camera
 * for each player looking down -Z at it; tick() presses Use with player 0's E and player 1's
 * gamepad A. */
struct AimComputer {
  explicit AimComputer(KX_Scene *scene) : button(scene), camera0(scene), camera1(scene)
  {
    settings.any_player = true;
  }

  void Setup(LN_UIService &service)
  {
    camera0.object.NodeSetWorldPosition(MT_Vector3(0.2f, 0.0f, 2.0f));
    camera1.object.NodeSetWorldPosition(MT_Vector3(-0.2f, 0.1f, 2.5f));
    camera0.object.NodeUpdateGS(0.0f);
    camera1.object.NodeUpdateGS(0.0f);
    service.SetPlayerCameraForTests([this](const int32_t user) -> KX_Camera * {
      return user == 0 ? &camera0.object : user == 1 ? &camera1.object : nullptr;
    });
    /* No walls (the physics world needs a running scene). */
    service.SetWorldRayForTests([](const MT_Vector3 &,
                                   const MT_Vector3 &,
                                   const std::vector<KX_GameObject *> &,
                                   float &) -> KX_GameObject * { return nullptr; });
  }

  LN_UIService::Registration ButtonRegistration(const int32_t user)
  {
    LN_UIService::Registration registration = MakeRegistration(
        nullptr,
        0,
        button.object,
        user == 0 ? camera0.object : camera1.object,
        0,
        LN_UIPropagation::Block,
        user);
    registration.world_position = button.object.NodeGetWorldPosition();
    registration.bounds_min = MT_Vector3(-0.5f, -0.5f, -0.05f);
    registration.bounds_max = MT_Vector3(0.5f, 0.5f, 0.05f);
    /* As the game registers it without the mouse over it: no ray of its own. */
    registration.ray_from = registration.ray_to = registration.camera->NodeGetWorldPosition();
    registration.has_precomputed_hit = true;
    registration.precomputed_hit = false;
    registration.enabled = interactable;
    return registration;
  }
  bool interactable = true;

  /* `e` and `a`: player 0's E key and player 1's A held this frame. */
  void Tick(LN_UIService &service, const bool e, const bool a)
  {
    device.ClearInputs();
    if (e != e_held) {
      device.ConvertEvent(SCA_IInputDevice::EKEY, e ? 1 : 0, 0);
    }
    e_held = e;
    input.Capture(&device, nullptr);
    LN_GamepadSnapshot gamepad = PointerGamepad(0.0f, 0.0f, a);
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = a && !a_held ? 1 : 0;
    gamepad.released[GAME_INPUT_GAMEPAD_A] = !a && a_held ? 1 : 0;
    a_held = a;
    input.SetGamepadForTests(1, gamepad);
    service.BeginTick(LN_Event::OnUpdate, index++, &input, 0.1);
    for (int32_t user = 0; user < 2; ++user) {
      service.RegisterElement(&button.object, settings, ButtonRegistration(user));
    }
    service.ResolveInteractions();
  }

  SceneBoundObject<KX_GameObject> button;
  SceneBoundObject<KX_Camera> camera0;
  SceneBoundObject<KX_Camera> camera1;
  LN_UIService::ElementSettings settings = MakeElementSettings("gift");
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 1;
  bool e_held = false;
  bool a_held = false;
};

/* Set Player Pointer's View Center: two players, each with their own camera, aim at the same Any
 * Player button and press it in the same frame. Use is the UI's only while a player aims at it:
 * looking away, it reaches the game. */
TEST(LN_UIService, TwoPlayersAimAtOneButtonAndPressItAtOnce)
{
  LoadUseInputMap();
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  AimComputer computer(scene);
  computer.Setup(service);
  service.SetPlayerAimPointer(0, true, 3.0f, "Use");
  service.SetPlayerAimPointer(1, true, 3.0f, "Use");
  const LN_InputConsumption &consumed = manager.GetInputConsumption();

  /* Player 0 looks away: their Use is the game's. Player 1 aims at the button. */
  computer.camera0.object.NodeSetWorldPosition(MT_Vector3(3.0f, 0.0f, 2.0f));
  computer.camera0.object.NodeUpdateGS(0.0f);
  computer.Tick(service, false, false);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Enter, "gift", 0), nullptr);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Enter, "gift", 1), nullptr);
  computer.Tick(service, true, false);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Press, "gift"), nullptr);
  EXPECT_FALSE(consumed.IsStrokeConsumed(SCA_IInputDevice::EKEY));
  EXPECT_FALSE(consumed.IsPressConsumed(SCA_IInputDevice::EKEY));
  computer.Tick(service, false, false);
  MT_Vector3 point;
  ASSERT_TRUE(service.GetPlayerPointerPoint(0, point));
  EXPECT_NEAR(point.z(), -1.0f, 1.0e-4f);

  /* Both aim at it. */
  computer.camera0.object.NodeSetWorldPosition(MT_Vector3(0.2f, 0.0f, 2.0f));
  computer.camera0.object.NodeUpdateGS(0.0f);
  computer.Tick(service, false, false);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Enter, "gift", 0), nullptr);
  /* Where each one aims: on the button's top. */
  ASSERT_TRUE(service.GetPlayerPointerPoint(1, point));
  EXPECT_NEAR(point.x(), -0.2f, 1.0e-4f);
  EXPECT_NEAR(point.z(), 0.05f, 1.0e-4f);
  /* Both press in the same frame, and both click. */
  computer.Tick(service, true, true);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Press, "gift", 0), nullptr);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Press, "gift", 1), nullptr);
  EXPECT_TRUE(consumed.IsStrokeConsumed(SCA_IInputDevice::EKEY));
  EXPECT_TRUE(consumed.IsGamepadPressConsumed(1, GAME_INPUT_GAMEPAD_A));
  computer.Tick(service, false, false);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Click, "gift", 0), nullptr);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Click, "gift", 1), nullptr);

  /* Accept (Enter, Space) is not Use: it presses nothing. */
  PressKey(computer.device, computer.input, SCA_IInputDevice::RETKEY, true);
  service.BeginTick(LN_Event::OnUpdate, computer.index++, &computer.input, 0.1);
  for (int32_t user = 0; user < 2; ++user) {
    service.RegisterElement(
        &computer.button.object, computer.settings, computer.ButtonRegistration(user));
  }
  service.ResolveInteractions();
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Press, "gift"), nullptr);

  /* Out of reach (2.45 from player 1's camera to the button): nothing for player 1. */
  service.SetPlayerAimPointer(1, true, 2.0f, "Use");
  computer.Tick(service, false, false);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Exit, "gift", 1), nullptr);
  computer.Tick(service, false, true);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Press, "gift", 1), nullptr);
  computer.Tick(service, false, false);

  /* Off: the pointer is gone. */
  service.SetPlayerAimPointer(0, false, 3.0f, "Use");
  computer.Tick(service, false, false);
  EXPECT_FALSE(service.GetPlayerPointerPoint(0, point));
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Exit, "gift", 0), nullptr);
}

/* View Center does not reach through a wall, but the body its camera hangs from does not hide
 * anything. A menu of the player's stops it, and switching to a surface lets go of a press. */
TEST(LN_UIService, ViewCenterStopsAtWallsAndWaitsInMenus)
{
  LoadUseInputMap();
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  AimComputer computer(scene);
  computer.Setup(service);
  /* Player 0's camera is the head of a body that surrounds it. */
  SceneBoundObject<KX_GameObject> body(scene);
  SceneBoundObject<KX_GameObject> wall(scene);
  body.node.AddChild(&computer.camera0.node);
  computer.camera0.node.SetParentRelation(new KX_NormalParentRelation());
  body.object.NodeSetWorldPosition(MT_Vector3(0.2f, 0.0f, 1.5f));
  computer.camera0.object.NodeSetLocalPosition(MT_Vector3(0.0f, 0.0f, 0.5f));
  body.object.NodeUpdateGS(0.0f);
  ASSERT_EQ(computer.camera0.object.GetParent(), &body.object);
  bool wall_up = false;
  std::vector<KX_GameObject *> last_ignored;
  service.SetWorldRayForTests([&](const MT_Vector3 &from,
                                  const MT_Vector3 & /*to*/,
                                  const std::vector<KX_GameObject *> &ignored,
                                  float &r_fraction) -> KX_GameObject * {
    /* Player 0's ray: the body around their camera first, then the wall halfway to the button. */
    if ((from - computer.camera0.object.NodeGetWorldPosition()).length() > 1.0e-3f) {
      return nullptr;
    }
    last_ignored = ignored;
    if (std::find(ignored.begin(), ignored.end(), &body.object) == ignored.end()) {
      r_fraction = 0.0f;
      return &body.object;
    }
    if (wall_up) {
      r_fraction = 0.3f;
      return &wall.object;
    }
    return nullptr;
  });
  service.SetPlayerAimPointer(0, true, 3.0f, "Use");
  computer.Tick(service, false, false);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Enter, "gift", 0), nullptr);
  EXPECT_NE(std::find(last_ignored.begin(), last_ignored.end(), &body.object),
            last_ignored.end());
  wall_up = true;
  computer.Tick(service, false, false);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Exit, "gift", 0), nullptr);
  computer.Tick(service, true, false);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Press, "gift", 0), nullptr);
  EXPECT_FALSE(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::EKEY));
  computer.Tick(service, false, false);
  wall_up = false;

  /* Player 0's menu has their input: View Center waits, and the UI uses their input. */
  computer.Tick(service, false, false);
  EXPECT_FALSE(service.UIUsesPlayerInput(0));
  const LN_UIService::MenuHandle menu = service.AddMenuForTests(
      0, LN_UIMenuRouting::Modal, LN_UIMenuBackBehavior::CloseMenu);
  computer.Tick(service, false, false);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Exit, "gift", 0), nullptr);
  EXPECT_TRUE(service.UIUsesPlayerInput(0));
  EXPECT_FALSE(service.UIUsesPlayerInput(1));
  service.CloseMenu(menu);
  computer.Tick(service, false, false);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Enter, "gift", 0), nullptr);
  EXPECT_FALSE(service.UIUsesPlayerInput(0));

  /* Player 1 presses, then switches to a surface pointer: the press is let go (no click), and a
   * surface pointer uses the player's input. */
  service.SetPlayerAimPointer(1, true, 3.0f, "Use");
  computer.Tick(service, false, false);
  computer.Tick(service, false, true);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Press, "gift", 1), nullptr);
  EXPECT_FALSE(service.UIUsesPlayerInput(1));
  PointerComputer surface(scene);
  surface.Setup(service);
  service.SetPlayerPointer(1, true, &surface.screen.object, nullptr, "", 1.0f, "Use");
  computer.Tick(service, false, false);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "gift", 1), nullptr);
  EXPECT_TRUE(service.UIUsesPlayerInput(1));
}

/* What View Center's ray starts inside (a body around a camera that only follows it) hides
 * nothing. Turning a Surface pointer off does not turn View Center off; turning it off and View
 * Center on in one go lets go of the surface's press. At a disabled button with Block, Use is
 * still kept from the game. */
TEST(LN_UIService, ViewCenterSkipsWhatItStartsInAndKeepsItsOwnSwitches)
{
  LoadUseInputMap();
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  AimComputer computer(scene);
  computer.Setup(service);
  SceneBoundObject<KX_GameObject> body(scene);
  int casts = 0;
  service.SetWorldRayForTests([&](const MT_Vector3 &from,
                                  const MT_Vector3 & /*to*/,
                                  const std::vector<KX_GameObject *> &ignored,
                                  float &r_fraction) -> KX_GameObject * {
    if ((from - computer.camera0.object.NodeGetWorldPosition()).length() > 1.0e-3f) {
      return nullptr;
    }
    ++casts;
    /* The body is around player 0's camera but not its parent. */
    if (std::find(ignored.begin(), ignored.end(), &body.object) == ignored.end()) {
      r_fraction = 0.0f;
      return &body.object;
    }
    return nullptr;
  });
  service.SetPlayerAimPointer(0, true, 3.0f, "Use");
  computer.Tick(service, false, false);
  EXPECT_NE(service.FindElementEvent(LN_UIElementEvent::Enter, "gift", 0), nullptr);
  EXPECT_EQ(casts, 2);

  /* A Surface node turning its pointer off leaves View Center on. */
  service.SetPlayerPointer(0, false, nullptr, nullptr, "", 1.0f);
  computer.Tick(service, false, false);
  MT_Vector3 point;
  EXPECT_TRUE(service.GetPlayerPointerPoint(0, point));
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Exit, "gift", 0), nullptr);

  /* Player 1 holds A on the gift button with a surface pointer, then that pointer is turned off
   * and View Center on in the same flush: the press is let go, no click comes. */
  PointerComputer surface(scene);
  SceneBoundObject<KX_GameObject> surface_button(scene);
  surface.AddButton(surface_button, MT_Vector3(0.0f, 0.0f, 0.1f));
  surface.Setup(service);
  service.SetWorldRayForTests([](const MT_Vector3 &,
                                 const MT_Vector3 &,
                                 const std::vector<KX_GameObject *> &,
                                 float &) -> KX_GameObject * { return nullptr; });
  LN_UIService::ElementSettings surface_settings = MakeElementSettings("screen");
  surface_settings.any_player = true;
  service.SetPlayerPointer(1, true, &surface.screen.object, nullptr, "", 1.0f);
  auto surface_tick = [&](const bool a) {
    computer.device.ClearInputs();
    computer.input.Capture(&computer.device, nullptr);
    LN_GamepadSnapshot gamepad = PointerGamepad(0.0f, 0.0f, a);
    gamepad.pressed[GAME_INPUT_GAMEPAD_A] = a && !computer.a_held ? 1 : 0;
    gamepad.released[GAME_INPUT_GAMEPAD_A] = !a && computer.a_held ? 1 : 0;
    computer.a_held = a;
    computer.input.SetGamepadForTests(1, gamepad);
    service.BeginTick(LN_Event::OnUpdate, computer.index++, &computer.input, 0.1);
    LN_UIService::Registration registration = surface.ButtonRegistration(surface_button);
    registration.user = 1;
    service.RegisterElement(&surface_button.object, surface_settings, registration);
    service.ResolveInteractions();
  };
  surface_tick(false);
  surface_tick(true);
  ASSERT_NE(service.FindElementEvent(LN_UIElementEvent::Press, "screen", 1), nullptr);
  service.SetPlayerPointer(1, false, nullptr, nullptr, "", 1.0f);
  /* Aimed from the computer's camera at the same button: a release there would click it. */
  service.SetPlayerAimPointer(1, true, 12.0f, "Use");
  surface_tick(true);
  MT_Vector3 aimed;
  ASSERT_TRUE(service.GetPlayerPointerPoint(1, aimed));
  EXPECT_NEAR(aimed.z(), 0.15f, 1.0e-3f);
  surface_tick(false);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Click, "screen", 1), nullptr);

  /* A View Center player at a disabled button with Block: no press, and Use is not the game's. */
  computer.Setup(service);
  computer.interactable = false;
  computer.Tick(service, false, false);
  computer.Tick(service, true, false);
  EXPECT_EQ(service.FindElementEvent(LN_UIElementEvent::Press, "gift", 0), nullptr);
  EXPECT_TRUE(manager.GetInputConsumption().IsStrokeConsumed(SCA_IInputDevice::EKEY));
}

/* Grid View Inventory: a grid that names its list (Object + Property) changes it itself. */

namespace {

/** A list game property of plain item names, as an inventory holds them. */
void SetItemList(KX_GameObject &object, const char *property, const std::vector<std::string> &names)
{
  LN_Value list;
  list.type = LN_ValueType::List;
  list.exists = true;
  for (const std::string &name : names) {
    LN_Value item;
    item.type = LN_ValueType::String;
    item.exists = true;
    item.string_value = name;
    list.list_value.push_back(item);
  }
  std::string reason;
  EXPECT_TRUE(LN_CommandBuffer::ApplyGamePropertyValue(&object, property, list, nullptr, reason))
      << reason;
}

/** An inventory as players see it: "Apple", or "Apple x2" for a stack. */
std::vector<std::string> ListedItems(KX_GameObject &object, const char *property)
{
  std::vector<std::string> names;
  for (const LN_Value &slot :
       LN_GamePropertyValue(object.GetProperty(property), nullptr).list_value)
  {
    std::string item;
    int32_t count = 0;
    if (LN_InventoryEntry(slot, item, count)) {
      names.push_back(count > 1 ? item + " x" + std::to_string(count) : item);
    }
  }
  return names;
}

void SetTextProperty(KX_GameObject &object, const char *name, const std::string &text)
{
  EXP_StringValue *value = new EXP_StringValue(text, name);
  object.SetProperty(name, value);
  value->Release();
}

std::string TextProperty(KX_GameObject &object)
{
  EXP_Value *value = object.GetProperty("Text");
  return value != nullptr ? value->GetText() : std::string("(none)");
}

/** Grid Views of slots in a row in front of a panel, each showing a game property, used with the
 * mouse. Frame runs one UI frame as the engine does: each grid reads its list property
 * (CollectUIViews), then the slots it shows register as UI Elements. */
class GridInventoryRig {
 public:
  struct Element {
    KX_GameObject *object = nullptr;
    LN_UIService::ElementSettings settings;
  };

  GridInventoryRig() : manager(*scene), service(manager.GetUIService()), camera(scene), panel(scene)
  {
    panel_settings.focus = LN_UIElementFocus::None;
  }

  /** A grid of `slots` showing `property` of `owner`, with Handle Moves on. */
  LN_UIService::ViewRegistration &AddGrid(const uint32_t query,
                                          const std::vector<KX_GameObject *> &slots,
                                          KX_GameObject &owner,
                                          const char *property,
                                          const char *action)
  {
    LN_UIService::ViewRegistration &view = views.emplace_back();
    view.listener = {tree, query};
    view.kind = LN_UIService::ViewKind::Grid;
    view.viewport = &panel.object;
    view.camera = &camera.object;
    view.bounds_min = MT_Vector3(-30.0f, -3.0f, 0.0f);
    view.bounds_max = MT_Vector3(30.0f, 3.0f, 0.0f);
    view.slots = slots;
    view.columns = int32_t(slots.size());
    view.cull_outside = false;
    view.show_empty = true;
    view.list_object = &owner;
    view.list_property = property;
    view.inventory.handle_moves = true;
    LN_UIService::ElementSettings settings = MakeElementSettings(action);
    settings.draggable = true;
    settings.drop_target = true;
    settings.drag_threshold = 1.0f;
    for (KX_GameObject *slot : slots) {
      elements.push_back({slot, settings});
    }
    return view;
  }

  LN_UIService::ElementSettings &SettingsOf(KX_GameObject &object)
  {
    for (Element &element : elements) {
      if (element.object == &object) {
        return element.settings;
      }
    }
    return elements.front().settings;
  }

  LN_UIService::Registration ElementAt(KX_GameObject &object,
                                       const MT_Vector3 &pointer,
                                       const float half_size)
  {
    LN_UIService::Registration registration;
    registration.object = &object;
    registration.camera = &camera.object;
    registration.ray_from = pointer + MT_Vector3(0.0f, 0.0f, 5.0f);
    registration.ray_to = pointer - MT_Vector3(0.0f, 0.0f, 5.0f);
    registration.world_position = object.NodeGetWorldPosition();
    registration.bounds_min = MT_Vector3(-half_size, -half_size, 0.0f);
    registration.bounds_max = MT_Vector3(half_size, half_size, 0.0f);
    registration.interaction_domain = LN_InteractionDomain::Overlay;
    return registration;
  }

  /** One UI frame; `key` is pressed in it and let go in the next. */
  void Frame(const MT_Vector3 &pointer,
             const int button = -1,
             const bool back = false,
             const SCA_IInputDevice::SCA_EnumInputs key = SCA_IInputDevice::NOKEY)
  {
    device.ClearInputs();
    device.ConvertMoveEvent(100 + int(pointer.x() * 20.0f), 100);
    if (button >= 0) {
      device.ConvertEvent(SCA_IInputDevice::LEFTMOUSE, button, 0);
    }
    if (back) {
      device.ConvertEvent(SCA_IInputDevice::ESCKEY, 1, 0);
    }
    if (pressed_key != SCA_IInputDevice::NOKEY) {
      device.ConvertEvent(pressed_key, 0, 0);
    }
    pressed_key = key;
    if (key != SCA_IInputDevice::NOKEY) {
      device.ConvertEvent(key, 1, 0);
    }
    input.Capture(&device, nullptr);
    service.BeginTick(LN_Event::OnUpdate, ++index, &input);
    for (LN_UIService::ViewRegistration view : views) {
      /* Shared references, as the grid's tree gives its items (ShareValue). */
      view.items =
          LN_GamePropertyValue(view.list_object->GetProperty(view.list_property), &manager)
              .list_value;
      view.ray_from = pointer + MT_Vector3(0.0f, 0.0f, 5.0f);
      view.ray_to = pointer - MT_Vector3(0.0f, 0.0f, 5.0f);
      service.RegisterView(view);
    }
    service.ResolveViews();
    service.RegisterElement(&panel.object, panel_settings, ElementAt(panel.object, pointer, 30.0f));
    for (const Element &element : elements) {
      if (element.object->GetVisible() && service.ObjectPassesViewClipping(element.object)) {
        service.RegisterElement(
            element.object, element.settings, ElementAt(*element.object, pointer, 0.5f));
      }
    }
    service.ResolveInteractions();
    service.UpdateElementPresentation(1.0 / 60.0);
  }

  /** Presses on `from` and drags to `to` (the last frame is over it, still held). */
  void DragOver(const MT_Vector3 &from, const MT_Vector3 &to)
  {
    Frame(from, 1);
    Frame(from + MT_Vector3(0.3f, 0.0f, 0.0f));
    Frame(to);
  }

  /** Drags `from` onto `to` and lets go there. */
  void Drop(const MT_Vector3 &from, const MT_Vector3 &to)
  {
    DragOver(from, to);
    Frame(to, 0);
  }

  std::string Outcome(const LN_UIElementEvent type, const char *action)
  {
    const LN_UIService::ElementEvent *event = service.FindElementEvent(type, action);
    return event != nullptr ? event->outcome : std::string("(no event)");
  }

  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager;
  LN_UIService &service;
  SceneBoundObject<KX_Camera> camera;
  SceneBoundObject<KX_GameObject> panel;
  const LN_RuntimeTree *tree = reinterpret_cast<LN_RuntimeTree *>(uintptr_t(2));
  std::deque<LN_UIService::ViewRegistration> views;
  std::deque<Element> elements;
  LN_UIService::ElementSettings panel_settings = MakeElementSettings("");
  DEV_InputDevice device;
  LN_InputSnapshot input;
  uint64_t index = 0;
  SCA_IInputDevice::SCA_EnumInputs pressed_key = SCA_IInputDevice::NOKEY;
};

void PlaceAt(SceneBoundObject<KX_GameObject> &object, const float x)
{
  object.object.NodeSetLocalPosition(MT_Vector3(x, 0.0f, 0.05f));
  object.node.UpdateWorldData(0.0);
}

}  // namespace

TEST(LN_UIService, GridHandleMovesStacksSwapsAndCombinesInItsOwnList)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  SceneBoundObject<KX_GameObject> slot0(rig.scene);
  SceneBoundObject<KX_GameObject> slot1(rig.scene);
  SceneBoundObject<KX_GameObject> slot2(rig.scene);
  PlaceAt(slot0, 0.0f);
  PlaceAt(slot1, 2.0f);
  PlaceAt(slot2, 4.0f);
  const MT_Vector3 at0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at1(2.0f, 0.0f, 0.0f);
  const MT_Vector3 at2(4.0f, 0.0f, 0.0f);
  SetItemList(owner.object, "inventory", {"Apple", "Gem", "Apple"});
  rig.AddGrid(70, {&slot0.object, &slot1.object, &slot2.object}, owner.object, "inventory", "bag");
  rig.Frame(at2);

  /* The same item joins the stack it is dropped on; both events say so. */
  rig.Drop(at2, at0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Stacked");
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::Drop, "bag"), "Stacked");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Apple x2", "Gem"}));

  /* Two different items change places. */
  rig.Frame(at1);
  rig.Drop(at1, at0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Swapped");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Gem", "Apple x2"}));

  /* A recipe of the list's object makes its result where the dragged item was. */
  SetItemList(owner.object, "recipes", {"Apple + Gem = Elixir"});
  rig.views[0].rules.recipes_property = "recipes";
  rig.Frame(at0);
  rig.Drop(at0, at1);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Combined");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Elixir", "Apple"}));

  /* An empty cell past the end: the stack goes to the end. */
  rig.Frame(at0);
  rig.Drop(at0, at2);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Apple", "Elixir"}));

  /* Without Handle Moves the drop is left to game logic. */
  rig.views[0].inventory.handle_moves = false;
  rig.Frame(at0);
  rig.Drop(at0, at1);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Apple", "Elixir"}));
}

TEST(LN_UIService, GridHandleMovesMovesItemsBetweenTheListsOfTwoObjects)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> chest(rig.scene);
  SceneBoundObject<KX_GameObject> bag0(rig.scene);
  SceneBoundObject<KX_GameObject> bag1(rig.scene);
  SceneBoundObject<KX_GameObject> chest0(rig.scene);
  SceneBoundObject<KX_GameObject> chest1(rig.scene);
  PlaceAt(bag0, 0.0f);
  PlaceAt(bag1, 2.0f);
  PlaceAt(chest0, 10.0f);
  PlaceAt(chest1, 12.0f);
  const MT_Vector3 at_bag0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at_chest0(10.0f, 0.0f, 0.0f);
  const MT_Vector3 at_chest1(12.0f, 0.0f, 0.0f);
  SetItemList(player.object, "inventory", {"Apple", "Gem"});
  rig.AddGrid(71, {&bag0.object, &bag1.object}, player.object, "inventory", "bag");
  rig.AddGrid(72, {&chest0.object, &chest1.object}, chest.object, "chest", "chest");
  rig.Frame(at_bag0);

  /* Into the chest's empty cell: a chest property is made for it. */
  rig.Drop(at_bag0, at_chest0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "chest"), "Moved");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (std::vector<std::string>{"Gem"}));
  EXPECT_EQ(ListedItems(chest.object, "chest"), (std::vector<std::string>{"Apple"}));

  /* Back onto the bag's other item: the two change places across the lists. */
  rig.Frame(at_chest0);
  rig.Drop(at_chest0, at_bag0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Swapped");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (std::vector<std::string>{"Apple"}));
  EXPECT_EQ(ListedItems(chest.object, "chest"), (std::vector<std::string>{"Gem"}));

  /* A full chest has no room: nothing changes, and the drop sounds Denied. */
  rig.views[1].rules.capacity = 1;
  rig.Frame(at_bag0);
  rig.Drop(at_bag0, at_chest1);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "chest"), "No Room");
  EXPECT_EQ(FrameSound(rig.service), LN_UIService::UISound::Denied);
  EXPECT_EQ(ListedItems(player.object, "inventory"), (std::vector<std::string>{"Apple"}));
  EXPECT_EQ(ListedItems(chest.object, "chest"), (std::vector<std::string>{"Gem"}));

  /* Both grids must handle moves: else game logic moves it (and it is moved only once). */
  rig.views[1].rules.capacity = 0;
  rig.views[1].inventory.handle_moves = false;
  rig.Frame(at_bag0);
  rig.Drop(at_bag0, at_chest1);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "chest"), "");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (std::vector<std::string>{"Apple"}));
  EXPECT_EQ(ListedItems(chest.object, "chest"), (std::vector<std::string>{"Gem"}));
}

TEST(LN_UIService, GridHandleMovesUsesMaxStackAndSoundsDeniedWhenNothingChanges)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  SceneBoundObject<KX_GameObject> slot0(rig.scene);
  SceneBoundObject<KX_GameObject> slot1(rig.scene);
  PlaceAt(slot0, 0.0f);
  PlaceAt(slot1, 2.0f);
  const MT_Vector3 at0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at1(2.0f, 0.0f, 0.0f);
  LN_Value list;
  list.type = LN_ValueType::List;
  list.exists = true;
  list.list_value = {LN_MakeInventoryEntry("Arrow", 3), LN_MakeInventoryEntry("Arrow", 2)};
  std::string reason;
  ASSERT_TRUE(
      LN_CommandBuffer::ApplyGamePropertyValue(&owner.object, "inventory", list, nullptr, reason));
  LN_UIService::ViewRegistration &view = rig.AddGrid(
      76, {&slot0.object, &slot1.object}, owner.object, "inventory", "bag");
  view.rules.max_stack = 4;
  rig.Frame(at1);

  /* No max_stack property in the Item Library: the list's Max Stack (its Inventory Rules). */
  rig.Drop(at1, at0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Stacked");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Arrow x4", "Arrow"}));
  /* A full stack takes no more: nothing changes, and the drop sounds Denied. */
  rig.Frame(at1);
  rig.Drop(at1, at0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "");
  EXPECT_EQ(FrameSound(rig.service), LN_UIService::UISound::Denied);
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Arrow x4", "Arrow"}));
}

/* A list that holds objects is read as the grid's own items hold them (shared references, the very
 * object even among copies with one name), so its rows are found and written back exactly. */
TEST(LN_UIService, GridListsReadTheirObjectsAsTheGridsOwnReferences)
{
  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  SceneBoundObject<KX_GameObject> torch(scene);
  SceneBoundObject<KX_GameObject> owner(scene);
  torch.object.SetName("Torch");
  EXP_DictValue *row = new EXP_DictValue();
  row->Set("item", new EXP_StringValue("Torch", ""));
  row->Set("owner", new KX_ObjectReferenceValue(&torch.object));
  EXP_ArrayValue *rows = new EXP_ArrayValue();
  rows->Append(row);
  owner.object.SetProperty("inventory", rows);
  rows->Release();

  const LN_Value list = LN_GamePropertyValue(owner.object.GetProperty("inventory"), &manager);
  ASSERT_EQ(list.type, LN_ValueType::List);
  ASSERT_EQ(list.list_value.size(), 1u);
  const auto cell = list.list_value[0].dict_value.find("owner");
  ASSERT_NE(cell, list.list_value[0].dict_value.end());
  EXPECT_EQ(cell->second.type, LN_ValueType::ObjectRef);
  EXPECT_EQ(cell->second.reference_name, "Torch");
  const LN_RuntimeRef shared = manager.MakeSharedObjectRef(&torch.object, "Torch");
  EXPECT_EQ(cell->second.runtime_ref.slot, shared.slot);
  EXPECT_EQ(cell->second.runtime_ref.generation, shared.generation);
  EXPECT_EQ(cell->second.runtime_ref.shared_domain, shared.shared_domain);
  EXPECT_NE(cell->second.runtime_ref.shared_domain, 0u);
  EXPECT_EQ(manager.ResolveSharedObjectRef(cell->second.runtime_ref), &torch.object);
  /* Without a manager: by name only. */
  const LN_Value by_name = LN_GamePropertyValue(owner.object.GetProperty("inventory"), nullptr);
  EXPECT_EQ(by_name.list_value[0].dict_value.at("owner").runtime_ref.shared_domain, 0u);
  owner.object.RemoveProperty("inventory");
}

TEST(LN_UIService, GridDropHintsAndRefusedObjectsShowWhatADropWouldDo)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> apple(rig.scene);
  SceneBoundObject<KX_GameObject> hint(rig.scene);
  SceneBoundObject<KX_GameObject> bag0(rig.scene);
  SceneBoundObject<KX_GameObject> bag1(rig.scene);
  SceneBoundObject<KX_GameObject> weapon(rig.scene);
  SceneBoundObject<KX_GameObject> weapon_refused(rig.scene);
  PlaceAt(bag0, 0.0f);
  PlaceAt(bag1, 2.0f);
  PlaceAt(weapon, 6.0f);
  weapon.node.AddChild(&weapon_refused.node);
  weapon_refused.node.SetParentRelation(new KX_NormalParentRelation());
  weapon_refused.object.SetName("WeaponRefused");
  weapon.object.NodeUpdateGS(0.0);
  blender::Object refused_source = {};
  std::snprintf(refused_source.id.name, sizeof(refused_source.id.name), "OBWeaponRefused");
  /* The Item Library's Apple: the name players see, and its category. */
  SetTextProperty(apple.object, "name", "Red Apple");
  SetTextProperty(apple.object, "category", "food");
  rig.service.SetItemLibraryForTests(
      [&](const std::string &item) { return item == "Apple" ? &apple.object : nullptr; });
  SetItemList(player.object, "inventory", {"Apple", "Gem"});
  rig.AddGrid(73, {&bag0.object, &bag1.object}, player.object, "inventory", "bag");
  rig.AddGrid(74, {&weapon.object}, player.object, "weapon", "weapon");
  for (LN_UIService::ViewRegistration &view : rig.views) {
    view.inventory.hint_text = &hint.object;
    view.inventory.accept_hint = "Put {name} here";
    view.inventory.refuse_hint = "{name} does not fit here";
  }
  rig.SettingsOf(weapon.object).accept_categories = ",weapon,";
  rig.SettingsOf(weapon.object).refused = &refused_source;
  const MT_Vector3 at_bag0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at_bag1(2.0f, 0.0f, 0.0f);
  const MT_Vector3 at_weapon(6.0f, 0.0f, 0.0f);

  rig.Frame(at_bag0);
  EXPECT_EQ(TextProperty(hint.object), "(none)");
  EXPECT_FALSE(weapon_refused.object.GetVisible());
  /* Over a slot that takes it. */
  rig.DragOver(at_bag0, at_bag1);
  EXPECT_EQ(TextProperty(hint.object), "Put Red Apple here");
  /* Over one whose Accept Categories refuse it: the other text, and its Refused Object. */
  rig.Frame(at_weapon);
  EXPECT_EQ(TextProperty(hint.object), "Red Apple does not fit here");
  EXPECT_TRUE(weapon_refused.object.GetVisible());
  /* Back cancels the drag: the hint empties and the frame hides. */
  rig.Frame(at_weapon, -1, true);
  EXPECT_NE(rig.service.FindElementEvent(LN_UIElementEvent::DragCancel, "bag"), nullptr);
  EXPECT_EQ(TextProperty(hint.object), "");
  EXPECT_FALSE(weapon_refused.object.GetVisible());
  /* A shared info line: what game logic wrote over the hint stays when the drag ends. */
  rig.Frame(at_bag0, 0);
  rig.DragOver(at_bag0, at_bag1);
  EXPECT_EQ(TextProperty(hint.object), "Put Red Apple here");
  SetTextProperty(hint.object, "Text", "Crafted: Elixir");
  rig.Frame(at_bag1, -1, true);
  EXPECT_EQ(TextProperty(hint.object), "Crafted: Elixir");
  weapon.node.RemoveChild(&weapon_refused.node);
}

TEST(LN_UIService, GridDropIntoWorldTakesOnlyThePlayersViewAndDropsInFrontByDefault)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  SceneBoundObject<KX_GameObject> ground(rig.scene);
  SceneBoundObject<KX_GameObject> slot0(rig.scene);
  SceneBoundObject<KX_GameObject> slot1(rig.scene);
  PlaceAt(slot0, 0.0f);
  PlaceAt(slot1, 2.0f);
  const MT_Vector3 at0(0.0f, 0.0f, 0.0f);
  SetItemList(owner.object, "inventory", {"Apple", "Gem"});
  LN_UIService::ViewRegistration &grid = rig.AddGrid(
      71, {&slot0.object, &slot1.object}, owner.object, "inventory", "bag");
  grid.inventory.drop_into_world = true;
  /* No Item Library object and an empty physics world: nothing really drops, the events still
   * say what the stack was let go over. */
  rig.service.SetItemLibraryForTests([](const std::string &) -> KX_GameObject * { return nullptr; });
  rig.service.SetWorldRayForTests(
      [](const MT_Vector3 &, const MT_Vector3 &, const std::vector<KX_GameObject *> &, float &)
          -> KX_GameObject * { return nullptr; });
  /* Outside the panel, by pointer pixel (the rig puts x at 100 + 20 x): a minimap drawn over the
   * player's view, the player's view showing only sky, then the ground. */
  const MT_Vector3 over_minimap(40.0f, 0.0f, 0.0f);
  const MT_Vector3 over_sky(50.0f, 0.0f, 0.0f);
  const MT_Vector3 over_ground(60.0f, 0.0f, 0.0f);
  rig.service.SetWorldDropForTests([&](const float pointer_x, const float /*pointer_y*/) {
    LN_UIService::WorldDropHit hit;
    if (pointer_x >= 1000.0f) {
      hit.over = LN_UIService::WorldDrop::View;
    }
    if (pointer_x >= 1200.0f) {
      hit.over = LN_UIService::WorldDrop::Object;
      hit.object = &ground.object;
      hit.point = MT_Vector3(9.0f, 9.0f, 0.0f);
    }
    return hit;
  });
  const auto let_go_over = [&](const MT_Vector3 &where) {
    rig.Frame(at0);
    rig.Drop(at0, where);
  };
  const auto fired = [&](const LN_UIElementEvent type) {
    return rig.service.FindElementEvent(type, "bag") != nullptr;
  };

  /* A minimap shows the world, but letting go there puts the stack back. */
  let_go_over(over_minimap);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel));
  EXPECT_FALSE(fired(LN_UIElementEvent::DropOutside));

  /* In front of the list's object (the default): anywhere over the player's view drops. */
  let_go_over(over_sky);
  EXPECT_TRUE(fired(LN_UIElementEvent::DropOutside));
  EXPECT_FALSE(fired(LN_UIElementEvent::DragCancel));
  let_go_over(over_ground);
  const LN_UIService::ElementEvent *outside = rig.service.FindElementEvent(
      LN_UIElementEvent::DropOutside, "bag");
  ASSERT_NE(outside, nullptr);
  EXPECT_EQ(rig.manager.ResolveObjectHandle(outside->target), &ground.object);

  /* At Pointer: the stack goes where the pointer is, so the sky has no place for it. */
  grid.inventory.drop_at_pointer = true;
  let_go_over(over_sky);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel));
  EXPECT_FALSE(fired(LN_UIElementEvent::DropOutside));
  let_go_over(over_ground);
  EXPECT_TRUE(fired(LN_UIElementEvent::DropOutside));
  let_go_over(over_minimap);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel));

  /* Without Drop Into World only something under the pointer is a Drop Outside, for routes. */
  grid.inventory.drop_into_world = false;
  grid.inventory.drop_at_pointer = false;
  let_go_over(over_sky);
  EXPECT_TRUE(fired(LN_UIElementEvent::DragCancel));
  EXPECT_EQ(ListedItems(owner.object, "inventory"), (std::vector<std::string>{"Apple", "Gem"}));
}

TEST(LN_UIService, GridSplitActionTakesPartOfAStack)
{
  /* The game's Split action: Left Ctrl. */
  {
    blender::GameData game;
    blender::BKE_game_input_map_ensure_builtins(&game);
    blender::GameInputAction *split = blender::BKE_game_input_action_add(
        &game, "Split", GAME_INPUT_ACTION_BUTTON);
    blender::BKE_game_input_binding_add(split, GAME_INPUT_KEYBOARD)->code =
        blender::EVT_LEFTCTRLKEY;
    LN_InputMap::Get().Load(&game);
    blender::BKE_game_input_map_free(&game);
  }
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  SceneBoundObject<KX_GameObject> chest(rig.scene);
  std::array<SceneBoundObject<KX_GameObject>, 3> bag_slots = {
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene)};
  std::array<SceneBoundObject<KX_GameObject>, 2> chest_slots = {
      SceneBoundObject<KX_GameObject>(rig.scene), SceneBoundObject<KX_GameObject>(rig.scene)};
  for (size_t slot = 0; slot < bag_slots.size(); ++slot) {
    PlaceAt(bag_slots[slot], 2.0f * float(slot));
  }
  PlaceAt(chest_slots[0], 10.0f);
  PlaceAt(chest_slots[1], 12.0f);
  const MT_Vector3 at0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at1(2.0f, 0.0f, 0.0f);
  const MT_Vector3 at2(4.0f, 0.0f, 0.0f);
  const MT_Vector3 at_chest0(10.0f, 0.0f, 0.0f);
  LN_Value bag;
  bag.type = LN_ValueType::List;
  bag.exists = true;
  bag.list_value = {LN_MakeInventoryEntry("Apple", 5), LN_MakeInventoryEntry("Gem", 1)};
  std::string reason;
  ASSERT_TRUE(
      LN_CommandBuffer::ApplyGamePropertyValue(&owner.object, "inventory", bag, nullptr, reason));
  SetItemList(chest.object, "chest", {});
  LN_UIService::ViewRegistration &bag_grid = rig.AddGrid(
      72,
      {&bag_slots[0].object, &bag_slots[1].object, &bag_slots[2].object},
      owner.object,
      "inventory",
      "bag");
  bag_grid.inventory.split_action = "Split";
  rig.AddGrid(73, {&chest_slots[0].object, &chest_slots[1].object}, chest.object, "chest", "chest");
  /* The copy a split drag leaves in the cell with the rest of the stack. */
  std::deque<SceneBoundObject<KX_GameObject>> copies;
  std::vector<MT_Vector3> copied_at;
  int removed = 0;
  rig.service.SetDragGhostForTests(
      [&](KX_GameObject *slot) {
        copied_at.push_back(slot->NodeGetWorldPosition());
        return &copies.emplace_back(rig.scene).object;
      },
      [&](KX_GameObject * /*copy*/) { removed++; });
  const auto hold_ctrl = [&](const bool held) {
    rig.device.ConvertEvent(SCA_IInputDevice::LEFTCTRLKEY, held ? 1 : 0, 0);
  };
  const auto amount_of = [&](const LN_UIElementEvent type, const char *action) {
    const LN_UIService::ElementEvent *event = rig.service.FindElementEvent(type, action);
    return event != nullptr ? event->amount : -1;
  };
  rig.Frame(at0);

  /* Ctrl held as the drag starts: half of the five (rounded up) goes to the empty cell. */
  hold_ctrl(true);
  rig.DragOver(at0, at2);
  EXPECT_EQ(amount_of(LN_UIElementEvent::Drag, "bag"), 3);
  /* One copy, made where the slot was before it followed the pointer; it stays there. */
  ASSERT_EQ(copies.size(), size_t(1));
  EXPECT_NEAR(copied_at[0].x(), 0.0f, 1.0e-4f);
  EXPECT_NEAR(copies[0].object.NodeGetWorldPosition().x(), 0.0f, 1.0e-4f);
  EXPECT_GT(bag_slots[0].object.NodeGetWorldPosition().x(), 3.0f);
  EXPECT_EQ(removed, 0);
  /* The copy takes the slot's color as it changes (a highlight action fading out on the slot),
   * not the color the slot had when the copy was made. */
  bag_slots[0].object.SetObjectColor(MT_Vector4(1.0f, 0.5f, 0.25f, 0.0f));
  rig.Frame(at2);
  EXPECT_FLOAT_EQ(copies[0].object.GetObjectColor()[1], 0.5f);
  EXPECT_FLOAT_EQ(copies[0].object.GetObjectColor()[3], 0.0f);
  EXPECT_EQ(removed, 0);
  rig.Frame(at2, 0);
  /* Dropped: the copy goes the same frame. */
  EXPECT_EQ(removed, 1);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(amount_of(LN_UIElementEvent::DropReceived, "bag"), 3);
  EXPECT_EQ(amount_of(LN_UIElementEvent::Drop, "bag"), 3);
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Apple x2", "Gem", "Apple x3"}));

  /* Without Ctrl the whole stack goes: the two Apple stacks join again. */
  hold_ctrl(false);
  rig.Frame(at2);
  rig.Drop(at2, at0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Stacked");
  EXPECT_EQ(amount_of(LN_UIElementEvent::Drop, "bag"), 3);
  EXPECT_EQ(ListedItems(owner.object, "inventory"), (std::vector<std::string>{"Apple x5", "Gem"}));
  /* A whole stack leaves no copy. */
  EXPECT_EQ(copies.size(), size_t(1));

  /* Split Amount 1: one Apple. Onto another item a part never swaps: it takes the nearest free
   * place, as Split Stack (also when no empty cell shows, a bag whose pages are full). */
  bag_grid.inventory.split_amount = 1;
  hold_ctrl(true);
  rig.Frame(at0);
  rig.Drop(at0, at1);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Apple x4", "Apple", "Gem"}));
  /* Into the chest: one Apple, the three stay in the bag. */
  rig.Frame(at0);
  rig.Drop(at0, at_chest0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "chest"), "Moved");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Apple x3", "Apple", "Gem"}));
  EXPECT_EQ(ListedItems(chest.object, "chest"), (std::vector<std::string>{"Apple"}));

  /* A grid without a Split Action moves whole stacks, Ctrl or not. */
  bag_grid.inventory.split_action.clear();
  rig.Frame(at0);
  rig.Drop(at0, at_chest0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "chest"), "Stacked");
  EXPECT_EQ(ListedItems(owner.object, "inventory"), (std::vector<std::string>{"Apple", "Gem"}));
  EXPECT_EQ(ListedItems(chest.object, "chest"), (std::vector<std::string>{"Apple x4"}));
  /* Every copy was taken away again. */
  EXPECT_EQ(removed, int(copies.size()));
  hold_ctrl(false);
  LN_InputMap::Get().Load(nullptr);
}

TEST(LN_UIService, GridItemOnlyDragLeavesTheSlotInItsCell)
{
  /* The game's Split action: Left Ctrl. */
  {
    blender::GameData game;
    blender::BKE_game_input_map_ensure_builtins(&game);
    blender::GameInputAction *split = blender::BKE_game_input_action_add(
        &game, "Split", GAME_INPUT_ACTION_BUTTON);
    blender::BKE_game_input_binding_add(split, GAME_INPUT_KEYBOARD)->code =
        blender::EVT_LEFTCTRLKEY;
    LN_InputMap::Get().Load(&game);
    blender::BKE_game_input_map_free(&game);
  }
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  std::array<SceneBoundObject<KX_GameObject>, 3> slots = {
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene)};
  for (size_t slot = 0; slot < slots.size(); ++slot) {
    PlaceAt(slots[slot], 2.0f * float(slot));
  }
  const MT_Vector3 at0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at2(4.0f, 0.0f, 0.0f);
  LN_Value bag;
  bag.type = LN_ValueType::List;
  bag.exists = true;
  bag.list_value = {LN_MakeInventoryEntry("Apple", 5), LN_MakeInventoryEntry("Gem", 1)};
  std::string reason;
  ASSERT_TRUE(
      LN_CommandBuffer::ApplyGamePropertyValue(&owner.object, "inventory", bag, nullptr, reason));
  LN_UIService::ViewRegistration &grid = rig.AddGrid(
      78, {&slots[0].object, &slots[1].object, &slots[2].object}, owner.object, "inventory", "bag");
  grid.inventory.split_action = "Split";
  for (SceneBoundObject<KX_GameObject> &slot : slots) {
    LN_UIService::ElementSettings &settings = rig.SettingsOf(slot.object);
    settings.drag_item_only = true;
    settings.hover_scale = 1.5f;
    settings.look_time = 0.0f;
  }
  /* The item's copy that follows the pointer. */
  std::deque<SceneBoundObject<KX_GameObject>> copies;
  int removed = 0;
  rig.service.SetDragGhostForTests(
      [&](KX_GameObject * /*slot*/) { return &copies.emplace_back(rig.scene).object; },
      [&](KX_GameObject * /*copy*/) { removed++; });
  /* Let go over no drop target: no game world there (the menu covers it). */
  rig.service.SetWorldDropForTests(
      [](const float /*pointer_x*/, const float /*pointer_y*/) {
        return LN_UIService::WorldDropHit();
      });
  const auto amount_of = [&](const LN_UIElementEvent type) {
    const LN_UIService::ElementEvent *event = rig.service.FindElementEvent(type, "bag");
    return event != nullptr ? event->amount : -1;
  };
  rig.Frame(at0);

  /* The whole stack: the slot stays in its cell, the copy follows, lifted at the slot's hover
   * size while the slot goes back to its own. */
  rig.DragOver(at0, at2);
  EXPECT_EQ(amount_of(LN_UIElementEvent::Drag), 5);
  ASSERT_EQ(copies.size(), size_t(1));
  EXPECT_NEAR(slots[0].object.NodeGetWorldPosition().x(), 0.0f, 1.0e-4f);
  EXPECT_GT(copies[0].object.NodeGetWorldPosition().x(), 3.0f);
  EXPECT_NEAR(slots[0].object.NodeGetLocalScaling().x(), 1.0f, 1.0e-4f);
  EXPECT_NEAR(copies[0].object.NodeGetWorldScaling().x(), 1.5f, 1.0e-4f);
  /* Let go on the empty cell: moved; the copy stays this frame, where it was let go, and goes
   * the next, when the grid shows the item in its new cell. */
  rig.Frame(at2, 0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(removed, 0);
  EXPECT_GT(copies[0].object.NodeGetWorldPosition().x(), 3.0f);
  rig.Frame(at2);
  EXPECT_EQ(removed, 1);
  EXPECT_NEAR(slots[0].object.NodeGetWorldPosition().x(), 0.0f, 1.0e-4f);
  EXPECT_EQ(ListedItems(owner.object, "inventory"), (std::vector<std::string>{"Gem", "Apple x5"}));

  /* Let go over its own cell: nothing changes, the copy goes the same way. */
  const MT_Vector3 at1(2.0f, 0.0f, 0.0f);
  rig.Frame(at1);
  rig.DragOver(at1, at1 + MT_Vector3(0.1f, 0.0f, 0.0f));
  ASSERT_EQ(copies.size(), size_t(2));
  rig.Frame(at1, 0);
  rig.Frame(at1);
  EXPECT_EQ(removed, 2);
  EXPECT_EQ(ListedItems(owner.object, "inventory"), (std::vector<std::string>{"Gem", "Apple x5"}));

  /* A split stack: the copy carries the part, the rest stays in the slot's cell. */
  rig.device.ConvertEvent(SCA_IInputDevice::LEFTCTRLKEY, 1, 0);
  rig.Frame(at1);
  rig.Drop(at1, at2);
  EXPECT_EQ(amount_of(LN_UIElementEvent::DropReceived), 3);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(ListedItems(owner.object, "inventory"),
            (std::vector<std::string>{"Gem", "Apple x2", "Apple x3"}));
  EXPECT_EQ(copies.size(), size_t(3));
  rig.Frame(at2);
  EXPECT_EQ(removed, 3);
  EXPECT_NEAR(slots[1].object.NodeGetWorldPosition().x(), 2.0f, 1.0e-4f);
  rig.device.ConvertEvent(SCA_IInputDevice::LEFTCTRLKEY, 0, 0);
  /* (Whole Element, where the slot follows: GridSplitActionTakesPartOfAStack.) */
  LN_InputMap::Get().Load(nullptr);
}

TEST(LN_UIService, HighlightAndRefusedStartAndEndOncePerChange)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> apple(rig.scene);
  SceneBoundObject<KX_GameObject> bag0(rig.scene);
  SceneBoundObject<KX_GameObject> bag1(rig.scene);
  SceneBoundObject<KX_GameObject> weapon(rig.scene);
  PlaceAt(bag0, 0.0f);
  PlaceAt(bag1, 2.0f);
  PlaceAt(weapon, 6.0f);
  SetTextProperty(apple.object, "category", "food");
  rig.service.SetItemLibraryForTests(
      [&](const std::string &item) { return item == "Apple" ? &apple.object : nullptr; });
  SetItemList(player.object, "inventory", {"Apple", "Gem"});
  rig.AddGrid(75, {&bag0.object, &bag1.object}, player.object, "inventory", "bag");
  rig.AddGrid(76, {&weapon.object}, player.object, "weapon", "weapon");
  /* No Highlight Object nor Refused Object: the events need none. */
  rig.SettingsOf(weapon.object).accept_categories = ",weapon,";
  const MT_Vector3 at_bag0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at_bag1(2.0f, 0.0f, 0.0f);
  const MT_Vector3 at_weapon(6.0f, 0.0f, 0.0f);
  const MT_Vector3 away(20.0f, 0.0f, 0.0f);
  const auto events_of = [&](const LN_UIElementEvent type) {
    std::vector<KX_GameObject *> objects;
    size_t count = 0;
    rig.service.FindElementEvent(type, std::string(), -1, 0, &count);
    for (size_t index = 0; index < count; ++index) {
      const LN_UIService::ElementEvent *event = rig.service.FindElementEvent(
          type, std::string(), -1, index);
      objects.push_back(event != nullptr ? rig.manager.ResolveObjectHandle(event->object) :
                                           nullptr);
    }
    return objects;
  };
  using Objects = std::vector<KX_GameObject *>;

  rig.Frame(away);
  rig.Frame(at_bag0);
  EXPECT_EQ(events_of(LN_UIElementEvent::HighlightStart), (Objects{&bag0.object}));
  EXPECT_EQ(events_of(LN_UIElementEvent::HighlightEnd), Objects{});
  /* Staying on it tells nothing again. */
  rig.Frame(at_bag0);
  EXPECT_EQ(events_of(LN_UIElementEvent::HighlightStart), Objects{});
  /* Moving to another element: the old one ends, the new one starts, in the same frame. */
  rig.Frame(at_bag1);
  EXPECT_EQ(events_of(LN_UIElementEvent::HighlightEnd), (Objects{&bag0.object}));
  EXPECT_EQ(events_of(LN_UIElementEvent::HighlightStart), (Objects{&bag1.object}));
  /* One On UI Event for several Actions: names separated by commas, spaces ignored. */
  EXPECT_NE(rig.service.FindElementEvent(LN_UIElementEvent::HighlightStart, "weapon, bag"),
            nullptr);
  EXPECT_NE(rig.service.FindElementEvent(LN_UIElementEvent::HighlightEnd, " bag ,weapon"),
            nullptr);
  EXPECT_EQ(rig.service.FindElementEvent(LN_UIElementEvent::HighlightStart, "weapon, armor"),
            nullptr);
  EXPECT_EQ(rig.service.FindElementEvent(LN_UIElementEvent::HighlightStart, "ba, g"), nullptr);

  /* Dragging the Apple over the weapon slot that refuses it: Refused Start there (not
   * highlighted, as its Highlight Object would not be), and Refused End when the drag leaves. */
  rig.Frame(at_bag0);
  rig.DragOver(at_bag0, at_weapon);
  EXPECT_EQ(events_of(LN_UIElementEvent::RefusedStart), (Objects{&weapon.object}));
  const LN_UIService::ElementEvent *refused = rig.service.FindElementEvent(
      LN_UIElementEvent::RefusedStart, "weapon");
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(rig.manager.ResolveObjectHandle(refused->target), &bag0.object);
  EXPECT_EQ(refused->target_action, "bag");
  EXPECT_EQ(events_of(LN_UIElementEvent::HighlightStart), Objects{});
  rig.Frame(at_bag1);
  EXPECT_EQ(events_of(LN_UIElementEvent::RefusedEnd), (Objects{&weapon.object}));
  /* Let go: the drag ends; nothing is left highlighted once the pointer is away. */
  rig.Frame(at_bag1, 0);
  rig.Frame(away);
  rig.Frame(away);
  EXPECT_EQ(events_of(LN_UIElementEvent::HighlightStart), Objects{});
  EXPECT_EQ(events_of(LN_UIElementEvent::RefusedStart), Objects{});
}

TEST(LN_UIService, GridKeepPlacesLetsPlayersLeaveGaps)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  std::array<SceneBoundObject<KX_GameObject>, 3> slots = {
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene)};
  for (size_t slot = 0; slot < slots.size(); ++slot) {
    PlaceAt(slots[slot], 2.0f * float(slot));
  }
  const MT_Vector3 at0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at1(2.0f, 0.0f, 0.0f);
  const MT_Vector3 at2(4.0f, 0.0f, 0.0f);
  SetItemList(owner.object, "inventory", {"Apple", "Gem"});
  LN_UIService::ViewRegistration &grid = rig.AddGrid(
      74, {&slots[0].object, &slots[1].object, &slots[2].object}, owner.object, "inventory", "bag");
  grid.rules.keep_places = true;
  const auto places = [&]() {
    std::vector<std::string> names;
    for (const LN_Value &slot :
         LN_GamePropertyValue(owner.object.GetProperty("inventory"), nullptr).list_value)
    {
      std::string item;
      int32_t count = 0;
      names.push_back(LN_InventoryEntry(slot, item, count) ? item : std::string("-"));
    }
    return names;
  };
  rig.Frame(at0);

  /* Onto the empty cell past the end: it stays there, a gap where it was. */
  rig.Drop(at0, at2);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(places(), (std::vector<std::string>{"-", "Gem", "Apple"}));
  /* The gap is an empty cell: it takes a drop, and the cell it came from is the gap now. */
  rig.Frame(at1);
  rig.Drop(at1, at0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(places(), (std::vector<std::string>{"Gem", "-", "Apple"}));
  /* An empty cell has nothing to drag. */
  rig.Frame(at1);
  rig.DragOver(at1, at0);
  EXPECT_EQ(rig.service.FindElementEvent(LN_UIElementEvent::Drag, "bag"), nullptr);
  rig.Frame(at0, 0);
  /* Keep Places off: two items swap as before; a gap already in the list stays until filled. */
  grid.rules.keep_places = false;
  rig.Frame(at2);
  rig.Drop(at2, at0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Swapped");
  EXPECT_EQ(places(), (std::vector<std::string>{"Apple", "-", "Gem"}));
}

TEST(LN_UIService, TooltipHidesWhileThePlayerDrags)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> apple(rig.scene);
  SceneBoundObject<KX_GameObject> slot0(rig.scene);
  SceneBoundObject<KX_GameObject> slot1(rig.scene);
  SceneBoundObject<KX_GameObject> tip0(rig.scene);
  SceneBoundObject<KX_GameObject> tip1(rig.scene);
  PlaceAt(slot0, 0.0f);
  PlaceAt(slot1, 2.0f);
  for (auto [slot, tip] : {std::pair{&slot0, &tip0}, std::pair{&slot1, &tip1}}) {
    slot->node.AddChild(&tip->node);
    tip->node.SetParentRelation(new KX_NormalParentRelation());
    tip->object.SetName("Tip");
    slot->object.NodeUpdateGS(0.0);
  }
  blender::Object tip_source = {};
  std::snprintf(tip_source.id.name, sizeof(tip_source.id.name), "OBTip");
  SetTextProperty(apple.object, "name", "Red Apple");
  rig.service.SetItemLibraryForTests(
      [&](const std::string &item) { return item == "Apple" ? &apple.object : nullptr; });
  SetItemList(player.object, "inventory", {"Gem", "Apple"});
  rig.AddGrid(79, {&slot0.object, &slot1.object}, player.object, "inventory", "bag");
  for (KX_GameObject *slot : {&slot0.object, &slot1.object}) {
    LN_UIService::ElementSettings &settings = rig.SettingsOf(*slot);
    settings.tooltip = &tip_source;
    settings.tooltip_text = "{name}";
    settings.tooltip_mode = LN_UITooltipMode::Beside;
  }
  rig.service.SetWorldDropForTests([](const float /*pointer_x*/, const float /*pointer_y*/) {
    return LN_UIService::WorldDropHit();
  });
  const MT_Vector3 at_slot0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at_slot1(2.0f, 0.0f, 0.0f);

  rig.Frame(at_slot1);
  EXPECT_TRUE(tip1.object.GetVisible());
  /* Dragging the Gem over the Apple: the Apple says nothing (it would cover the drop). */
  rig.Frame(at_slot0);
  rig.DragOver(at_slot0, at_slot1);
  EXPECT_FALSE(tip1.object.GetVisible());
  EXPECT_FALSE(tip0.object.GetVisible());
  /* Put down (they swap): what is pointed at says what it is again. */
  rig.Frame(at_slot1, 0);
  rig.Frame(at_slot1);
  rig.Frame(at_slot1);
  EXPECT_TRUE(tip1.object.GetVisible());
}

TEST(LN_UIService, TooltipShowsWhatTheElementAPlayerPointsAtSays)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> apple(rig.scene);
  SceneBoundObject<KX_GameObject> slot0(rig.scene);
  SceneBoundObject<KX_GameObject> slot1(rig.scene);
  SceneBoundObject<KX_GameObject> tip0(rig.scene);
  SceneBoundObject<KX_GameObject> tip1(rig.scene);
  SceneBoundObject<KX_GameObject> button(rig.scene);
  SceneBoundObject<KX_GameObject> info_line(rig.scene);
  PlaceAt(slot0, 0.0f);
  PlaceAt(slot1, 2.0f);
  PlaceAt(button, 8.0f);
  /* Each slot (a grid copy) has its own tooltip child, named like the authored one. */
  for (auto [slot, tip] : {std::pair{&slot0, &tip0}, std::pair{&slot1, &tip1}}) {
    slot->node.AddChild(&tip->node);
    tip->node.SetParentRelation(new KX_NormalParentRelation());
    tip->object.SetName("Tip");
    slot->object.NodeUpdateGS(0.0);
  }
  button.node.AddChild(&info_line.node);
  info_line.node.SetParentRelation(new KX_NormalParentRelation());
  info_line.object.SetName("InfoLine");
  button.object.NodeUpdateGS(0.0);
  blender::Object tip_source = {};
  std::snprintf(tip_source.id.name, sizeof(tip_source.id.name), "OBTip");
  blender::Object line_source = {};
  std::snprintf(line_source.id.name, sizeof(line_source.id.name), "OBInfoLine");
  SetTextProperty(apple.object, "name", "Red Apple");
  SetTextProperty(apple.object, "description", "Crunchy");
  rig.service.SetItemLibraryForTests(
      [&](const std::string &item) { return item == "Apple" ? &apple.object : nullptr; });
  SetItemList(player.object, "inventory", {"Apple"});
  rig.AddGrid(75, {&slot0.object, &slot1.object}, player.object, "inventory", "bag");
  for (KX_GameObject *slot : {&slot0.object, &slot1.object}) {
    LN_UIService::ElementSettings &settings = rig.SettingsOf(*slot);
    settings.tooltip = &tip_source;
    settings.tooltip_text = "{name}\n{description}";
    settings.tooltip_mode = LN_UITooltipMode::Beside;
    settings.tooltip_offset = MT_Vector3(0.0f, 1.0f, 0.0f);
  }
  /* A plain button saying something about itself on an info line that stays. */
  LN_UIService::ElementSettings button_settings = MakeElementSettings("settings");
  button_settings.tooltip = &line_source;
  button_settings.tooltip_text = "Open the {name}";
  button_settings.tooltip_mode = LN_UITooltipMode::TextOnly;
  SetTextProperty(button.object, "name", "Settings");
  rig.elements.push_back({&button.object, button_settings});
  const MT_Vector3 away(25.0f, 0.0f, 0.0f);
  const MT_Vector3 at_slot0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at_slot1(2.0f, 0.0f, 0.0f);
  const MT_Vector3 at_button(8.0f, 0.0f, 0.0f);

  /* Hidden until pointed at. */
  rig.Frame(away);
  EXPECT_FALSE(tip0.object.GetVisible());
  EXPECT_FALSE(tip1.object.GetVisible());
  EXPECT_TRUE(info_line.object.GetVisible());
  /* Beside its slot, with the item's words (no trailing line without a description). */
  rig.Frame(at_slot0);
  EXPECT_TRUE(tip0.object.GetVisible());
  EXPECT_EQ(TextProperty(tip0.object), "Red Apple\nCrunchy");
  EXPECT_NEAR(tip0.object.NodeGetWorldPosition().x(), 0.0f, 1.0e-5f);
  EXPECT_NEAR(tip0.object.NodeGetWorldPosition().y(), 1.0f, 1.0e-5f);
  /* An empty cell says nothing. */
  rig.Frame(at_slot1);
  EXPECT_FALSE(tip0.object.GetVisible());
  EXPECT_FALSE(tip1.object.GetVisible());
  /* Text Only: the line's text changes and empties, the line stays shown. */
  rig.Frame(at_button);
  EXPECT_EQ(TextProperty(info_line.object), "Open the Settings");
  EXPECT_TRUE(info_line.object.GetVisible());
  /* The words follow the property they show while it stays pointed at. */
  SetTextProperty(button.object, "name", "Options");
  rig.Frame(at_button);
  EXPECT_EQ(TextProperty(info_line.object), "Open the Options");
  /* A hint that ended emptied the shared line: it says it again. */
  SetTextProperty(info_line.object, "Text", "");
  rig.Frame(at_button);
  EXPECT_EQ(TextProperty(info_line.object), "Open the Options");
  rig.Frame(away);
  EXPECT_EQ(TextProperty(info_line.object), "");
  EXPECT_TRUE(info_line.object.GetVisible());
  /* What game logic writes on the line while it is pointed at stays, also when the pointer
   * leaves. */
  rig.Frame(at_button);
  EXPECT_EQ(TextProperty(info_line.object), "Open the Options");
  SetTextProperty(info_line.object, "Text", "Saved");
  rig.Frame(at_button);
  EXPECT_EQ(TextProperty(info_line.object), "Saved");
  rig.Frame(away);
  EXPECT_EQ(TextProperty(info_line.object), "Saved");
  slot0.node.RemoveChild(&tip0.node);
  slot1.node.RemoveChild(&tip1.node);
  button.node.RemoveChild(&info_line.node);
}

namespace {

/** `entry` with its own `key` (an item's own data, such as a sword's durability). */
LN_Value WithOwn(LN_Value entry, const std::string &key, const int32_t number)
{
  LN_Value &value = entry.dict_value[key];
  value.type = LN_ValueType::Int;
  value.exists = true;
  value.int_value = number;
  return entry;
}

LN_Value WithOwn(LN_Value entry, const std::string &key, const std::string &text)
{
  LN_Value &value = entry.dict_value[key];
  value.type = LN_ValueType::String;
  value.exists = true;
  value.string_value = text;
  return entry;
}

void SetEntries(KX_GameObject &object, const char *property, const std::vector<LN_Value> &entries)
{
  LN_Value list;
  list.type = LN_ValueType::List;
  list.exists = true;
  list.list_value = entries;
  std::string reason;
  EXPECT_TRUE(LN_CommandBuffer::ApplyGamePropertyValue(&object, property, list, nullptr, reason))
      << reason;
}

/** What a grid slot shows: "Apple@1" (its item and its place in the list), "-@4" when empty. */
std::string ShownIn(GridInventoryRig &rig, KX_GameObject &slot)
{
  LN_Value item;
  int32_t index = -1;
  if (!rig.service.GetBoundItem(&slot, item, index)) {
    return "(unbound)";
  }
  std::string name;
  int32_t count = 0;
  return (LN_InventoryEntry(item, name, count) ? name : std::string("-")) + "@" +
         std::to_string(index);
}

/** Keyboard actions of the game's Input Map. */
void LoadKeyActions(const std::vector<std::pair<const char *, int>> &actions)
{
  blender::GameData game;
  blender::BKE_game_input_map_ensure_builtins(&game);
  for (const auto &[name, key] : actions) {
    blender::GameInputAction *action = blender::BKE_game_input_action_add(
        &game, name, GAME_INPUT_ACTION_BUTTON);
    blender::BKE_game_input_binding_add(action, GAME_INPUT_KEYBOARD)->code = key;
  }
  LN_InputMap::Get().Load(&game);
  blender::BKE_game_input_map_free(&game);
}

}  // namespace

/* Show Categories and Search: the grid shows only the matching items, each still at its real place
 * in the list, so moves and drops change the right items. */
TEST(LN_UIService, GridFilterShowsMatchingItemsAtTheirRealPlaces)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> chest(rig.scene);
  SceneBoundObject<KX_GameObject> apple(rig.scene);
  SceneBoundObject<KX_GameObject> mushroom(rig.scene);
  SceneBoundObject<KX_GameObject> sword(rig.scene);
  std::array<SceneBoundObject<KX_GameObject>, 3> slots = {
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene)};
  SceneBoundObject<KX_GameObject> chest_slot(rig.scene);
  for (size_t slot = 0; slot < slots.size(); ++slot) {
    PlaceAt(slots[slot], 2.0f * float(slot));
  }
  PlaceAt(chest_slot, 10.0f);
  const MT_Vector3 at0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at1(2.0f, 0.0f, 0.0f);
  const MT_Vector3 at2(4.0f, 0.0f, 0.0f);
  const MT_Vector3 at_chest(10.0f, 0.0f, 0.0f);
  SetTextProperty(apple.object, "category", "food");
  SetTextProperty(apple.object, "name", "Red Apple");
  SetTextProperty(mushroom.object, "category", "food");
  SetTextProperty(sword.object, "category", "weapon");
  rig.service.SetItemLibraryForTests([&](const std::string &item) -> KX_GameObject * {
    return item == "Apple"    ? &apple.object :
           item == "Mushroom" ? &mushroom.object :
           item == "Sword"    ? &sword.object :
                                nullptr;
  });
  SetItemList(player.object, "inventory", {"Sword", "Apple", "Gem", "Mushroom"});
  SetItemList(chest.object, "chest", {"Potion"});
  LN_UIService::ViewRegistration &bag = rig.AddGrid(
      80, {&slots[0].object, &slots[1].object, &slots[2].object}, player.object, "inventory", "bag");
  rig.AddGrid(81, {&chest_slot.object}, chest.object, "chest", "chest");
  const auto shown = [&]() {
    return std::vector<std::string>{ShownIn(rig, slots[0].object),
                                    ShownIn(rig, slots[1].object),
                                    ShownIn(rig, slots[2].object)};
  };
  using Shown = std::vector<std::string>;

  /* Food only: the Apple and the Mushroom at their places; the empty cell stands for the end. */
  bag.show_categories = "Food";
  rig.Frame(at0);
  EXPECT_EQ(shown(), (Shown{"Apple@1", "Mushroom@3", "-@4"}));
  /* A swap swaps them in the list, the Sword and the Gem stay where they are. */
  rig.Drop(at1, at0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Swapped");
  EXPECT_EQ(ListedItems(player.object, "inventory"),
            (std::vector<std::string>{"Sword", "Mushroom", "Gem", "Apple"}));
  rig.Frame(at0);
  EXPECT_EQ(shown(), (Shown{"Mushroom@1", "Apple@3", "-@4"}));
  /* Within a filtered list an empty cell is no place to move to: nothing changes. */
  rig.Drop(at0, at2);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "");
  EXPECT_EQ(ListedItems(player.object, "inventory"),
            (std::vector<std::string>{"Sword", "Mushroom", "Gem", "Apple"}));
  /* From another list it goes to the list's end (and is not food, so the grid hides it). */
  rig.Frame(at_chest);
  rig.Drop(at_chest, at2);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(ListedItems(player.object, "inventory"),
            (std::vector<std::string>{"Sword", "Mushroom", "Gem", "Apple", "Potion"}));
  rig.Frame(at0);
  EXPECT_EQ(shown(), (Shown{"Mushroom@1", "Apple@3", "-@5"}));

  /* Search: in the name players see (the Item Library's "name"), any case, spaces trimmed. */
  bag.show_categories.clear();
  bag.search = " red ";
  rig.Frame(at0);
  EXPECT_EQ(shown(), (Shown{"Apple@3", "-@5", "-@5"}));
  bag.search = "SWO";
  rig.Frame(at0);
  EXPECT_EQ(shown(), (Shown{"Sword@0", "-@5", "-@5"}));
  /* Both: an item must match both. Several categories: any of them. */
  bag.search = "o";
  bag.show_categories = "weapon, food";
  rig.Frame(at0);
  EXPECT_EQ(shown(), (Shown{"Sword@0", "Mushroom@1", "-@5"}));
  /* No filter: every item at its own place, as before. */
  bag.search.clear();
  bag.show_categories.clear();
  rig.Frame(at0);
  EXPECT_EQ(shown(), (Shown{"Sword@0", "Mushroom@1", "Gem@2"}));

  /* An item's own category comes before its library's. */
  SetEntries(player.object,
             "inventory",
             {LN_MakeInventoryEntry("Sword", 1),
              WithOwn(LN_MakeInventoryEntry("Gem", 1), "category", "food")});
  bag.show_categories = "food";
  rig.Frame(at0);
  EXPECT_EQ(shown(), (Shown{"Gem@1", "-@2", "-@2"}));
}

/* Use Action uses the slot the player highlights, Use Keys the n-th slot (a hotbar), only for a
 * slot holding an item, and the press is the UI's. */
TEST(LN_UIService, GridUseActionAndUseKeysUseTheSlotsItem)
{
  LoadKeyActions({{"UseItem", blender::EVT_FKEY},
                  {"Quick1", blender::EVT_ONEKEY},
                  {"Quick2", blender::EVT_TWOKEY}});
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  SceneBoundObject<KX_GameObject> slot0(rig.scene);
  SceneBoundObject<KX_GameObject> slot1(rig.scene);
  SceneBoundObject<KX_GameObject> other(rig.scene);
  PlaceAt(slot0, 0.0f);
  PlaceAt(slot1, 2.0f);
  const MT_Vector3 at0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 away(25.0f, 0.0f, 0.0f);
  SetItemList(owner.object, "inventory", {"Apple", "Gem"});
  LN_UIService::ViewRegistration &grid = rig.AddGrid(
      82, {&slot0.object, &slot1.object}, owner.object, "inventory", "bag");
  grid.use_action = "UseItem";
  grid.use_keys = "Quick1, Quick2";
  const auto used = [&]() -> std::string {
    const LN_UIService::ElementEvent *event = rig.service.FindElementEvent(
        LN_UIElementEvent::Use, "bag");
    if (event == nullptr) {
      return "(none)";
    }
    std::string name;
    int32_t count = 0;
    LN_InventoryEntry(event->item, name, count);
    KX_GameObject *slot = rig.manager.ResolveObjectHandle(event->object);
    return name + "@" + std::to_string(event->index) +
           (slot == &slot0.object ? " slot0" :
            slot == &slot1.object ? " slot1" :
                                    " ?");
  };
  const LN_InputConsumption &consumed = rig.manager.GetInputConsumption();
  /* What the UI took is forgotten each frame, as LN_Manager::BeginFrame does. */
  const auto next_frame = [&]() { const_cast<LN_InputConsumption &>(consumed).BeginFrame(); };

  /* The highlighted slot. */
  rig.Frame(at0);
  rig.Frame(at0, -1, false, SCA_IInputDevice::FKEY);
  EXPECT_EQ(used(), "Apple@0 slot0");
  EXPECT_TRUE(consumed.IsPressConsumed(SCA_IInputDevice::FKEY));
  /* Nothing highlighted: nothing used, and F stays the game's. */
  rig.Frame(away);
  next_frame();
  rig.Frame(away, -1, false, SCA_IInputDevice::FKEY);
  EXPECT_EQ(used(), "(none)");
  EXPECT_FALSE(consumed.IsPressConsumed(SCA_IInputDevice::FKEY));
  /* The keys: the n-th key the n-th slot, wherever the pointer is. */
  rig.Frame(away, -1, false, SCA_IInputDevice::TWOKEY);
  EXPECT_EQ(used(), "Gem@1 slot1");
  rig.Frame(away, -1, false, SCA_IInputDevice::ONEKEY);
  EXPECT_EQ(used(), "Apple@0 slot0");
  /* Before the UI frame too (a Logic + Physics step first): the key is hidden from the game. */
  rig.Frame(away);
  next_frame();
  rig.device.ClearInputs();
  rig.device.ConvertEvent(SCA_IInputDevice::TWOKEY, 1, 0);
  rig.input.Capture(&rig.device, nullptr);
  rig.service.ConsumeInputBeforeResolve(rig.input);
  EXPECT_TRUE(consumed.IsPressConsumed(SCA_IInputDevice::TWOKEY));
  rig.pressed_key = SCA_IInputDevice::TWOKEY;
  /* An empty slot uses nothing, and leaves the key to the game. */
  SetItemList(owner.object, "inventory", {"Apple"});
  rig.Frame(away);
  next_frame();
  rig.Frame(away, -1, false, SCA_IInputDevice::TWOKEY);
  EXPECT_EQ(used(), "(none)");
  EXPECT_FALSE(consumed.IsPressConsumed(SCA_IInputDevice::TWOKEY));
  /* While the player uses a UI in the game world (Take UI Control) the keys are that UI's. */
  ASSERT_EQ(rig.service.TakeControl(&other.object, 0, true), LN_UIResult::Success);
  rig.Frame(away);
  rig.Frame(away, -1, false, SCA_IInputDevice::ONEKEY);
  EXPECT_EQ(used(), "(none)");
  ASSERT_EQ(rig.service.TakeControl(&other.object, 0, false), LN_UIResult::Success);
  rig.Frame(away);
  rig.Frame(away, -1, false, SCA_IInputDevice::ONEKEY);
  EXPECT_EQ(used(), "Apple@0 slot0");
  /* While the player is in another menu the keys are that menu's. */
  rig.service.AddMenuForTests(
      0, LN_UIMenuRouting::Modal, LN_UIMenuBackBehavior::CloseMenu, {&other.object});
  rig.Frame(away);
  rig.Frame(away, -1, false, SCA_IInputDevice::ONEKEY);
  EXPECT_EQ(used(), "(none)");
  LN_InputMap::Get().Load(nullptr);
}

/* Item Shown: each slot tells what it shows when that changes (opening, another item, a count or
 * own value changed, emptied), at most one node frame's worth of events a frame. */
TEST(LN_UIService, GridSlotsTellWhatTheyShowWhenItChanges)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  std::array<SceneBoundObject<KX_GameObject>, 3> slots = {
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene),
      SceneBoundObject<KX_GameObject>(rig.scene)};
  for (size_t slot = 0; slot < slots.size(); ++slot) {
    PlaceAt(slots[slot], 2.0f * float(slot));
    slots[slot].object.SetName("S" + std::to_string(slot));
  }
  const MT_Vector3 away(25.0f, 0.0f, 0.0f);
  SetEntries(owner.object,
             "inventory",
             {WithOwn(LN_MakeInventoryEntry("Sword", 1), "durability", 5),
              LN_MakeInventoryEntry("Apple", 1)});
  rig.AddGrid(
      83, {&slots[0].object, &slots[1].object, &slots[2].object}, owner.object, "inventory", "bag");
  const auto told = [&]() {
    std::vector<std::string> events;
    size_t count = 0;
    rig.service.FindElementEvent(LN_UIElementEvent::ItemShown, "bag", -1, 0, &count);
    for (size_t index = 0; index < count; ++index) {
      const LN_UIService::ElementEvent *event = rig.service.FindElementEvent(
          LN_UIElementEvent::ItemShown, "bag", -1, index);
      std::string name = "-";
      int32_t amount = 0;
      LN_InventoryEntry(event->item, name, amount);
      const auto durability = event->item.dict_value.find("durability");
      events.push_back(rig.manager.ResolveObjectHandle(event->object)->GetName() + ":" + name +
                       " x" + std::to_string(event->amount) + "@" +
                       std::to_string(event->index) +
                       (durability != event->item.dict_value.end() ?
                            " d" + std::to_string(durability->second.int_value) :
                            ""));
    }
    return events;
  };
  using Told = std::vector<std::string>;

  /* Opening: every cell, the empty one too. */
  rig.Frame(away);
  EXPECT_EQ(told(), (Told{"S0:Sword x1@0 d5", "S1:Apple x1@1", "S2:- x0@2"}));
  rig.Frame(away);
  EXPECT_EQ(told(), Told{});
  /* A count and an own value. */
  SetEntries(owner.object,
             "inventory",
             {WithOwn(LN_MakeInventoryEntry("Sword", 1), "durability", 4),
              LN_MakeInventoryEntry("Apple", 3)});
  rig.Frame(away);
  EXPECT_EQ(told(), (Told{"S0:Sword x1@0 d4", "S1:Apple x3@1"}));
  /* The Sword gone: the Apple moves up, its old cell empties. */
  SetEntries(owner.object, "inventory", {LN_MakeInventoryEntry("Apple", 3)});
  rig.Frame(away);
  EXPECT_EQ(told(), (Told{"S0:Apple x3@0", "S1:- x0@1"}));
  rig.Frame(away);
  EXPECT_EQ(told(), Told{});
}

TEST(LN_UIService, ABigGridTellsWhatItShowsOverSeveralFrames)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> owner(rig.scene);
  std::deque<SceneBoundObject<KX_GameObject>> slots;
  std::vector<KX_GameObject *> slot_objects;
  for (int slot = 0; slot < 70; ++slot) {
    slot_objects.push_back(&slots.emplace_back(rig.scene).object);
  }
  SetItemList(owner.object, "inventory", {"Apple"});
  rig.AddGrid(84, slot_objects, owner.object, "inventory", "bag");
  const auto told = [&]() {
    size_t count = 0;
    rig.service.FindElementEvent(LN_UIElementEvent::ItemShown, "bag", -1, 0, &count);
    return count;
  };
  const MT_Vector3 away(100.0f, 0.0f, 0.0f);
  rig.Frame(away);
  EXPECT_EQ(told(), size_t(64));
  rig.Frame(away);
  EXPECT_EQ(told(), size_t(6));
  rig.Frame(away);
  EXPECT_EQ(told(), size_t(0));
}

/* {tokens} read the item's own values first; a library text with braces ("Durability
 * {durability}") shows the item's values too. */
TEST(LN_UIService, ItemTokensReadTheItemsOwnValuesFirst)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> sword(rig.scene);
  SceneBoundObject<KX_GameObject> slot0(rig.scene);
  SceneBoundObject<KX_GameObject> slot1(rig.scene);
  SceneBoundObject<KX_GameObject> tip0(rig.scene);
  SceneBoundObject<KX_GameObject> tip1(rig.scene);
  PlaceAt(slot0, 0.0f);
  PlaceAt(slot1, 2.0f);
  for (auto [slot, tip] : {std::pair{&slot0, &tip0}, std::pair{&slot1, &tip1}}) {
    slot->node.AddChild(&tip->node);
    tip->node.SetParentRelation(new KX_NormalParentRelation());
    tip->object.SetName("Tip");
    slot->object.NodeUpdateGS(0.0);
  }
  blender::Object tip_source = {};
  std::snprintf(tip_source.id.name, sizeof(tip_source.id.name), "OBTip");
  SetTextProperty(sword.object, "description", "Durability {durability}");
  EXP_IntValue *durability = new EXP_IntValue(10, "durability");
  sword.object.SetProperty("durability", durability);
  durability->Release();
  rig.service.SetItemLibraryForTests(
      [&](const std::string &item) { return item == "Sword" ? &sword.object : nullptr; });
  SetEntries(player.object,
             "inventory",
             {WithOwn(WithOwn(LN_MakeInventoryEntry("Sword", 1), "durability", 4),
                      "name",
                      "Old Sword"),
              LN_MakeInventoryEntry("Sword", 1)});
  rig.AddGrid(85, {&slot0.object, &slot1.object}, player.object, "inventory", "bag");
  for (KX_GameObject *slot : {&slot0.object, &slot1.object}) {
    LN_UIService::ElementSettings &settings = rig.SettingsOf(*slot);
    settings.tooltip = &tip_source;
    settings.tooltip_text = "{name}\n{description}";
    settings.tooltip_mode = LN_UITooltipMode::Beside;
  }
  rig.Frame(MT_Vector3(0.0f, 0.0f, 0.0f));
  EXPECT_EQ(TextProperty(tip0.object), "Old Sword\nDurability 4");
  /* Without its own: the library's. */
  rig.Frame(MT_Vector3(2.0f, 0.0f, 0.0f));
  EXPECT_EQ(TextProperty(tip1.object), "Sword\nDurability 10");
  slot0.node.RemoveChild(&tip0.node);
  slot1.node.RemoveChild(&tip1.node);
}

/* Show Message: newest line last, at most Lines, each gone after its Seconds; the same text moves
 * to the end; what game logic wrote over the messages stays. */
TEST(LN_UIService, ShowMessageStacksLinesAndTakesEachAwayInTime)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> log(rig.scene);
  const MT_Vector3 away(25.0f, 0.0f, 0.0f);
  const auto show = [&](const char *text, const double seconds) {
    return rig.service.ShowMessage(&log.object, -1, text, seconds, 3);
  };
  const auto frames = [&](const int count) {
    for (int frame = 0; frame < count; ++frame) {
      rig.Frame(away);
    }
  };
  EXPECT_TRUE(show("Picked up Apple", 0.05));
  EXPECT_EQ(TextProperty(log.object), "Picked up Apple");
  /* 0 seconds: it stays until newer lines push it out. */
  EXPECT_TRUE(show("Bag full", 0.0));
  EXPECT_EQ(TextProperty(log.object), "Picked up Apple\nBag full");
  frames(4);
  EXPECT_EQ(TextProperty(log.object), "Bag full");
  show("Picked up Gem", 0.05);
  show("Picked up Coin", 0.05);
  show("Picked up Key", 0.05);
  EXPECT_EQ(TextProperty(log.object), "Picked up Gem\nPicked up Coin\nPicked up Key");
  /* The same text again: last, with its time from now. */
  frames(2);
  show("Picked up Gem", 0.05);
  EXPECT_EQ(TextProperty(log.object), "Picked up Coin\nPicked up Key\nPicked up Gem");
  frames(2);
  EXPECT_EQ(TextProperty(log.object), "Picked up Gem");
  /* Nothing to show adds nothing. */
  EXPECT_TRUE(show("", 1.0));
  EXPECT_EQ(TextProperty(log.object), "Picked up Gem");
  /* Game logic wrote over it: theirs stays. */
  SetTextProperty(log.object, "Text", "Saved");
  frames(4);
  EXPECT_EQ(TextProperty(log.object), "Saved");
  show("Hello", 0.05);
  EXPECT_EQ(TextProperty(log.object), "Hello");
  frames(4);
  EXPECT_EQ(TextProperty(log.object), "");
  EXPECT_FALSE(rig.service.ShowMessage(nullptr, -1, "Lost", 1.0, 3));
}

/* An item that cannot change places with the one it is dropped on (its quick slot takes only
 * food) goes into the other grid's list instead: unequipping into a bag whose cells are all
 * filled. Its own kind's stack first, then the first gap, else the end; refused only when the list
 * has no room. A grid of one cell is a slot: nothing goes in beside its item. */
TEST(LN_UIService, GridDropThatCannotSwapGoesIntoTheOtherList)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> apple(rig.scene);
  SceneBoundObject<KX_GameObject> mushroom(rig.scene);
  SceneBoundObject<KX_GameObject> gem(rig.scene);
  std::array<SceneBoundObject<KX_GameObject>, 3> bag = {SceneBoundObject<KX_GameObject>(rig.scene),
                                                         SceneBoundObject<KX_GameObject>(rig.scene),
                                                         SceneBoundObject<KX_GameObject>(rig.scene)};
  SceneBoundObject<KX_GameObject> quick(rig.scene);
  SceneBoundObject<KX_GameObject> pocket(rig.scene);
  for (size_t slot = 0; slot < bag.size(); ++slot) {
    PlaceAt(bag[slot], 2.0f * float(slot));
  }
  PlaceAt(quick, 10.0f);
  PlaceAt(pocket, 14.0f);
  const MT_Vector3 at_bag0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at_bag2(4.0f, 0.0f, 0.0f);
  const MT_Vector3 at_quick(10.0f, 0.0f, 0.0f);
  const MT_Vector3 at_pocket(14.0f, 0.0f, 0.0f);
  const MT_Vector3 away(25.0f, 0.0f, 0.0f);
  SetTextProperty(apple.object, "category", "food");
  SetTextProperty(mushroom.object, "category", "food");
  SetTextProperty(gem.object, "category", "treasure");
  rig.service.SetItemLibraryForTests([&](const std::string &item) -> KX_GameObject * {
    return item == "Apple"    ? &apple.object :
           item == "Mushroom" ? &mushroom.object :
           item == "Gem"      ? &gem.object :
                                nullptr;
  });
  LN_UIService::ViewRegistration &bag_grid = rig.AddGrid(
      90, {&bag[0].object, &bag[1].object, &bag[2].object}, player.object, "inventory", "bag");
  rig.AddGrid(91, {&quick.object}, player.object, "quick", "quick");
  LN_UIService::ViewRegistration &pocket_grid = rig.AddGrid(
      92, {&pocket.object}, player.object, "pocket", "pocket");
  rig.SettingsOf(quick.object).accept_categories = ",food,";
  const auto unequip = [&](const std::vector<std::string> &inventory, const MT_Vector3 &onto) {
    SetItemList(player.object, "inventory", inventory);
    SetItemList(player.object, "quick", {"Apple"});
    rig.Frame(away);
    rig.Drop(at_quick, onto);
    return rig.Outcome(LN_UIElementEvent::DropReceived, "bag");
  };
  using Items = std::vector<std::string>;

  /* Every cell filled, nothing that stacks: the Apple goes to the end (the Gem stays). */
  EXPECT_EQ(unequip({"Sword", "Gem", "Coin"}, at_bag2), "Moved");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (Items{"Sword", "Gem", "Coin", "Apple"}));
  EXPECT_EQ(ListedItems(player.object, "quick"), Items{});
  /* An Apple stack in the bag takes it. */
  EXPECT_EQ(unequip({"Apple", "Gem", "Coin"}, at_bag2), "Stacked");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (Items{"Apple x2", "Gem", "Coin"}));
  /* Food it can take back: the two still change places, as before. */
  EXPECT_EQ(unequip({"Mushroom", "Gem", "Coin"}, at_bag0), "Swapped");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (Items{"Apple", "Gem", "Coin"}));
  EXPECT_EQ(ListedItems(player.object, "quick"), Items{"Mushroom"});
  /* Keep Places: into the first gap. */
  bag_grid.rules.keep_places = true;
  SetEntries(player.object,
             "inventory",
             {LN_MakeInventoryEntry("Sword", 1), LN_Value(), LN_MakeInventoryEntry("Gem", 1)});
  SetItemList(player.object, "quick", {"Apple"});
  rig.Frame(away);
  rig.Drop(at_quick, at_bag2);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (Items{"Sword", "Apple", "Gem"}));
  bag_grid.rules.keep_places = false;
  /* A bag at its Capacity has no room: refused, the Apple stays in its slot. */
  bag_grid.rules.capacity = 3;
  EXPECT_EQ(unequip({"Sword", "Gem", "Coin"}, at_bag2), "(no event)");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (Items{"Sword", "Gem", "Coin"}));
  EXPECT_EQ(ListedItems(player.object, "quick"), Items{"Apple"});
  bag_grid.rules.capacity = 0;

  /* A one-cell grid (equipment) that does not swap: nothing goes in beside its item. */
  pocket_grid.inventory.swap = false;
  SetItemList(player.object, "inventory", {"Gem"});
  SetItemList(player.object, "pocket", {"Mushroom"});
  rig.Frame(away);
  rig.Drop(at_bag0, at_pocket);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "pocket"), "");
  EXPECT_EQ(ListedItems(player.object, "pocket"), Items{"Mushroom"});
  EXPECT_EQ(ListedItems(player.object, "inventory"), Items{"Gem"});
}

/* Show Message's Count: a pickup feed. The same text again while its line shows adds up ("Apple
 * x3"), nothing is shown for 1, {count} puts the total where the text says, and once the line has
 * gone the count starts over. Count 0 adds no line (nothing was picked up); -1 is a plain
 * message. */
TEST(LN_UIService, ShowMessageAddsUpTheCountsOfTheSameText)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> log(rig.scene);
  const MT_Vector3 away(25.0f, 0.0f, 0.0f);
  const auto show = [&](const char *text, const int32_t count) {
    return rig.service.ShowMessage(&log.object, -1, text, 0.05, 4, count);
  };
  const auto frames = [&](const int count) {
    for (int frame = 0; frame < count; ++frame) {
      rig.Frame(away);
    }
  };
  EXPECT_TRUE(show("Apple", 1));
  EXPECT_EQ(TextProperty(log.object), "Apple");
  show("Gold Coin", 5);
  show("Apple", 2);
  EXPECT_EQ(TextProperty(log.object), "Gold Coin x5\nApple x3");
  show("+{count} Gem", 1);
  show("+{count} Gem", 1);
  EXPECT_EQ(TextProperty(log.object), "Gold Coin x5\nApple x3\n+2 Gem");
  EXPECT_TRUE(show("Apple", 0));
  EXPECT_EQ(TextProperty(log.object), "Gold Coin x5\nApple x3\n+2 Gem");
  frames(4);
  EXPECT_EQ(TextProperty(log.object), "");
  show("Apple", 1);
  show("Bag full", -1);
  show("Bag full", -1);
  EXPECT_EQ(TextProperty(log.object), "Apple\nBag full");
}

/* A list's Inventory Rules refuse what it may not hold, whatever grid moves it: a drag from
 * another list is refused and goes back; a swap that would put such an item into the other list
 * puts the dragged stack into the list instead. */
TEST(LN_UIService, GridListAcceptCategoriesRefuseWhatTheListMayNotHold)
{
  GridInventoryRig rig;
  SceneBoundObject<KX_GameObject> player(rig.scene);
  SceneBoundObject<KX_GameObject> apple(rig.scene);
  SceneBoundObject<KX_GameObject> sword(rig.scene);
  std::array<SceneBoundObject<KX_GameObject>, 3> bag = {SceneBoundObject<KX_GameObject>(rig.scene),
                                                         SceneBoundObject<KX_GameObject>(rig.scene),
                                                         SceneBoundObject<KX_GameObject>(rig.scene)};
  SceneBoundObject<KX_GameObject> weapon(rig.scene);
  for (size_t slot = 0; slot < bag.size(); ++slot) {
    PlaceAt(bag[slot], 2.0f * float(slot));
  }
  PlaceAt(weapon, 10.0f);
  const MT_Vector3 at_bag0(0.0f, 0.0f, 0.0f);
  const MT_Vector3 at_bag1(2.0f, 0.0f, 0.0f);
  const MT_Vector3 at_weapon(10.0f, 0.0f, 0.0f);
  const MT_Vector3 away(25.0f, 0.0f, 0.0f);
  SetTextProperty(apple.object, "category", "food");
  SetTextProperty(sword.object, "category", "weapon");
  rig.service.SetItemLibraryForTests([&](const std::string &item) -> KX_GameObject * {
    return item == "Apple" ? &apple.object : item == "Sword" ? &sword.object : nullptr;
  });
  rig.AddGrid(97, {&bag[0].object, &bag[1].object, &bag[2].object}, player.object, "inventory", "bag");
  LN_UIService::ViewRegistration &weapon_grid = rig.AddGrid(
      98, {&weapon.object}, player.object, "weapon", "weapon");
  /* The weapon list takes only weapons; its slot itself takes anything. */
  weapon_grid.rules.accept_categories = ",weapon,";
  using Items = std::vector<std::string>;

  /* The apple is refused: it stays in the bag, and the weapon list stays empty. */
  SetItemList(player.object, "inventory", {"Apple", "Sword"});
  SetItemList(player.object, "weapon", {});
  rig.Frame(away);
  rig.Drop(at_bag0, at_weapon);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "weapon"), "(no event)");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (Items{"Apple", "Sword"}));
  EXPECT_EQ(ListedItems(player.object, "weapon"), Items{});
  /* The sword goes in. */
  rig.Frame(away);
  rig.Drop(at_bag1, at_weapon);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "weapon"), "Moved");
  EXPECT_EQ(ListedItems(player.object, "inventory"), Items{"Apple"});
  EXPECT_EQ(ListedItems(player.object, "weapon"), Items{"Sword"});
  /* The sword dropped on the apple would swap the apple into the weapon list: it goes into the
   * bag beside the apple instead. */
  rig.Frame(away);
  rig.Drop(at_weapon, at_bag0);
  EXPECT_EQ(rig.Outcome(LN_UIElementEvent::DropReceived, "bag"), "Moved");
  EXPECT_EQ(ListedItems(player.object, "inventory"), (Items{"Apple", "Sword"}));
  EXPECT_EQ(ListedItems(player.object, "weapon"), Items{});
}

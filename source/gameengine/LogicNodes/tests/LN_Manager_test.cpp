/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "KX_GameObject.h"
#include "LN_Manager.h"
#include "LN_Program.h"

class LN_ManagerTestAccess {
 public:
  static void AddConstraintBreakEvent(LN_Manager &manager,
                                      KX_GameObject &owner,
                                      KX_GameObject *target,
                                      const std::string &name)
  {
    LN_RigidBodyConstraintBreakEvent event;
    event.owner = manager.MakeObjectHandle(&owner, owner.GetName());
    event.target = manager.MakeObjectHandle(target, target ? target->GetName() : std::string());
    event.constraint_name = name;
    manager.m_rigidBodyConstraintBreakEvents.push_back(std::move(event));
    manager.IndexRigidBodyConstraintBreakEvents();
  }

  static void QueueForUpdate(LN_Manager &manager)
  {
    manager.QueueRigidBodyConstraintBreakEventsForUpdate();
  }

  static void ActivatePending(LN_Manager &manager)
  {
    manager.ActivatePendingRigidBodyConstraintBreakEvents();
  }

  static void ClearActive(LN_Manager &manager)
  {
    manager.ClearRigidBodyConstraintBreakEvents();
  }

  static size_t PendingCount(const LN_Manager &manager)
  {
    return manager.m_pendingRigidBodyConstraintBreakEvents.size();
  }

  static std::vector<std::string> EventNamesForOwner(LN_Manager &manager, KX_GameObject &owner)
  {
    std::vector<std::string> result;
    for (uint32_t event_index = manager.FirstRigidBodyConstraintBreakEventIndex(&owner);
         event_index != LN_INVALID_INDEX;
         event_index = manager.NextRigidBodyConstraintBreakEventIndex(event_index))
    {
      const LN_RigidBodyConstraintBreakEvent *event =
          manager.RigidBodyConstraintBreakEventAt(event_index);
      if (event != nullptr) {
        result.push_back(event->constraint_name);
      }
    }
    return result;
  }

  static void InvalidateObject(LN_Manager &manager, KX_GameObject &object)
  {
    manager.m_denseIds.InvalidateHandle(LN_DenseHandleKind::Object, &object);
  }
};

namespace {

TEST(LN_Manager, ExposesItsOwningRuntimeScene)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);

  EXPECT_EQ(&manager.GetScene(), unused_scene);
}

TEST(LN_Manager, ManyReplicaProgramRegistrationsReuseCompiledProgram)
{
  /* This test only exercises registration/cache paths that do not dereference the scene. */
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);

  std::shared_ptr<LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 2.0f, 3.0f));
  program->InternString("replica_event");

  LN_GamePropertyRef game_property_ref;
  game_property_ref.name = "replica_property";
  game_property_ref.name_id = program->InternString(game_property_ref.name);
  program->AddGamePropertyRef(game_property_ref);

  LN_TreePropertyRef tree_property_ref;
  tree_property_ref.name = "replica_tree_property";
  tree_property_ref.name_id = program->InternString(tree_property_ref.name);
  program->AddTreePropertyRef(tree_property_ref);

  std::vector<std::unique_ptr<KX_GameObject>> objects;
  objects.reserve(4096);
  for (uint32_t index = 0; index < 4096; index++) {
    std::unique_ptr<KX_GameObject> object = std::make_unique<KX_GameObject>();
    object->SetName("ReplicaObject" + std::to_string(index));
    ASSERT_NE(manager.RegisterCompiledProgram(object.get(), program, index, 0, true), nullptr);
    objects.push_back(std::move(object));
  }

  EXPECT_EQ(manager.GetCachedProgramCountForTests(), 1u);
}

TEST(LN_Manager, BulkQueuedRemovalDetachesRuntimeTreesBeforeFinalUnregister)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);
  std::shared_ptr<LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 2.0f, 3.0f));

  std::vector<std::unique_ptr<KX_GameObject>> objects;
  std::vector<KX_GameObject *> removal_queue;
  for (uint32_t index = 0; index < 16; index++) {
    std::unique_ptr<KX_GameObject> object = std::make_unique<KX_GameObject>();
    object->SetName("BulkRemovedObject" + std::to_string(index));
    ASSERT_NE(manager.RegisterCompiledProgram(object.get(), program, index, 0, true), nullptr);
    removal_queue.push_back(object.get());
    objects.push_back(std::move(object));
  }
  ASSERT_TRUE(manager.HasRuntimeTrees());

  manager.NotifyGameObjectsQueuedForBulkRemoval(removal_queue);
  EXPECT_FALSE(manager.HasRuntimeTrees());

  for (KX_GameObject *object : removal_queue) {
    manager.UnregisterGameObject(object);
  }
  EXPECT_FALSE(manager.HasRuntimeTrees());
}

TEST(LN_Manager, ConstraintBreakSnapshotsBridgeFixedBatchesToOneUpdate)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);
  KX_GameObject owner;
  KX_GameObject target;
  owner.SetName("Owner");
  target.SetName("Target");

  LN_ManagerTestAccess::AddConstraintBreakEvent(manager, owner, &target, "joint_A");
  EXPECT_NE(manager.FindRigidBodyConstraintBreakEvent(
                &owner, "joint_A", LN_RigidBodyConstraintMatchMode::Exact),
            nullptr);
  LN_ManagerTestAccess::QueueForUpdate(manager);
  EXPECT_EQ(LN_ManagerTestAccess::PendingCount(manager), 1u);
  EXPECT_EQ(manager.FindRigidBodyConstraintBreakEvent(
                &owner, "joint_A", LN_RigidBodyConstraintMatchMode::Exact),
            nullptr);

  LN_ManagerTestAccess::AddConstraintBreakEvent(manager, owner, &target, "joint_B");
  LN_ManagerTestAccess::QueueForUpdate(manager);
  EXPECT_EQ(LN_ManagerTestAccess::PendingCount(manager), 2u);

  LN_ManagerTestAccess::ActivatePending(manager);
  EXPECT_EQ((LN_ManagerTestAccess::EventNamesForOwner(manager, owner)),
            (std::vector<std::string>{"joint_A", "joint_B"}));
  EXPECT_NE(manager.FindRigidBodyConstraintBreakEvent(
                &owner, "joint_", LN_RigidBodyConstraintMatchMode::Contains),
            nullptr);
  EXPECT_NE(manager.FindRigidBodyConstraintBreakEvent(
                &owner, std::string(), LN_RigidBodyConstraintMatchMode::All),
            nullptr);

  LN_ManagerTestAccess::ClearActive(manager);
  EXPECT_EQ(manager.FindRigidBodyConstraintBreakEvent(
                &owner, "joint_A", LN_RigidBodyConstraintMatchMode::Exact),
            nullptr);
  LN_ManagerTestAccess::ActivatePending(manager);
  EXPECT_TRUE(LN_ManagerTestAccess::EventNamesForOwner(manager, owner).empty());
}

TEST(LN_Manager, DeferredConstraintBreakDropsDestroyedOwnerSafely)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);
  KX_GameObject owner;
  owner.SetName("Owner");

  LN_ManagerTestAccess::AddConstraintBreakEvent(manager, owner, nullptr, "joint");
  LN_ManagerTestAccess::QueueForUpdate(manager);
  LN_ManagerTestAccess::InvalidateObject(manager, owner);
  LN_ManagerTestAccess::ActivatePending(manager);

  EXPECT_TRUE(LN_ManagerTestAccess::EventNamesForOwner(manager, owner).empty());
  EXPECT_EQ(manager.FindRigidBodyConstraintBreakEvent(
                &owner, "joint", LN_RigidBodyConstraintMatchMode::Exact),
            nullptr);
}

TEST(LN_Manager, ConstraintBreakLookupKeepsOwnersAndDuplicateNamesIsolated)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);
  KX_GameObject owner_a;
  KX_GameObject owner_b;
  owner_a.SetName("OwnerA");
  owner_b.SetName("OwnerB");

  LN_ManagerTestAccess::AddConstraintBreakEvent(manager, owner_a, nullptr, "joint");
  LN_ManagerTestAccess::AddConstraintBreakEvent(manager, owner_b, nullptr, "joint");
  LN_ManagerTestAccess::AddConstraintBreakEvent(manager, owner_a, nullptr, "joint");

  EXPECT_EQ((LN_ManagerTestAccess::EventNamesForOwner(manager, owner_a)),
            (std::vector<std::string>{"joint", "joint"}));
  EXPECT_EQ((LN_ManagerTestAccess::EventNamesForOwner(manager, owner_b)),
            (std::vector<std::string>{"joint"}));
  EXPECT_NE(manager.FindRigidBodyConstraintBreakEvent(
                &owner_a, "joint", LN_RigidBodyConstraintMatchMode::Exact),
            nullptr);
  EXPECT_NE(manager.FindRigidBodyConstraintBreakEvent(
                &owner_b, "joint", LN_RigidBodyConstraintMatchMode::Exact),
            nullptr);
}

TEST(LN_Manager, DeferredConstraintBreakRetainsEventWhenOnlyTargetIsDestroyed)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);
  KX_GameObject owner;
  KX_GameObject target;
  owner.SetName("Owner");
  target.SetName("Target");

  LN_ManagerTestAccess::AddConstraintBreakEvent(manager, owner, &target, "joint");
  LN_ManagerTestAccess::QueueForUpdate(manager);
  LN_ManagerTestAccess::InvalidateObject(manager, target);
  LN_ManagerTestAccess::ActivatePending(manager);

  const LN_RigidBodyConstraintBreakEvent *event =
      manager.FindRigidBodyConstraintBreakEvent(
          &owner, "joint", LN_RigidBodyConstraintMatchMode::Exact);
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(manager.ResolveObjectHandle(event->target), nullptr);
}

}  // namespace

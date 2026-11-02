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
#include "LN_RuntimeTree.h"
#include "LN_TestTrees.h"
#include "LN_Types.h"

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

  static size_t TreeCount(const LN_Manager &manager)
  {
    return manager.m_runtimeTrees.size();
  }

  static size_t WaitingRunOnceCount(const LN_Manager &manager)
  {
    return manager.m_pendingRunOnce.size();
  }

  /** What a Logic + Physics step does with the waiting Run Logic Tree calls. */
  static uint32_t RunWaitingRunOnceCalls(LN_Manager &manager)
  {
    LN_TickContext context;
    context.event = LN_Event::OnFixedUpdate;
    context.use_fixed_timestep = true;
    context.tick_index = 1;
    context.delta_time = 1.0 / 60.0;
    context.unscaled_dt = 1.0 / 60.0;
    manager.m_commandBuffer.BeginRecording();
    const uint32_t passes = manager.ProcessPendingRunOnce(context, manager.m_runOnceSerial, nullptr);
    manager.m_commandBuffer.EndRecording();
    manager.m_commandBuffer.Clear();
    return passes;
  }

  static void RebuildRuntimeTreeIndexes(LN_Manager &manager)
  {
    manager.RebuildRuntimeTreeIndexes();
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

/* Run Logic Tree: a Logic + Physics step runs every call made before it began, however many. A
 * cap of 128 a step (2026-10-31, briefly) left the rest waiting, so a game making more calls than
 * that every step fell further behind at each step. */
TEST(LN_Manager, EveryWaitingRunLogicTreeCallRunsInTheNextStep)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);
  std::shared_ptr<LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 2.0f, 3.0f));
  std::vector<std::unique_ptr<KX_GameObject>> objects;
  for (uint32_t index = 0; index < 300; index++) {
    objects.push_back(std::make_unique<KX_GameObject>());
    objects.back()->SetName("Caller" + std::to_string(index));
    ASSERT_NE(manager.RegisterCompiledProgram(objects.back().get(), program, index, 0, true),
              nullptr);
  }
  for (const std::unique_ptr<KX_GameObject> &object : objects) {
    manager.QueueRunLogicTreeOnce(object.get(), program->GetSourceTreeName());
  }
  for (uint32_t call = 0; call < 100; call++) {
    manager.QueueRunLogicTreeOnce(objects.front().get(), program->GetSourceTreeName());
  }
  EXPECT_EQ(LN_ManagerTestAccess::WaitingRunOnceCount(manager), 400u);
  EXPECT_EQ(LN_ManagerTestAccess::RunWaitingRunOnceCalls(manager), 400u);
  EXPECT_EQ(LN_ManagerTestAccess::WaitingRunOnceCount(manager), 0u);
}

/* The waiting list is bounded (a route calling every frame of a long pause): calls beyond the
 * bound are ignored, the ones waiting all run at the next step. */
TEST(LN_Manager, RunLogicTreeKeepsABoundedNumberOfCallsWaiting)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*unused_scene);
  std::shared_ptr<LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 2.0f, 3.0f));
  KX_GameObject object;
  object.SetName("PausedCaller");
  ASSERT_NE(manager.RegisterCompiledProgram(&object, program, 0, 0, true), nullptr);
  for (size_t call = 0; call < LN_MAX_WAITING_RUN_ONCE_CALLS + 10; call++) {
    manager.QueueRunLogicTreeOnce(&object, program->GetSourceTreeName());
  }
  EXPECT_EQ(LN_ManagerTestAccess::WaitingRunOnceCount(manager), LN_MAX_WAITING_RUN_ONCE_CALLS);
  manager.UnregisterGameObject(&object);
  EXPECT_EQ(LN_ManagerTestAccess::WaitingRunOnceCount(manager), 0u);
}

/* Tree batches take only an object's one tree (BatchNote::sole_tree). The note follows each
 * object's tree list and survives a rebuild of the indexes, which also must not touch trees that
 * are gone: the manager's Clear deleted its trees first and then wrote into them (until
 * 2026-10-31, a use after free at every scene end). */
TEST(LN_Manager, ATreeIsNotedAsItsObjectsOnlyTreeAcrossIndexChanges)
{
  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  auto manager = std::make_unique<LN_Manager>(*unused_scene);
  std::shared_ptr<LN_Program> program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(1.0f, 2.0f, 3.0f));
  std::shared_ptr<LN_Program> second_program = LN_Program::CreateDebugSetWorldPosition(
      MT_Vector3(4.0f, 5.0f, 6.0f));
  KX_GameObject first;
  first.SetName("TwoTrees");
  KX_GameObject second;
  second.SetName("OneTree");
  LN_RuntimeTree *first_a = manager->RegisterCompiledProgram(&first, program, 0, 0, true);
  ASSERT_NE(first_a, nullptr);
  EXPECT_TRUE(first_a->GetBatchNote().sole_tree);
  LN_RuntimeTree *only = manager->RegisterCompiledProgram(&second, program, 1, 0, true);
  ASSERT_NE(only, nullptr);
  EXPECT_TRUE(only->GetBatchNote().sole_tree);
  LN_RuntimeTree *first_b = manager->RegisterCompiledProgram(&first, second_program, 0, 1, true);
  ASSERT_NE(first_b, nullptr);
  ASSERT_NE(first_a, first_b);
  EXPECT_FALSE(first_a->GetBatchNote().sole_tree);
  EXPECT_FALSE(first_b->GetBatchNote().sole_tree);
  EXPECT_TRUE(only->GetBatchNote().sole_tree);

  LN_ManagerTestAccess::RebuildRuntimeTreeIndexes(*manager);
  EXPECT_FALSE(first_a->GetBatchNote().sole_tree);
  EXPECT_FALSE(first_b->GetBatchNote().sole_tree);
  EXPECT_TRUE(only->GetBatchNote().sole_tree);

  manager->UnregisterGameObject(&first);
  EXPECT_TRUE(only->GetBatchNote().sole_tree);
  /* `second` is still registered: the manager's end deletes its tree and must not write into it
   * afterwards (a memory checker catches it). */
  manager.reset();
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

/* Load Scene merges another scene's trees into this one (lean runtime plan A2: a merge seam). The
 * scene's object lists stand in for a KX_Scene (LN_SetSceneAccessForTests). The merged trees
 * move over, Every N Steps takes turns with the target's own trees of the same source tree
 * (their spread numbers continue), and their steps start over. */
TEST(LN_Manager, AMergedScenesTreesTakeTurnsAndStartTheirStepsOver)
{
  ln_test::BrainOfOptions options;
  options.chains = 1;
  options.step_every = 4;
  options.measure_distance = false;
  const std::shared_ptr<LN_Program> program = ln_test::BrainOf(options);
  ASSERT_NE(program, nullptr);
  ASSERT_FALSE(program->GetStepGates().empty());

  KX_Scene *unused_scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager target(*unused_scene);
  LN_Manager other(*unused_scene);
  std::vector<std::unique_ptr<KX_GameObject>> target_objects;
  std::vector<std::unique_ptr<KX_GameObject>> other_objects;
  std::vector<LN_RuntimeTree *> other_trees;
  for (uint32_t index = 0; index < 3; index++) {
    target_objects.push_back(std::make_unique<KX_GameObject>());
    target_objects.back()->SetName("Mine" + std::to_string(index));
    ASSERT_NE(target.RegisterCompiledProgram(target_objects.back().get(), program, index, 0, true),
              nullptr);
  }
  for (uint32_t index = 0; index < 2; index++) {
    other_objects.push_back(std::make_unique<KX_GameObject>());
    other_objects.back()->SetName("Merged" + std::to_string(index));
    other_trees.push_back(
        other.RegisterCompiledProgram(other_objects.back().get(), program, index, 0, true));
    ASSERT_NE(other_trees.back(), nullptr);
  }
  /* The other scene had its own numbering and had counted steps. */
  EXPECT_EQ(other_trees[0]->GetStepSpread(), 0u);
  EXPECT_EQ(other_trees[1]->GetStepSpread(), 1u);
  for (LN_RuntimeTree *tree : other_trees) {
    tree->PrepareStepGates(7, 1.0 / 60.0, 1.0 / 60.0);
    EXPECT_NE(tree->DumpStateForTests(0).bookkeeping.find("step_gates_step=7"), std::string::npos);
  }
  EXPECT_EQ(LN_ManagerTestAccess::TreeCount(target), 3u);
  EXPECT_EQ(LN_ManagerTestAccess::TreeCount(other), 2u);

  /* The merged scene's objects join the target's lists. */
  LN_SceneAccessForTests access;
  for (const auto &object : target_objects) {
    access.objects.push_back(object.get());
  }
  for (const auto &object : other_objects) {
    access.objects.push_back(object.get());
  }
  LN_SetSceneAccessForTests(&access);
  target.AbsorbRuntimeTreesFrom(other);
  LN_SetSceneAccessForTests(nullptr);

  EXPECT_EQ(LN_ManagerTestAccess::TreeCount(target), 5u);
  EXPECT_EQ(LN_ManagerTestAccess::TreeCount(other), 0u);
  EXPECT_EQ(other_trees[0]->GetStepSpread(), 3u); /* after the target's 0, 1, 2 */
  EXPECT_EQ(other_trees[1]->GetStepSpread(), 4u);
  for (LN_RuntimeTree *tree : other_trees) {
    EXPECT_TRUE(tree->HasStepSpread());
    EXPECT_NE(tree->DumpStateForTests(0).bookkeeping.find("step_gates_step=-"), std::string::npos)
        << "the steps start over";
    EXPECT_EQ(tree->GetGameObject(), other_objects[tree == other_trees[0] ? 0 : 1].get());
  }
}

}  // namespace

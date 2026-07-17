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

namespace {

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

}  // namespace

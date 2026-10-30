/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameStateLogicTrees.cpp
 *  \ingroup logicnodes
 *
 * Exact, transactional restoration of runtime C++ Logic Node tree attachments.
 */

#include "LN_Manager.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "LN_Program.h"
#include "LN_RuntimeTree.h"

#include "KX_GameObject.h"

struct LN_PreparedRuntimeTreeRestore::Impl {
  struct ReusedTree {
    LN_RuntimeTree *runtime_tree = nullptr;
    bool enabled = true;
    bool runtime_active = false;
  };

  LN_Manager *manager = nullptr;
  std::unordered_set<KX_GameObject *> excluded_objects;
  std::unordered_set<LN_RuntimeTree *> reused_tree_set;
  std::unordered_map<KX_GameObject *, bool> runtime_active;
  std::vector<KX_GameObject *> created_empty_object_indexes;
  std::vector<ReusedTree> reused_trees;
  std::vector<std::unique_ptr<LN_RuntimeTree>> runtime_trees;
  bool committed = false;
};

LN_PreparedRuntimeTreeRestore::LN_PreparedRuntimeTreeRestore()
    : m_impl(std::make_unique<Impl>())
{
}

LN_PreparedRuntimeTreeRestore::~LN_PreparedRuntimeTreeRestore()
{
  Release();
}

LN_PreparedRuntimeTreeRestore::LN_PreparedRuntimeTreeRestore(
    LN_PreparedRuntimeTreeRestore &&other) noexcept
    : m_impl(std::move(other.m_impl))
{
}

LN_PreparedRuntimeTreeRestore &LN_PreparedRuntimeTreeRestore::operator=(
    LN_PreparedRuntimeTreeRestore &&other) noexcept
{
  if (this != &other) {
    Release();
    m_impl = std::move(other.m_impl);
  }
  return *this;
}

void LN_PreparedRuntimeTreeRestore::Release()
{
  if (m_impl == nullptr) {
    return;
  }
  if (!m_impl->committed && m_impl->manager != nullptr) {
    for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_impl->runtime_trees) {
      if (runtime_tree != nullptr) {
        m_impl->manager->m_denseIds.InvalidateRuntimeTree(runtime_tree.get());
      }
    }
    for (KX_GameObject *gameobj : m_impl->created_empty_object_indexes) {
      const auto found = m_impl->manager->m_runtimeTreesByObject.find(gameobj);
      if (found != m_impl->manager->m_runtimeTreesByObject.end() &&
          found->second.empty())
      {
        m_impl->manager->m_runtimeTreesByObject.erase(found);
      }
    }
  }
  m_impl.reset();
}

void LN_PreparedRuntimeTreeRestore::Reset()
{
  Release();
  m_impl = std::make_unique<Impl>();
}

LN_GameStateResult LN_Manager::PrepareGameStateRuntimeTreeRestore(
    const std::vector<LN_RuntimeTreeRestoreTarget> &targets,
    const std::vector<KX_GameObject *> &removed_objects,
    LN_PreparedRuntimeTreeRestore &r_prepared,
    std::string &r_error)
{
  AssertMainThread();
  r_error.clear();
  r_prepared.Reset();
  LN_PreparedRuntimeTreeRestore::Impl &prepared = *r_prepared.m_impl;
  prepared.manager = this;

  if (m_isTicking || m_isShuttingDown) {
    r_error = "C++ Logic Node attachment restore is unavailable while the manager is active";
    return LN_GameStateResult::Busy;
  }

  try {
    prepared.excluded_objects.reserve(targets.size() + removed_objects.size());
    prepared.reused_tree_set.reserve(targets.size());
    prepared.runtime_active.reserve(targets.size());
    size_t attachment_count = 0;
    for (KX_GameObject *gameobj : removed_objects) {
      if (gameobj == nullptr || !prepared.excluded_objects.insert(gameobj).second) {
        r_error = "C++ Logic Node restore removal set contains an invalid or duplicate object";
        r_prepared.Reset();
        return LN_GameStateResult::InternalError;
      }
    }
    for (const LN_RuntimeTreeRestoreTarget &target : targets) {
      if (target.gameobj == nullptr ||
          !prepared.excluded_objects.insert(target.gameobj).second ||
          IsGameObjectQueuedForRemoval(target.gameobj))
      {
        r_error = "C++ Logic Node restore target is invalid, duplicate, or pending removal";
        r_prepared.Reset();
        return LN_GameStateResult::InternalError;
      }
      if (attachment_count >
          std::numeric_limits<size_t>::max() - target.attachments.size())
      {
        r_error = "C++ Logic Node attachment count overflowed";
        r_prepared.Reset();
        return LN_GameStateResult::CorruptData;
      }
      attachment_count += target.attachments.size();
    }
    prepared.runtime_trees.reserve(attachment_count);
    prepared.reused_trees.reserve(attachment_count);

    for (const LN_RuntimeTreeRestoreTarget &target : targets) {
      bool runtime_active = false;
      const int scene_object_index = FindSceneObjectIndex(target.gameobj, &runtime_active);
      if (scene_object_index < 0) {
        r_error = "C++ Logic Node restore target is outside the runtime scene";
        r_prepared.Reset();
        return LN_GameStateResult::InternalError;
      }
      prepared.runtime_active.emplace(target.gameobj, runtime_active);

      uint32_t previous_applied_index = 0;
      bool has_previous_applied_index = false;
      for (const LN_RuntimeTreeAttachment &attachment : target.attachments) {
        if (attachment.tree_name.empty() ||
            (has_previous_applied_index &&
             attachment.applied_tree_index <= previous_applied_index))
        {
          r_error = "saved C++ Logic Node attachments are not in strict applied-index order";
          r_prepared.Reset();
          return LN_GameStateResult::CorruptData;
        }
        previous_applied_index = attachment.applied_tree_index;
        has_previous_applied_index = true;

        std::shared_ptr<const LN_Program> program = EnsureCompiledProgram(
            attachment.tree_name, attachment.library_path);
        if (program == nullptr ||
            program->GetSourceTreeName() != attachment.tree_name ||
            program->GetSourceTreeLibraryPath() != attachment.library_path)
        {
          r_error = "saved LogicNodeTree '" + attachment.tree_name +
                    "' is missing or incompatible with the current game build";
          r_prepared.Reset();
          return LN_GameStateResult::Incompatible;
        }

        LN_RuntimeTree *existing =
            FindRuntimeTree(target.gameobj, attachment.applied_tree_index);
        std::shared_ptr<const LN_Program> existing_program =
            existing != nullptr ? existing->GetProgram() : nullptr;
        if (existing != nullptr && existing_program != nullptr &&
            existing_program->MatchesSource(program->GetSourceTreeName(),
                                            program->GetSourceTreeLibraryPath(),
                                            program->GetSourceChecksum()))
        {
          prepared.reused_tree_set.insert(existing);
          prepared.reused_trees.push_back(
              {existing, attachment.enabled, runtime_active});
          continue;
        }

        auto runtime_tree = std::make_unique<LN_RuntimeTree>(
            program,
            target.gameobj,
            uint32_t(scene_object_index),
            attachment.applied_tree_index);
        runtime_tree->SetLogicManager(this);
        runtime_tree->SetEnabled(attachment.enabled);
        runtime_tree->SetRuntimeActive(false);
        runtime_tree->BindDenseIds(m_denseIds);
        CacheCompiledProgram(program);
        m_commandBuffer.PrewarmMaterialParameterBindings(*runtime_tree);
        prepared.runtime_trees.push_back(std::move(runtime_tree));
      }
    }

    size_t excluded_tree_count = 0;
    for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
      if (runtime_tree != nullptr &&
          prepared.excluded_objects.contains(runtime_tree->GetGameObject()) &&
          !prepared.reused_tree_set.contains(runtime_tree.get()))
      {
        excluded_tree_count++;
      }
    }
    const size_t new_tree_count = prepared.runtime_trees.size();
    if (excluded_tree_count > m_runtimeTrees.size() ||
        new_tree_count >
            std::numeric_limits<size_t>::max() -
                (m_runtimeTrees.size() - excluded_tree_count))
    {
      r_error = "final C++ Logic Node attachment count overflowed";
      r_prepared.Reset();
      return LN_GameStateResult::InternalError;
    }
    const size_t final_tree_count =
        m_runtimeTrees.size() - excluded_tree_count + new_tree_count;
    m_runtimeTrees.reserve(final_tree_count);
    m_runtimeTreeByObjectAndAppliedIndex.reserve(final_tree_count);
    m_runtimeTreesByObject.reserve(m_runtimeTreesByObject.size() + targets.size());

    for (const LN_RuntimeTreeRestoreTarget &target : targets) {
      auto [found, inserted] = m_runtimeTreesByObject.try_emplace(target.gameobj);
      if (inserted) {
        prepared.created_empty_object_indexes.push_back(target.gameobj);
      }
      found->second.reserve(target.attachments.size());
    }
  }
  catch (const std::exception &exception) {
    r_error = std::string("could not prepare C++ Logic Node attachment restore: ") +
              exception.what();
    r_prepared.Reset();
    return LN_GameStateResult::InternalError;
  }
  catch (...) {
    r_error = "could not prepare C++ Logic Node attachment restore";
    r_prepared.Reset();
    return LN_GameStateResult::InternalError;
  }
  return LN_GameStateResult::Success;
}

void LN_Manager::CommitGameStateRuntimeTreeRestore(
    LN_PreparedRuntimeTreeRestore &prepared_restore)
{
  AssertMainThread();
  BLI_assert(!m_isTicking);
  BLI_assert(prepared_restore.m_impl != nullptr);
  LN_PreparedRuntimeTreeRestore::Impl &prepared = *prepared_restore.m_impl;
  BLI_assert(prepared.manager == this);
  BLI_assert(!prepared.committed);

  std::erase_if(m_pendingRunOnce,
                [&prepared](const std::pair<KX_GameObject *, std::string> &entry) {
                  return prepared.excluded_objects.contains(entry.first);
                });
  for (KX_GameObject *gameobj : prepared.excluded_objects) {
    m_commandBuffer.RemoveCommandsForGameObject(gameobj);
  }

  for (const std::unique_ptr<LN_RuntimeTree> &runtime_tree : m_runtimeTrees) {
    if (runtime_tree == nullptr ||
        !prepared.excluded_objects.contains(runtime_tree->GetGameObject()) ||
        prepared.reused_tree_set.contains(runtime_tree.get()))
    {
      continue;
    }
    UnindexRuntimeTree(runtime_tree.get());
    UnregisterRuntimeTreeObjectRefs(runtime_tree.get());
    m_denseIds.InvalidateRuntimeTree(runtime_tree.get());
    runtime_tree->DetachGameObject();
  }
  m_runtimeTrees.erase(
      std::remove_if(m_runtimeTrees.begin(),
                     m_runtimeTrees.end(),
                     [](const std::unique_ptr<LN_RuntimeTree> &runtime_tree) {
                       return runtime_tree == nullptr || !runtime_tree->IsAttached();
                     }),
      m_runtimeTrees.end());

  for (const LN_PreparedRuntimeTreeRestore::Impl::ReusedTree &reused :
       prepared.reused_trees)
  {
    BLI_assert(reused.runtime_tree != nullptr && reused.runtime_tree->IsAttached());
    reused.runtime_tree->SetEnabled(reused.enabled);
    reused.runtime_tree->SetRuntimeActive(reused.runtime_active);
  }
  for (std::unique_ptr<LN_RuntimeTree> &runtime_tree : prepared.runtime_trees) {
    const auto active = prepared.runtime_active.find(runtime_tree->GetGameObject());
    BLI_assert(active != prepared.runtime_active.end());
    runtime_tree->SetRuntimeActive(active->second);
    IndexRuntimeTree(runtime_tree.get());
    m_runtimeTrees.push_back(std::move(runtime_tree));
  }
  prepared.runtime_trees.clear();
  prepared.reused_trees.clear();
  prepared.reused_tree_set.clear();
  prepared.created_empty_object_indexes.clear();
  prepared.committed = true;
  prepared.manager = nullptr;
}

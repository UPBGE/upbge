/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Manager.h
 *  \ingroup logicnodes
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "LN_CommandBuffer.h"
#include "LN_DenseIds.h"
#include "LN_EventBus.h"
#include "LN_ParallelTreeExecutor.h"
#include "LN_Snapshot.h"

class KX_GameObject;
class KX_Scene;
class LN_Program;
class LN_RuntimeTree;

struct LN_TickContext;
class LN_Manager {
 public:
  explicit LN_Manager(KX_Scene &scene);
  ~LN_Manager();

  LN_RuntimeTree *RegisterCompiledProgram(KX_GameObject *gameobj,
                                          std::shared_ptr<const LN_Program> program,
                                          uint32_t scene_object_index,
                                          uint32_t applied_tree_index,
                                          bool enabled,
                                          bool runtime_active = true);

  bool IsLogicTreeRunning(KX_GameObject *gameobj, const std::string &tree_name) const;
  void QueueRunLogicTreeOnce(KX_GameObject *target_object, const std::string &tree_name);

  void NotifyGameObjectQueuedForRemoval(KX_GameObject *gameobj);
  void NotifyGameObjectsQueuedForBulkRemoval(const std::vector<KX_GameObject *> &gameobjects);
  void UnregisterGameObject(KX_GameObject *gameobj);
  void InvalidateRuntimeRefs(LN_RuntimeRefKind kind, const void *pointer);
  void ClearPendingRuntimeWork();
  void BeginShutdown();
  void ReplicateRuntimeTrees(KX_GameObject *source_gameobj,
                             KX_GameObject *replica_gameobj,
                             uint32_t scene_object_index);
  void AbsorbRuntimeTreesFrom(LN_Manager &other);
  void Tick(double current_time, double fixed_dt, bool use_fixed_timestep);
  void Clear();

  bool HasRuntimeTrees() const
  {
    return !m_runtimeTrees.empty();
  }

  size_t GetCachedProgramCountForTests() const
  {
    return m_cachedPrograms.size();
  }

  void FlushSetLogicTreeEnabled(KX_GameObject *target_object,
                                const std::string &tree_name,
                                bool enabled);
  void FlushInstallLogicTree(KX_GameObject *target_object,
                             const std::string &tree_name,
                             bool initial_enabled);

  void PushEvent(LN_EventEntry event);
  const LN_EventBus &GetEventBus() const;
  LN_EventSubjectId InternEventSubject(const std::string &subject);
  LN_GamePropertyId InternGamePropertyName(const std::string &name);
  LN_SoundId InternSoundName(const std::string &name);
  LN_ObjectHandle MakeObjectHandle(KX_GameObject *gameobj, const std::string &debug_name = "");
  const std::string &DebugName(LN_EventSubjectId id) const;
  const std::string &DebugName(LN_GamePropertyId id) const;
  const std::string &DebugName(LN_SoundId id) const;

  /** Per-tick object name lookup (valid only while `m_isTicking`). */
  KX_GameObject *FindObjectByName(const std::string &name);

 private:
  struct RuntimeTreeLookupKey {
    KX_GameObject *gameobj = nullptr;
    uint32_t applied_tree_index = 0;

    bool operator==(const RuntimeTreeLookupKey &other) const
    {
      return gameobj == other.gameobj && applied_tree_index == other.applied_tree_index;
    }
  };

  struct RuntimeTreeLookupKeyHash {
    size_t operator()(const RuntimeTreeLookupKey &key) const
    {
      const size_t object_hash = std::hash<KX_GameObject *>{}(key.gameobj);
      const size_t index_hash = std::hash<uint32_t>{}(key.applied_tree_index);
      return object_hash ^ (index_hash + 0x9e3779b9u + (object_hash << 6u) + (object_hash >> 2u));
    }
  };

  LN_RuntimeTree *RegisterDebugHardcodedSetWorldPosition(KX_GameObject *gameobj,
                                                         const MT_Vector3 &position);
  void RegisterDebugPropertyBindings();
  void CacheCompiledProgram(const std::shared_ptr<const LN_Program> &program);
  std::shared_ptr<const LN_Program> EnsureCompiledProgram(const std::string &tree_name);
  LN_RuntimeTree *FindRuntimeTreeBySourceName(KX_GameObject *gameobj,
                                              const std::string &tree_name) const;
  uint32_t AllocateNextAppliedTreeIndex(KX_GameObject *gameobj) const;
  void ProcessPendingRunOnce(std::vector<LN_CommandBuffer::RecordedCommandList> &command_lists,
                             const LN_TickContext &context,
                             bool collect_profile_counters);
  void CaptureSharedTickReadContext(uint32_t input_channels,
                                    double fixed_dt,
                                    uint64_t tick_index);
  void RebuildObjectNameCache();

  friend class LN_RuntimeTree;

  LN_RuntimeTree *FindRuntimeTree(KX_GameObject *gameobj, uint32_t applied_tree_index) const;
  void IndexRuntimeTree(LN_RuntimeTree *runtime_tree);
  void UnindexRuntimeTree(LN_RuntimeTree *runtime_tree);
  void ClearRuntimeTreeIndexes();
  void RebuildRuntimeTreeIndexes();
  void IndexRuntimeTreeObjectRefs(LN_RuntimeTree *runtime_tree);
  void RegisterRuntimeObjectRef(LN_RuntimeTree *runtime_tree, KX_GameObject *referenced_object);
  void UnregisterRuntimeObjectRef(LN_RuntimeTree *runtime_tree, KX_GameObject *referenced_object);
  void UnregisterRuntimeTreeObjectRefs(LN_RuntimeTree *runtime_tree);
  void DetachRuntimeTreesForGameObject(KX_GameObject *gameobj);
  void RemoveDetachedRuntimeTrees();
  void AddQueuedRemovalObject(KX_GameObject *gameobj);
  void RemoveQueuedRemovalObject(KX_GameObject *gameobj);
  bool IsGameObjectQueuedForRemoval(KX_GameObject *gameobj) const;
  int FindSceneObjectIndex(KX_GameObject *gameobj, bool *r_runtime_active = nullptr) const;
  void AssertMainThread() const;

  KX_Scene &m_scene;
  std::unordered_map<std::string, std::shared_ptr<const LN_Program>> m_programBySourceTreeName;
  std::unordered_set<const LN_Program *> m_validatedPrograms;
  std::unordered_set<const LN_Program *> m_cachedPrograms;
  std::deque<std::pair<KX_GameObject *, std::string>> m_pendingRunOnce;
  std::vector<std::unique_ptr<LN_RuntimeTree>> m_runtimeTrees;
  std::unordered_map<RuntimeTreeLookupKey, LN_RuntimeTree *, RuntimeTreeLookupKeyHash>
      m_runtimeTreeByObjectAndAppliedIndex;
  std::unordered_map<KX_GameObject *, std::vector<LN_RuntimeTree *>> m_runtimeTreesByObject;
  std::unordered_map<KX_GameObject *, std::vector<LN_RuntimeTree *>>
      m_runtimeTreesByReferencedObject;
  std::unordered_map<LN_RuntimeTree *, std::vector<KX_GameObject *>>
      m_referencedObjectsByRuntimeTree;
  std::mutex m_runtimeObjectRefIndexMutex;
  std::vector<KX_GameObject *> m_queuedRemovalObjects;
  LN_CommandBuffer m_commandBuffer;
  std::thread::id m_mainThreadId;
  uint64_t m_tickIndex = 0;
  bool m_isTicking = false;
  bool m_isShuttingDown = false;
  bool m_hasDetachedRuntimeTrees = false;
  bool m_debugBindingsScanned = false;
  bool m_reportedVariableTimestepSkip = false;
  LN_EventBus m_eventBus;
  LN_InputSnapshot m_sharedTickInput;
  LN_TickReadContext m_sharedTickReadContext;
  bool m_hasSharedTickInput = false;
  std::vector<LN_ParallelTreeExecutor::RuntimeTreeWorkItem> m_tickRunnableTreesScratch;
  std::unordered_map<std::string, KX_GameObject *> m_objectByName;
  bool m_objectNameCacheValid = false;
  LN_DenseIdRegistry m_denseIds;
  bool m_profileTickMetricsEnabled = false;
  mutable std::atomic<uint64_t> m_tickHotMapLookupCount = 0;
};

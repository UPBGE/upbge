/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_GameState.h
 *  \ingroup logicnodes
 */

#pragma once

#include "LN_DenseIds.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class KX_GameObject;
class KX_Scene;
enum class KX_ObjectRemovalReason : uint8_t;

enum class LN_GameStateOperation : uint8_t {
  Save = 0,
  Load,
};

enum class LN_GameStateResult : uint8_t {
  Success = 0,
  Cancelled,
  InvalidRequest,
  PathUnavailable,
  NotFound,
  IoError,
  CorruptData,
  Incompatible,
  UnsupportedContent,
  Busy,
  InternalError,
};

inline bool LN_GameStateSucceeded(const LN_GameStateResult result)
{
  return result == LN_GameStateResult::Success;
}

struct LN_GameStateCompletionTarget {
  LN_RuntimeTreeId runtime_tree_id;
  uint64_t program_revision = 0;
  uint32_t expression_index = LN_INVALID_INDEX;
  uint32_t source_ref_index = LN_INVALID_INDEX;
};

/**
 * Move-only decoded checkpoint handed from a source scene to the engine's deferred scene
 * replacement transaction. The checkpoint model remains private so scene switching cannot bypass
 * the Save Game codec or mutate checkpoint data between validation and reconciliation.
 */
class LN_GameStateLoadPayload {
 public:
  ~LN_GameStateLoadPayload();

  LN_GameStateLoadPayload(LN_GameStateLoadPayload &&) noexcept;
  LN_GameStateLoadPayload &operator=(LN_GameStateLoadPayload &&) noexcept;

  LN_GameStateLoadPayload(const LN_GameStateLoadPayload &) = delete;
  LN_GameStateLoadPayload &operator=(const LN_GameStateLoadPayload &) = delete;

  const std::string &GetSceneUuid() const;
  const std::string &GetSceneName() const;

 private:
  class Impl;
  explicit LN_GameStateLoadPayload(std::unique_ptr<Impl> impl);

  friend class LN_GameStateService;
  std::unique_ptr<Impl> m_impl;
};

/**
 * Explicit Save Game data I/O.
 *
 * Save Object State captures existence, transforms, hierarchy, semantic physics state, materials,
 * exact runtime C++ Logic Node tree attachments, and bounded Geometry Nodes modifier and node-value
 * state. The active scene compositor's node values and mute state are captured once per checkpoint.
 * Material state includes object slots, object-local parameters, and runtime-editable values of
 * referenced shared material assets. Shader graphs, Geometry Nodes/compositor graph topology, and
 * non-Geometry modifier definitions remain authored by the current game build; unsupported runtime
 * structural edits are rejected instead of being silently omitted. Normal C++ Logic Nodes Add
 * Object roots whose runtime Save Object State value is enabled are tracked by durable
 * source/instance identities. Explicitly marked scalar game properties remain independently
 * persistent. JSON serialization and atomic disk I/O run on one ordered worker so Save -> Load
 * requests retain command order without blocking simulation.
 */
class LN_GameStateService {
 public:
  using CompletionFn = std::function<void(const LN_GameStateCompletionTarget &target,
                                          LN_GameStateOperation operation,
                                          const std::string &filepath,
                                          LN_GameStateResult result,
                                          const std::string &error)>;
  using SceneTransitionFn = std::function<LN_GameStateResult(
      const LN_GameStateCompletionTarget &target,
      const std::string &filepath,
      std::unique_ptr<LN_GameStateLoadPayload> payload,
      std::string &r_error)>;

  LN_GameStateService();
  ~LN_GameStateService();

  LN_GameStateService(const LN_GameStateService &) = delete;
  LN_GameStateService &operator=(const LN_GameStateService &) = delete;

  bool RequestSave(KX_Scene &scene,
                   int32_t slot,
                   const std::string &path,
                   const LN_GameStateCompletionTarget &target,
                   std::string &r_error);
  bool RequestLoad(int32_t slot,
                   const std::string &path,
                   const LN_GameStateCompletionTarget &target,
                   std::string &r_error);

  /** Build the scene-owned checkpoint registry once, after conversion has fully completed. */
  LN_GameStateResult InitializeRegistry(KX_Scene &scene, std::string &r_error);

  /** Validate/register one normal C++ Logic Nodes Add Object operation. */
  LN_GameStateResult PreflightAddObject(KX_Scene &scene,
                                        KX_GameObject &source,
                                        float life_seconds,
                                        bool full_copy,
                                        std::string &r_error) const;
  LN_GameStateResult RegisterAddedObject(KX_Scene &scene,
                                         KX_GameObject &source,
                                         KX_GameObject &root,
                                         const std::vector<KX_GameObject *> &members,
                                         float life_seconds,
                                         bool full_copy,
                                         std::string &r_error);
  LN_GameStateResult SetSaveExistence(KX_Scene &scene,
                                      KX_GameObject *root,
                                      bool enabled,
                                      std::string &r_error);
  void NotifyObjectRemoved(KX_GameObject &object, KX_ObjectRemovalReason reason);

  /**
   * Apply completed loads and deliver completions. A decoded checkpoint for another scene is
   * transferred exactly once through `scene_transition_fn`; accepted transfers intentionally do
   * not complete a node in the source scene because that runtime tree is about to be destroyed.
   * Must be called on the game thread.
   */
  void Poll(KX_Scene &scene,
            const CompletionFn &completion_fn,
            const SceneTransitionFn &scene_transition_fn = SceneTransitionFn());

  /** Reconcile a checkpoint transferred by the engine into its fully converted destination. */
  LN_GameStateResult ApplyTransferredLoad(KX_Scene &scene,
                                          const LN_GameStateLoadPayload &payload,
                                          std::string &r_error);

  /** Cancel queued work and report terminal failure for every still-live completion target. */
  void CancelPending(const CompletionFn &completion_fn = CompletionFn());

  /** Resolve the node's directory-or-file Path input to an absolute Save Game filename. */
  static bool ResolveFilePath(int32_t slot,
                              const std::string &path,
                              const std::string &scene_uuid,
                              const std::string &user_data_root,
                              std::string &r_filepath,
                              std::string &r_error);

 private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

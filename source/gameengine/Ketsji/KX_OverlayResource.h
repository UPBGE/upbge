/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file KX_OverlayResource.h
 *  \ingroup ketsji
 *
 * Scene-local overlay resource ownership. This registry never instantiates render objects; it
 * records the independent enabled request, per-menu leases, creator visibility, and membership
 * generation so Logic Nodes, the Collection Actuator, and Python share one lifetime policy.
 *
 * Visual layer/order metadata is intentionally omitted: the current game-engine overlay path is
 * one camera and one overlay pass. Creator-facing layer controls stay unshipped until a
 * game-engine-only prototype can isolate depth without changing Blender graphics code.
 */

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

class KX_Camera;

namespace blender {
struct Collection;
}

/** Result of a runtime overlay-collection state request. */
enum class KX_OverlayCollectionResult : uint8_t {
  Success = 0,
  NoChange,
  MissingCamera,
  MissingCollection,
  CameraWrongScene,
  CameraUnavailable,
  CameraConflict,
  CollectionWrongScene,
  CollectionSharedAcrossScenes,
  CollectionHierarchyConflict,
  CollectionOverlap,
  CollectionHasNoRuntimeObjects,
  RuntimeUnavailable,
  ObjectInstantiationFailed,
};

bool KX_OverlayCollectionResultSucceeded(KX_OverlayCollectionResult result);
const char *KX_OverlayCollectionResultMessage(KX_OverlayCollectionResult result);

class KX_OverlayResourceRegistry {
 public:
  using MenuHandle = int32_t;

  bool Empty() const
  {
    return m_entries.empty();
  }

  size_t ResourceCount() const
  {
    return m_entries.size();
  }

  bool Has(const blender::Collection *collection) const;
  bool IndependentEnabled(const blender::Collection *collection) const;
  bool HasAnyLease(const blender::Collection *collection) const;
  bool HasLease(const blender::Collection *collection, MenuHandle handle) const;
  /** That menu's lease is shown (not covered and hidden). */
  bool LeaseVisible(const blender::Collection *collection, MenuHandle handle) const;
  bool CreatorVisible(const blender::Collection *collection) const;
  bool ShouldExist(const blender::Collection *collection) const;
  /** True if this entry stays alive after `handle` is released. */
  bool WouldExistWithoutLease(const blender::Collection *collection, MenuHandle handle) const;
  bool ShouldRender(const blender::Collection *collection) const;
  uint64_t MembershipGeneration(const blender::Collection *collection) const;
  uint64_t InsertionSequence(const blender::Collection *collection) const;
  bool PresentationRequested(const blender::Collection *collection) const;
  KX_Camera *EstablishedCamera() const
  {
    return m_establishedCamera;
  }
  const std::vector<blender::Collection *> &Collections() const
  {
    return m_order;
  }

  /** Repeating an already-enabled independent request is NoChange. */
  KX_OverlayCollectionResult ValidateIndependentEnable(
      const blender::Collection *collection) const;
  /** Repeating disable when the independent request is already off is NoChange. */
  KX_OverlayCollectionResult ValidateIndependentDisable(
      const blender::Collection *collection) const;

  void SetEstablishedCamera(KX_Camera *camera);
  void Create(blender::Collection *collection);
  void SetIndependentEnabled(blender::Collection *collection, bool enabled);
  void AddLease(blender::Collection *collection, MenuHandle handle, bool visible);
  void RemoveLease(blender::Collection *collection, MenuHandle handle);
  void SetLeaseVisible(blender::Collection *collection, MenuHandle handle, bool visible);
  void SetCreatorVisible(blender::Collection *collection, bool visible);
  void NoteMembershipChanged(blender::Collection *collection);
  void Erase(blender::Collection *collection);
  void Clear();

 private:
  struct Lease {
    MenuHandle handle = 0;
    bool visible = true;
  };

  struct Entry {
    bool independent_enabled = false;
    bool creator_visible = true;
    uint64_t membership_generation = 1;
    uint64_t insertion_sequence = 0;
    std::vector<Lease> leases;
  };

  Entry *Find(blender::Collection *collection);
  const Entry *Find(const blender::Collection *collection) const;
  bool PresentationVisible(const Entry &entry) const;

  std::unordered_map<blender::Collection *, Entry> m_entries;
  std::vector<blender::Collection *> m_order;
  KX_Camera *m_establishedCamera = nullptr;
  uint64_t m_nextInsertionSequence = 1;
};

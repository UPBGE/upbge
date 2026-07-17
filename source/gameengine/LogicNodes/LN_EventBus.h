/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_EventBus.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "LN_DenseIds.h"
#include "LN_Types.h"

class KX_GameObject;

enum class LN_EventPayloadLane : uint8_t {
  None = 0,
  Bool,
  Int,
  Float,
  Vector,
  Vector4,
  Matrix,
  Color,
  Rotation,
  String,
  Object,
  Scene,
  Collection,
  Datablock,
  List,
  Dict,
  GenericValue,
};

struct LN_EventEntry {
  LN_EventSubjectId subject_id;
  LN_ObjectHandle messenger_handle;
  LN_ObjectHandle target_handle;
  std::string subject;
  LN_Value content;
  LN_EventPayloadLane payload_lane = LN_EventPayloadLane::None;
  KX_GameObject *messenger = nullptr;
  KX_GameObject *target = nullptr;
};

class LN_EventBus {
 public:
  void Publish(LN_EventEntry event);
  void BeginTick();
  void ClearAll();

  const std::vector<LN_EventEntry> &PendingEvents() const
  {
    return m_pendingEvents;
  }

  const std::vector<LN_EventEntry> &TickEvents() const
  {
    return m_tickEvents;
  }

  uint32_t TickEventCount() const
  {
    return uint32_t(m_tickEvents.size());
  }

  const LN_EventEntry *FindFirst(LN_EventSubjectId subject_id,
                                 const std::string &subject,
                                 const LN_ObjectHandle &target_handle,
                                 const KX_GameObject *target) const;

  uint64_t IndexedLookupCount() const
  {
    return m_indexedLookupCount;
  }

  uint64_t FallbackScanCount() const
  {
    return m_fallbackScanCount;
  }

 private:
  void RebuildIndexes();
  bool EntryMatchesFilter(const LN_EventEntry &event,
                          LN_EventSubjectId subject_id,
                          const std::string &subject,
                          const LN_ObjectHandle &target_handle,
                          const KX_GameObject *target) const;

  std::vector<LN_EventEntry> m_pendingEvents;
  std::vector<LN_EventEntry> m_tickEvents;
  std::unordered_map<uint32_t, std::vector<uint32_t>> m_subjectIndex;
  std::unordered_map<uint64_t, std::vector<uint32_t>> m_targetIndex;
  std::vector<uint32_t> m_broadcastEventIndices;
  mutable uint64_t m_indexedLookupCount = 0;
  mutable uint64_t m_fallbackScanCount = 0;
};

LN_EventPayloadLane LN_EventPayloadLaneForValue(const LN_Value &value);

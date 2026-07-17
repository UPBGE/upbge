/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_EventBus.cpp
 *  \ingroup logicnodes
 */

#include "LN_EventBus.h"

#include <utility>

namespace {

bool logic_nodes_handle_matches(const LN_ObjectHandle &a, const LN_ObjectHandle &b)
{
  return a.IsValid() && b.IsValid() && a.index == b.index && a.generation == b.generation;
}

uint64_t logic_nodes_target_index_key(const LN_ObjectHandle &handle)
{
  return (uint64_t(handle.index) << 32u) | uint64_t(handle.generation);
}

bool logic_nodes_subject_matches(const LN_EventEntry &event,
                                 const LN_EventSubjectId subject_id,
                                 const std::string &subject)
{
  if (subject_id.IsValid() && event.subject_id.IsValid()) {
    return event.subject_id.index == subject_id.index;
  }
  return event.subject == subject;
}

bool logic_nodes_target_matches(const LN_EventEntry &event,
                                const LN_ObjectHandle &target_handle,
                                const KX_GameObject *target)
{
  if (event.target == nullptr && !event.target_handle.IsValid()) {
    return true;
  }
  if (logic_nodes_handle_matches(event.target_handle, target_handle)) {
    return true;
  }
  return target != nullptr && event.target == target;
}

}  // namespace

LN_EventPayloadLane LN_EventPayloadLaneForValue(const LN_Value &value)
{
  if (!value.exists) {
    return LN_EventPayloadLane::None;
  }

  switch (value.type) {
    case LN_ValueType::None:
      return LN_EventPayloadLane::None;
    case LN_ValueType::Bool:
      return LN_EventPayloadLane::Bool;
    case LN_ValueType::Int:
      return LN_EventPayloadLane::Int;
    case LN_ValueType::Float:
      return LN_EventPayloadLane::Float;
    case LN_ValueType::Vector:
      return LN_EventPayloadLane::Vector;
    case LN_ValueType::Vector4:
      return LN_EventPayloadLane::Vector4;
    case LN_ValueType::Matrix:
      return LN_EventPayloadLane::Matrix;
    case LN_ValueType::Color:
      return LN_EventPayloadLane::Color;
    case LN_ValueType::Rotation:
      return LN_EventPayloadLane::Rotation;
    case LN_ValueType::String:
      return LN_EventPayloadLane::String;
    case LN_ValueType::ObjectRef:
      return LN_EventPayloadLane::Object;
    case LN_ValueType::SceneRef:
      return LN_EventPayloadLane::Scene;
    case LN_ValueType::CollectionRef:
      return LN_EventPayloadLane::Collection;
    case LN_ValueType::DatablockRef:
      return LN_EventPayloadLane::Datablock;
    case LN_ValueType::List:
      return LN_EventPayloadLane::List;
    case LN_ValueType::Dict:
      return LN_EventPayloadLane::Dict;
    case LN_ValueType::Generic:
      return LN_EventPayloadLane::GenericValue;
  }

  return LN_EventPayloadLane::GenericValue;
}

void LN_EventBus::Publish(LN_EventEntry event)
{
  if (event.payload_lane == LN_EventPayloadLane::None) {
    event.payload_lane = LN_EventPayloadLaneForValue(event.content);
  }
  m_pendingEvents.push_back(std::move(event));
}

void LN_EventBus::BeginTick()
{
  m_tickEvents = std::move(m_pendingEvents);
  m_pendingEvents.clear();
  RebuildIndexes();
  m_indexedLookupCount = 0;
  m_fallbackScanCount = 0;
}

void LN_EventBus::ClearAll()
{
  m_pendingEvents.clear();
  m_tickEvents.clear();
  m_subjectIndex.clear();
  m_targetIndex.clear();
  m_broadcastEventIndices.clear();
  m_indexedLookupCount = 0;
  m_fallbackScanCount = 0;
}

const LN_EventEntry *LN_EventBus::FindFirst(const LN_EventSubjectId subject_id,
                                            const std::string &subject,
                                            const LN_ObjectHandle &target_handle,
                                            const KX_GameObject *target) const
{
  if (!subject_id.IsValid() && subject.empty()) {
    return nullptr;
  }

  if (subject_id.IsValid()) {
    const auto indexed_events = m_subjectIndex.find(subject_id.index);
    if (indexed_events != m_subjectIndex.end()) {
      m_indexedLookupCount++;
      for (const uint32_t event_index : indexed_events->second) {
        const LN_EventEntry &event = m_tickEvents[event_index];
        if (EntryMatchesFilter(event, subject_id, subject, target_handle, target)) {
          return &event;
        }
      }
    }
  }

  if (!subject_id.IsValid() && target_handle.IsValid()) {
    const auto targeted_events = m_targetIndex.find(logic_nodes_target_index_key(target_handle));
    if (targeted_events != m_targetIndex.end() || !m_broadcastEventIndices.empty()) {
      m_indexedLookupCount++;
      size_t target_index = 0;
      size_t broadcast_index = 0;
      const std::vector<uint32_t> empty_targeted_events;
      const std::vector<uint32_t> &targeted_indices = targeted_events != m_targetIndex.end() ?
                                                          targeted_events->second :
                                                          empty_targeted_events;
      while (target_index < targeted_indices.size() ||
             broadcast_index < m_broadcastEventIndices.size())
      {
        const bool use_targeted =
            broadcast_index >= m_broadcastEventIndices.size() ||
            (target_index < targeted_indices.size() &&
             targeted_indices[target_index] <= m_broadcastEventIndices[broadcast_index]);
        const uint32_t event_index = use_targeted ? targeted_indices[target_index++] :
                                                   m_broadcastEventIndices[broadcast_index++];
        const LN_EventEntry &event = m_tickEvents[event_index];
        if (EntryMatchesFilter(event, subject_id, subject, target_handle, target)) {
          return &event;
        }
      }
    }
  }

  m_fallbackScanCount++;
  for (const LN_EventEntry &event : m_tickEvents) {
    if (EntryMatchesFilter(event, subject_id, subject, target_handle, target)) {
      return &event;
    }
  }
  return nullptr;
}

void LN_EventBus::RebuildIndexes()
{
  m_subjectIndex.clear();
  m_targetIndex.clear();
  m_broadcastEventIndices.clear();
  for (uint32_t index = 0; index < m_tickEvents.size(); index++) {
    const LN_EventEntry &event = m_tickEvents[index];
    if (event.subject_id.IsValid()) {
      m_subjectIndex[event.subject_id.index].push_back(index);
    }
    if (event.target_handle.IsValid()) {
      m_targetIndex[logic_nodes_target_index_key(event.target_handle)].push_back(index);
    }
    else if (event.target == nullptr) {
      m_broadcastEventIndices.push_back(index);
    }
  }
}

bool LN_EventBus::EntryMatchesFilter(const LN_EventEntry &event,
                                     const LN_EventSubjectId subject_id,
                                     const std::string &subject,
                                     const LN_ObjectHandle &target_handle,
                                     const KX_GameObject *target) const
{
  return logic_nodes_subject_matches(event, subject_id, subject) &&
         logic_nodes_target_matches(event, target_handle, target);
}

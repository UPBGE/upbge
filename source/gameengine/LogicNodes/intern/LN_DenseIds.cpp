/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_DenseIds.cpp
 *  \ingroup logicnodes
 */

#include "LN_DenseIds.h"

#include <algorithm>

namespace {

const std::string &empty_debug_name()
{
  static const std::string empty;
  return empty;
}

uint32_t handle_hash(const void *pointer, const LN_DenseHandleKind kind)
{
  /* Fibonacci hashing: the multiply spreads the aligned pointer bits over the high bits. */
  const uint64_t key = uint64_t(uintptr_t(pointer)) ^ uint64_t(kind);
  return uint32_t((key * 0x9E3779B97F4A7C15ull) >> 32);
}

/** Up to this many handles, scanning them is faster than hashing (measured). */
constexpr size_t MAX_SCANNED_HANDLES = 6;

}  // namespace

inline uint32_t LN_DenseIdRegistry::FindHandle(const LN_DenseHandleKind kind,
                                               const void *pointer,
                                               uint32_t *r_slot) const
{
  if (m_handleIndex.empty()) {
    for (uint32_t index = 0; index < m_handles.size(); index++) {
      if (m_handles[index].pointer == pointer && m_handles[index].kind == kind) {
        return index;
      }
    }
    return LN_INVALID_INDEX;
  }
  const uint32_t mask = uint32_t(m_handleIndex.size()) - 1;
  for (uint32_t slot = handle_hash(pointer, kind) & mask;; slot = (slot + 1) & mask) {
    const HandleIndexSlot &entry = m_handleIndex[slot];
    if (entry.pointer == nullptr) {
      return LN_INVALID_INDEX;
    }
    if (entry.pointer == pointer && entry.kind == kind) {
      if (r_slot) {
        *r_slot = slot;
      }
      return entry.handle_index;
    }
  }
}

void LN_DenseIdRegistry::IndexHandle(const uint32_t handle_index)
{
  const auto insert = [this](const uint32_t index) {
    const HandleEntry &entry = m_handles[index];
    const uint32_t mask = uint32_t(m_handleIndex.size()) - 1;
    uint32_t slot = handle_hash(entry.pointer, entry.kind) & mask;
    while (m_handleIndex[slot].pointer != nullptr) {
      slot = (slot + 1) & mask;
    }
    m_handleIndex[slot] = {entry.pointer, index, entry.kind};
    m_handleIndexCount++;
  };
  if ((m_handleIndexCount + 1) * 4 <= m_handleIndex.size()) {
    insert(handle_index);
    return;
  }
  /* Create or grow the index, then add every live entry (this includes `handle_index`) and list
   * the free ones. */
  m_handleIndex.assign(std::max<size_t>(32, m_handleIndex.size() * 2), HandleIndexSlot());
  m_handleIndexCount = 0;
  m_freeHandles.clear();
  for (uint32_t index = 0; index < m_handles.size(); index++) {
    if (m_handles[index].pointer != nullptr) {
      insert(index);
    }
    else {
      m_freeHandles.push_back(index);
    }
  }
}

void LN_DenseIdRegistry::UnindexHandleSlot(uint32_t slot)
{
  /* Backward-shift deletion: pull later entries of the probe run into the hole unless their home
   * slot lies after it, so lookups never need tombstones. */
  const uint32_t mask = uint32_t(m_handleIndex.size()) - 1;
  for (uint32_t next = (slot + 1) & mask; m_handleIndex[next].pointer != nullptr;
       next = (next + 1) & mask)
  {
    const HandleIndexSlot &entry = m_handleIndex[next];
    const uint32_t home = handle_hash(entry.pointer, entry.kind) & mask;
    if (((next - home) & mask) >= ((next - slot) & mask)) {
      m_handleIndex[slot] = entry;
      slot = next;
    }
  }
  m_handleIndex[slot] = HandleIndexSlot();
  m_handleIndexCount--;
}

template<typename IdT>
IdT LN_DenseIdRegistry::InternInTable(std::unordered_map<std::string, uint32_t> &map,
                                      std::vector<InternEntry> &entries,
                                      const std::string &value)
{
  if (value.empty()) {
    return IdT{};
  }
  const auto existing = map.find(value);
  if (existing != map.end()) {
    IdT id;
    id.index = existing->second;
    return id;
  }

  const uint32_t index = uint32_t(entries.size());
  entries.push_back({value});
  map.emplace(value, index);
  IdT id;
  id.index = index;
  return id;
}

uint32_t LN_DenseIdRegistry::AddHandle(const LN_DenseHandleKind kind,
                                       void *pointer,
                                       const std::string &debug_name)
{
  uint32_t index = 0;
  if (m_handleIndex.empty()) {
    /* Few handles: reuse the first free entry, if any. */
    while (index < m_handles.size() && m_handles[index].pointer != nullptr) {
      index++;
    }
  }
  else if (!m_freeHandles.empty()) {
    index = m_freeHandles.back();
    m_freeHandles.pop_back();
  }
  else {
    index = uint32_t(m_handles.size());
  }
  if (index < m_handles.size()) {
    HandleEntry &entry = m_handles[index];
    entry.pointer = pointer;
    entry.kind = kind;
    entry.generation++;
    entry.debug_name = debug_name;
  }
  else {
    m_handles.push_back({pointer, kind, 1, debug_name});
  }
  if (!m_handleIndex.empty() || m_handles.size() > MAX_SCANNED_HANDLES) {
    IndexHandle(index);
  }
  return index;
}

template<typename HandleT>
HandleT LN_DenseIdRegistry::MakeHandle(const LN_DenseHandleKind kind,
                                       void *pointer,
                                       const std::string &debug_name)
{
  HandleT handle;
  handle.kind = kind;
  if (pointer == nullptr) {
    return handle;
  }
  handle.index = FindHandle(kind, pointer);
  if (handle.index == LN_INVALID_INDEX) {
    handle.index = AddHandle(kind, pointer, debug_name);
  }
  handle.generation = m_handles[handle.index].generation;
  return handle;
}

template<typename IdT>
const std::string &LN_DenseIdRegistry::DebugNameInTable(const std::vector<InternEntry> &entries,
                                                       const IdT id) const
{
  if (!id.IsValid() || id.index >= entries.size()) {
    return empty_debug_name();
  }
  return entries[id.index].debug_name;
}

LN_SharedStringId LN_DenseIdRegistry::InternString(const std::string &value)
{
  return InternInTable<LN_SharedStringId>(m_stringToId, m_strings, value);
}

LN_EventSubjectId LN_DenseIdRegistry::InternEventSubject(const std::string &subject)
{
  return InternInTable<LN_EventSubjectId>(m_eventSubjectToId, m_eventSubjects, subject);
}

LN_GamePropertyId LN_DenseIdRegistry::InternGameProperty(const std::string &name)
{
  return InternInTable<LN_GamePropertyId>(m_gamePropertyToId, m_gameProperties, name);
}

LN_TreePropertyId LN_DenseIdRegistry::InternTreeProperty(const std::string &name)
{
  return InternInTable<LN_TreePropertyId>(m_treePropertyToId, m_treeProperties, name);
}

LN_MaterialSlotId LN_DenseIdRegistry::InternMaterialSlot(const std::string &name)
{
  return InternInTable<LN_MaterialSlotId>(m_materialSlotToId, m_materialSlots, name);
}

LN_NodeSocketId LN_DenseIdRegistry::InternNodeSocket(const std::string &name)
{
  return InternInTable<LN_NodeSocketId>(m_nodeSocketToId, m_nodeSockets, name);
}

LN_ActionId LN_DenseIdRegistry::InternAction(const std::string &name)
{
  return InternInTable<LN_ActionId>(m_actionToId, m_actions, name);
}

LN_SoundId LN_DenseIdRegistry::InternSound(const std::string &name)
{
  return InternInTable<LN_SoundId>(m_soundToId, m_sounds, name);
}

LN_InputCodeId LN_DenseIdRegistry::InternInputCode(const std::string &name)
{
  return InternInTable<LN_InputCodeId>(m_inputCodeToId, m_inputCodes, name);
}

LN_ObjectHandle LN_DenseIdRegistry::MakeObjectHandle(void *pointer, const std::string &debug_name)
{
  return MakeHandle<LN_ObjectHandle>(LN_DenseHandleKind::Object, pointer, debug_name);
}

LN_ObjectHandle LN_DenseIdRegistry::FindObjectHandle(const void *pointer) const
{
  LN_ObjectHandle handle;
  if (pointer == nullptr) {
    return handle;
  }
  handle.index = FindHandle(LN_DenseHandleKind::Object, pointer);
  if (handle.index != LN_INVALID_INDEX) {
    handle.generation = m_handles[handle.index].generation;
  }
  return handle;
}

LN_SceneHandle LN_DenseIdRegistry::MakeSceneHandle(void *pointer, const std::string &debug_name)
{
  return MakeHandle<LN_SceneHandle>(LN_DenseHandleKind::Scene, pointer, debug_name);
}

LN_DatablockHandle LN_DenseIdRegistry::MakeDatablockHandle(void *pointer,
                                                           const std::string &debug_name)
{
  return MakeHandle<LN_DatablockHandle>(LN_DenseHandleKind::Datablock, pointer, debug_name);
}

LN_RuntimeTreeId LN_DenseIdRegistry::MakeRuntimeTreeId(void *pointer,
                                                       const std::string &debug_name)
{
  LN_RuntimeTreeId id;
  if (pointer == nullptr) {
    return id;
  }

  for (uint32_t index = 0; index < m_runtimeTrees.size(); index++) {
    HandleEntry &entry = m_runtimeTrees[index];
    if (entry.pointer == pointer) {
      id.index = index;
      id.generation = entry.generation;
      return id;
    }
  }

  for (uint32_t index = 0; index < m_runtimeTrees.size(); index++) {
    HandleEntry &entry = m_runtimeTrees[index];
    if (entry.pointer == nullptr) {
      entry.pointer = pointer;
      entry.kind = LN_DenseHandleKind::RuntimeTree;
      entry.generation++;
      entry.debug_name = debug_name;
      id.index = index;
      id.generation = entry.generation;
      return id;
    }
  }

  const uint32_t index = uint32_t(m_runtimeTrees.size());
  m_runtimeTrees.push_back({pointer, LN_DenseHandleKind::RuntimeTree, 1, debug_name});
  id.index = index;
  id.generation = 1;
  return id;
}

void *LN_DenseIdRegistry::ResolveHandle(const LN_DenseHandle &handle) const
{
  if (!handle.IsValid() || handle.index >= m_handles.size()) {
    return nullptr;
  }
  const HandleEntry &entry = m_handles[handle.index];
  if (entry.kind != handle.kind || entry.generation != handle.generation) {
    return nullptr;
  }
  return entry.pointer;
}

void *LN_DenseIdRegistry::ResolveRuntimeTreeId(const LN_RuntimeTreeId &id) const
{
  if (!id.IsValid() || id.index >= m_runtimeTrees.size()) {
    return nullptr;
  }
  const HandleEntry &entry = m_runtimeTrees[id.index];
  if (entry.generation != id.generation) {
    return nullptr;
  }
  return entry.pointer;
}

void LN_DenseIdRegistry::InvalidateHandle(const LN_DenseHandleKind kind, const void *pointer)
{
  if (kind == LN_DenseHandleKind::None || pointer == nullptr) {
    return;
  }
  uint32_t slot = LN_INVALID_INDEX;
  const uint32_t index = FindHandle(kind, pointer, &slot);
  if (index == LN_INVALID_INDEX) {
    return;
  }
  HandleEntry &entry = m_handles[index];
  entry.pointer = nullptr;
  entry.generation++;
  if (slot != LN_INVALID_INDEX) {
    m_freeHandles.push_back(index);
    UnindexHandleSlot(slot);
  }
}

void LN_DenseIdRegistry::InvalidateRuntimeTree(void *pointer)
{
  if (pointer == nullptr) {
    return;
  }
  for (HandleEntry &entry : m_runtimeTrees) {
    if (entry.pointer == pointer) {
      entry.pointer = nullptr;
      entry.generation++;
    }
  }
}

void LN_DenseIdRegistry::ClearSceneLocalState()
{
  /* Every entry becomes free; list them in reverse so the lowest indices are reused first. */
  m_freeHandles.clear();
  for (uint32_t index = uint32_t(m_handles.size()); index-- > 0;) {
    HandleEntry &entry = m_handles[index];
    if (entry.pointer != nullptr) {
      entry.pointer = nullptr;
      entry.generation++;
    }
    if (!m_handleIndex.empty()) {
      m_freeHandles.push_back(index);
    }
  }
  std::fill(m_handleIndex.begin(), m_handleIndex.end(), HandleIndexSlot());
  m_handleIndexCount = 0;
  for (HandleEntry &entry : m_runtimeTrees) {
    if (entry.pointer != nullptr) {
      entry.pointer = nullptr;
      entry.generation++;
    }
  }
}

void LN_DenseIdRegistry::Clear()
{
  m_stringToId.clear();
  m_eventSubjectToId.clear();
  m_gamePropertyToId.clear();
  m_treePropertyToId.clear();
  m_materialSlotToId.clear();
  m_nodeSocketToId.clear();
  m_actionToId.clear();
  m_soundToId.clear();
  m_inputCodeToId.clear();
  m_strings.clear();
  m_eventSubjects.clear();
  m_gameProperties.clear();
  m_treeProperties.clear();
  m_materialSlots.clear();
  m_nodeSockets.clear();
  m_actions.clear();
  m_sounds.clear();
  m_inputCodes.clear();
  m_handles.clear();
  m_handleIndex.clear();
  m_handleIndexCount = 0;
  m_freeHandles.clear();
  m_runtimeTrees.clear();
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_SharedStringId id) const
{
  return DebugNameInTable(m_strings, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_EventSubjectId id) const
{
  return DebugNameInTable(m_eventSubjects, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_GamePropertyId id) const
{
  return DebugNameInTable(m_gameProperties, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_TreePropertyId id) const
{
  return DebugNameInTable(m_treeProperties, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_MaterialSlotId id) const
{
  return DebugNameInTable(m_materialSlots, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_NodeSocketId id) const
{
  return DebugNameInTable(m_nodeSockets, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_ActionId id) const
{
  return DebugNameInTable(m_actions, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_SoundId id) const
{
  return DebugNameInTable(m_sounds, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_InputCodeId id) const
{
  return DebugNameInTable(m_inputCodes, id);
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_DenseHandle &handle) const
{
  if (!handle.IsValid() || handle.index >= m_handles.size()) {
    return empty_debug_name();
  }
  const HandleEntry &entry = m_handles[handle.index];
  if (entry.kind != handle.kind || entry.generation != handle.generation) {
    return empty_debug_name();
  }
  return entry.debug_name;
}

const std::string &LN_DenseIdRegistry::DebugName(const LN_RuntimeTreeId id) const
{
  if (!id.IsValid() || id.index >= m_runtimeTrees.size()) {
    return empty_debug_name();
  }
  const HandleEntry &entry = m_runtimeTrees[id.index];
  if (entry.generation != id.generation) {
    return empty_debug_name();
  }
  return entry.debug_name;
}

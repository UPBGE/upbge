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

}  // namespace

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

template<typename HandleT>
HandleT LN_DenseIdRegistry::MakeHandle(const LN_DenseHandleKind kind,
                                       void *pointer,
                                       const std::string &debug_name)
{
  HandleT handle;
  if (pointer == nullptr) {
    handle.kind = kind;
    return handle;
  }

  for (uint32_t index = 0; index < m_handles.size(); index++) {
    HandleEntry &entry = m_handles[index];
    if (entry.kind == kind && entry.pointer == pointer) {
      handle.kind = kind;
      handle.index = index;
      handle.generation = entry.generation;
      return handle;
    }
  }

  for (uint32_t index = 0; index < m_handles.size(); index++) {
    HandleEntry &entry = m_handles[index];
    if (entry.pointer == nullptr) {
      entry.pointer = pointer;
      entry.kind = kind;
      entry.generation++;
      entry.debug_name = debug_name;
      handle.kind = kind;
      handle.index = index;
      handle.generation = entry.generation;
      return handle;
    }
  }

  const uint32_t index = uint32_t(m_handles.size());
  m_handles.push_back({pointer, kind, 1, debug_name});
  handle.kind = kind;
  handle.index = index;
  handle.generation = 1;
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
  for (HandleEntry &entry : m_handles) {
    if (entry.kind == kind && entry.pointer == pointer) {
      entry.pointer = nullptr;
      entry.generation++;
    }
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
  for (HandleEntry &entry : m_handles) {
    if (entry.pointer != nullptr) {
      entry.pointer = nullptr;
      entry.generation++;
    }
  }
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

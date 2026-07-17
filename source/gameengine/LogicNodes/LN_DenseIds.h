/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_DenseIds.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "LN_Types.h"

struct LN_SharedStringId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_EventSubjectId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_EventTargetId {
  uint32_t index = LN_INVALID_INDEX;
  uint32_t generation = 0;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX && generation != 0;
  }
};

struct LN_GamePropertyId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_TreePropertyId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_MaterialSlotId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_NodeSocketId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_ActionId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_SoundId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_InputCodeId {
  uint32_t index = LN_INVALID_INDEX;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX;
  }
};

struct LN_RuntimeTreeId {
  uint32_t index = LN_INVALID_INDEX;
  uint32_t generation = 0;

  bool IsValid() const
  {
    return index != LN_INVALID_INDEX && generation != 0;
  }
};

enum class LN_DenseHandleKind : uint8_t {
  None = 0,
  Object,
  Scene,
  Material,
  Socket,
  Action,
  Sound,
  Datablock,
  RuntimeTree,
};

struct LN_DenseHandle {
  LN_DenseHandleKind kind = LN_DenseHandleKind::None;
  uint32_t index = LN_INVALID_INDEX;
  uint32_t generation = 0;

  bool IsValid() const
  {
    return kind != LN_DenseHandleKind::None && index != LN_INVALID_INDEX && generation != 0;
  }
};

struct LN_ObjectHandle : LN_DenseHandle {
  LN_ObjectHandle()
  {
    kind = LN_DenseHandleKind::Object;
  }
};

struct LN_SceneHandle : LN_DenseHandle {
  LN_SceneHandle()
  {
    kind = LN_DenseHandleKind::Scene;
  }
};

struct LN_DatablockHandle : LN_DenseHandle {
  LN_DatablockHandle()
  {
    kind = LN_DenseHandleKind::Datablock;
  }
};

class LN_DenseIdRegistry {
 public:
  LN_SharedStringId InternString(const std::string &value);
  LN_EventSubjectId InternEventSubject(const std::string &subject);
  LN_GamePropertyId InternGameProperty(const std::string &name);
  LN_TreePropertyId InternTreeProperty(const std::string &name);
  LN_MaterialSlotId InternMaterialSlot(const std::string &name);
  LN_NodeSocketId InternNodeSocket(const std::string &name);
  LN_ActionId InternAction(const std::string &name);
  LN_SoundId InternSound(const std::string &name);
  LN_InputCodeId InternInputCode(const std::string &name);

  LN_ObjectHandle MakeObjectHandle(void *pointer, const std::string &debug_name = "");
  LN_SceneHandle MakeSceneHandle(void *pointer, const std::string &debug_name = "");
  LN_DatablockHandle MakeDatablockHandle(void *pointer, const std::string &debug_name = "");
  LN_RuntimeTreeId MakeRuntimeTreeId(void *pointer, const std::string &debug_name = "");

  void *ResolveHandle(const LN_DenseHandle &handle) const;
  void *ResolveRuntimeTreeId(const LN_RuntimeTreeId &id) const;
  void InvalidateHandle(LN_DenseHandleKind kind, const void *pointer);
  void InvalidateRuntimeTree(void *pointer);
  void ClearSceneLocalState();
  void Clear();

  const std::string &DebugName(LN_SharedStringId id) const;
  const std::string &DebugName(LN_EventSubjectId id) const;
  const std::string &DebugName(LN_GamePropertyId id) const;
  const std::string &DebugName(LN_TreePropertyId id) const;
  const std::string &DebugName(LN_MaterialSlotId id) const;
  const std::string &DebugName(LN_NodeSocketId id) const;
  const std::string &DebugName(LN_ActionId id) const;
  const std::string &DebugName(LN_SoundId id) const;
  const std::string &DebugName(LN_InputCodeId id) const;
  const std::string &DebugName(const LN_DenseHandle &handle) const;
  const std::string &DebugName(LN_RuntimeTreeId id) const;

 private:
  struct InternEntry {
    std::string debug_name;
  };

  struct HandleEntry {
    void *pointer = nullptr;
    LN_DenseHandleKind kind = LN_DenseHandleKind::None;
    uint32_t generation = 1;
    std::string debug_name;
  };

  template<typename IdT>
  IdT InternInTable(std::unordered_map<std::string, uint32_t> &map,
                    std::vector<InternEntry> &entries,
                    const std::string &value);
  template<typename HandleT>
  HandleT MakeHandle(LN_DenseHandleKind kind, void *pointer, const std::string &debug_name);
  template<typename IdT>
  const std::string &DebugNameInTable(const std::vector<InternEntry> &entries, IdT id) const;

  std::unordered_map<std::string, uint32_t> m_stringToId;
  std::unordered_map<std::string, uint32_t> m_eventSubjectToId;
  std::unordered_map<std::string, uint32_t> m_gamePropertyToId;
  std::unordered_map<std::string, uint32_t> m_treePropertyToId;
  std::unordered_map<std::string, uint32_t> m_materialSlotToId;
  std::unordered_map<std::string, uint32_t> m_nodeSocketToId;
  std::unordered_map<std::string, uint32_t> m_actionToId;
  std::unordered_map<std::string, uint32_t> m_soundToId;
  std::unordered_map<std::string, uint32_t> m_inputCodeToId;

  std::vector<InternEntry> m_strings;
  std::vector<InternEntry> m_eventSubjects;
  std::vector<InternEntry> m_gameProperties;
  std::vector<InternEntry> m_treeProperties;
  std::vector<InternEntry> m_materialSlots;
  std::vector<InternEntry> m_nodeSockets;
  std::vector<InternEntry> m_actions;
  std::vector<InternEntry> m_sounds;
  std::vector<InternEntry> m_inputCodes;
  std::vector<HandleEntry> m_handles;
  std::vector<HandleEntry> m_runtimeTrees;
};

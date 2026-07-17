/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_DebugMap.h
 *  \ingroup logicnodes
 */

#pragma once

#include <vector>

#include "LN_Types.h"

struct LN_DebugMapEntry {
  LN_Event event = LN_Event::OnInit;
  uint32_t instruction_index = 0;
  uint32_t source_ref_index = 0;
};

class LN_DebugMap {
 public:
  void Clear()
  {
    m_entries.clear();
  }

  void Add(LN_Event event, uint32_t instruction_index, uint32_t source_ref_index)
  {
    m_entries.push_back({event, instruction_index, source_ref_index});
  }

  const std::vector<LN_DebugMapEntry> &GetEntries() const
  {
    return m_entries;
  }

 private:
  std::vector<LN_DebugMapEntry> m_entries;
};

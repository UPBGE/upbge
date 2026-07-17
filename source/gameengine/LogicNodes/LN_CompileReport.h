/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_CompileReport.h
 *  \ingroup logicnodes
 */

#pragma once

#include <string>
#include <vector>

#include "LN_Types.h"

struct LN_CompileIssue {
  LN_CompileSeverity severity = LN_CompileSeverity::Info;
  std::string message;
  uint32_t source_ref_index = 0;
};

class LN_CompileReport {
 public:
  void Clear()
  {
    m_sourceTreeName.clear();
    m_disabledReason.clear();
    m_issues.clear();
  }

  void SetSourceTreeName(const std::string &source_tree_name)
  {
    m_sourceTreeName = source_tree_name;
  }

  const std::string &GetSourceTreeName() const
  {
    return m_sourceTreeName;
  }

  void SetDisabledReason(const std::string &disabled_reason)
  {
    m_disabledReason = disabled_reason;
  }

  const std::string &GetDisabledReason() const
  {
    return m_disabledReason;
  }

  void AddIssue(LN_CompileSeverity severity,
                const std::string &message,
                uint32_t source_ref_index = 0)
  {
    m_issues.push_back({severity, message, source_ref_index});
  }

  const std::vector<LN_CompileIssue> &GetIssues() const
  {
    return m_issues;
  }

  bool HasErrors() const
  {
    for (const LN_CompileIssue &issue : m_issues) {
      if (issue.severity == LN_CompileSeverity::Error) {
        return true;
      }
    }
    return false;
  }

  bool IsEmpty() const
  {
    return m_disabledReason.empty() && m_issues.empty();
  }

 private:
  std::string m_sourceTreeName;
  std::string m_disabledReason;
  std::vector<LN_CompileIssue> m_issues;
};

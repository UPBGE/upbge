/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_NodeMetadata.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "LN_NodeRegistry.h"
#include "LN_TreeCompiler.h"

enum class LN_NodeVisibility : uint8_t {
  Hidden = 0,
  Release,
  Experimental,
};

enum class LN_NodeSchedulerPolicy : uint8_t {
  SnapshotReadOnly = 0,
  CommandEmitting,
  MainThreadOnly,
};

struct LN_NodeMetadataContract {
  std::string idname;
  LN_NodeKind kind = LN_NodeKind::ValueBool;
  std::string category;
  std::vector<LN_PinDefinition> inputs;
  std::vector<LN_PinDefinition> outputs;
  bool has_side_effects = false;
  LN_RequiredPhase required_phase = LN_RequiredPhase::None;
  bool requires_jolt = false;
  bool future_pure_batchable = false;
  LN_ExecutionClass execution_class = LN_ExecutionClass::SnapshotReadOnly;
  LN_NodeSchedulerPolicy scheduler_policy = LN_NodeSchedulerPolicy::SnapshotReadOnly;
  LN_NodeVisibility visibility = LN_NodeVisibility::Hidden;
  LN_TreeCompiler::CompileHandlerInfo compile_handler_info;
  std::string compile_handler_id;
};

const std::vector<LN_NodeMetadataContract> &LN_GetNodeMetadataContracts();
const LN_NodeMetadataContract *LN_FindNodeMetadataContract(const std::string &idname);
bool LN_ValidateNodeMetadataContracts(std::vector<std::string> *r_errors = nullptr);
bool LN_ValidateNodeMetadataContracts(const std::vector<LN_NodeMetadataContract> &contracts,
                                      std::vector<std::string> *r_errors = nullptr);

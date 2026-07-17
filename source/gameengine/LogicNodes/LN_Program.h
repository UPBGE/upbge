/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Program.h
 *  \ingroup logicnodes
 */

#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "LN_CompileReport.h"
#include "LN_DebugMap.h"
#include "LN_ExecBlockIR.h"
#include "LN_RegisterExpressionIR.h"
#include "LN_Types.h"

class LN_Program {
 public:
  LN_Program();

  static std::shared_ptr<LN_Program> CreateDebugSetWorldPosition(const MT_Vector3 &position);

  const std::string &GetName() const
  {
    return m_name;
  }

  const std::string &GetSourceTreeName() const
  {
    return m_sourceTreeName;
  }

  const std::string &GetSourceTreeLibraryPath() const
  {
    return m_sourceTreeLibraryPath;
  }

  const std::string &GetSourceChecksum() const
  {
    return m_sourceChecksum;
  }

  uint32_t GetProgramVersion() const
  {
    return m_programVersion;
  }

  uint32_t GetSchemaVersion() const
  {
    return m_schemaVersion;
  }

  uint32_t GetRuntimeBuildFlags() const
  {
    return m_runtimeBuildFlags;
  }

  bool IsCurrentRuntimeCompatible() const;
  bool MatchesSource(const std::string &source_tree_name,
                     const std::string &source_tree_library_path,
                     const std::string &source_checksum) const;
  bool CanPreserveRuntimeStateWhenReplacing(const LN_Program &previous) const;

  const std::vector<LN_SourceRef> &GetSourceRefs() const
  {
    return m_sourceRefs;
  }

  const std::vector<int32_t> &GetSourceNodeOrder() const
  {
    return m_sourceNodeOrder;
  }

  const std::vector<LN_Constant> &GetConstants() const
  {
    return m_constants;
  }

  const std::vector<LN_GamePropertyRef> &GetGamePropertyRefs() const
  {
    return m_gamePropertyRefs;
  }

  const std::vector<LN_TreePropertyRef> &GetTreePropertyRefs() const
  {
    return m_treePropertyRefs;
  }

  const std::vector<std::string> &GetStringTable() const
  {
    return m_stringTable;
  }

  const std::string &GetString(LN_StringId string_id) const;

  const std::vector<LN_GroupCallFrame> &GetGroupCallFrames() const
  {
    return m_groupCallFrames;
  }

  const std::vector<LN_LoopFrame> &GetLoopFrames() const
  {
    return m_loopFrames;
  }

  const std::vector<LN_BoolExpression> &GetBoolExpressions() const
  {
    return m_boolExpressions;
  }

  const std::vector<LN_FloatExpression> &GetFloatExpressions() const
  {
    return m_floatExpressions;
  }

  const std::vector<LN_IntExpression> &GetIntExpressions() const
  {
    return m_intExpressions;
  }

  const std::vector<LN_StringExpression> &GetStringExpressions() const
  {
    return m_stringExpressions;
  }

  const std::vector<LN_VectorExpression> &GetVectorExpressions() const
  {
    return m_vectorExpressions;
  }

  const std::vector<LN_QueryExpression> &GetQueryExpressions() const
  {
    return m_queryExpressions;
  }

  uint32_t GetQueryRuntimeStateCount() const
  {
    return m_queryRuntimeStateCount;
  }

  uint32_t GetSpawnPoolStateCount() const
  {
    return m_spawnPoolStateCount;
  }

  uint32_t GetTimeFlowStateCount() const
  {
    return m_timeFlowStateCount;
  }

  uint32_t GetTimerStateCount() const
  {
    return GetTimeFlowStateCount();
  }

  /** False when the program must run on the main thread. */
  bool IsParallelEligible() const;
  LN_MainThreadOnlyReason GetMainThreadOnlyReason() const;
  const LN_SchedulerSummary &GetSchedulerSummary() const
  {
    return m_schedulerSummary;
  }
  const LN_ProgramDependencySummary &GetDependencySummary() const
  {
    return m_dependencySummary;
  }
  std::string DescribeSchedulerSummary() const;

  const std::vector<LN_ColorExpression> &GetColorExpressions() const
  {
    return m_colorExpressions;
  }

  const std::vector<LN_ValueExpression> &GetValueExpressions() const
  {
    return m_valueExpressions;
  }

  const std::vector<LN_VectorCommandPayload> &GetVectorCommandPayloads() const
  {
    return m_vectorCommandPayloads;
  }

  const std::vector<LN_GamePropertyCommandPayload> &GetGamePropertyCommandPayloads() const
  {
    return m_gamePropertyCommandPayloads;
  }
  const std::vector<LN_RigidBodyConstraintCommandPayload> &
  GetRigidBodyConstraintCommandPayloads() const
  {
    return m_rigidBodyConstraintCommandPayloads;
  }

  const std::vector<std::array<float, LN_TWEEN_CURVE_SAMPLE_COUNT>> &GetTweenCurveTables() const
  {
    return m_tweenCurveTables;
  }

  uint32_t AddGamePropertyRef(const LN_GamePropertyRef &property_ref);
  uint32_t AddTreePropertyRef(const LN_TreePropertyRef &property_ref);
  LN_StringId InternString(const std::string &value);
  uint32_t AddSpawnPoolState();
  uint32_t AddTimeFlowState();
  uint32_t AddTimerState();
  uint32_t AddGroupCallFrame(const LN_GroupCallFrame &call_frame);
  uint32_t AddLoopFrame(const LN_LoopFrame &loop_frame);
  void UpdateLoopFrame(uint32_t loop_frame_index, const LN_LoopFrame &loop_frame);
  uint32_t AddBoolExpression(const LN_BoolExpression &expression);
  uint32_t AddFloatExpression(const LN_FloatExpression &expression);
  uint32_t AddIntExpression(const LN_IntExpression &expression);
  uint32_t AddStringExpression(const LN_StringExpression &expression);
  uint32_t AddVectorExpression(const LN_VectorExpression &expression);
  uint32_t AddQueryExpression(const LN_QueryExpression &expression);
  void AddRayQueryDetailRequirement(int32_t cacheKey, uint8_t detailFlags);
  uint32_t AddColorExpression(const LN_ColorExpression &expression);
  uint32_t AddValueExpression(const LN_ValueExpression &expression);
  uint32_t AddTweenCurveTable(const std::array<float, LN_TWEEN_CURVE_SAMPLE_COUNT> &samples);
  uint32_t AddSourceRef(const LN_SourceRef &source_ref);
  void AddConstant(const LN_Constant &constant);
  void AddCompileIssue(LN_CompileSeverity severity,
                       const std::string &message,
                       uint32_t source_ref_index);
  uint32_t AddInstruction(LN_Event event, const LN_Instruction &instruction);
  uint32_t AddRigidBodyConstraintCommandPayload(
      const LN_RigidBodyConstraintCommandPayload &payload);

  const std::vector<LN_InstructionHeader> &GetInstructionHeaders(LN_Event event) const;
  const LN_Instruction &GetInstructionPayload(LN_Event event, uint32_t payload_index) const;
  const LN_Instruction &GetInstructionPayload(LN_Event event,
                                              const LN_InstructionHeader &instruction) const;
  bool ValidateInstructionPayloads(std::vector<std::string> *r_errors = nullptr) const;

  const LN_DebugMap &GetDebugMap() const
  {
    return m_debugMap;
  }

  const LN_CompileReport &GetCompileReport() const
  {
    return m_compileReport;
  }

  const std::vector<LN_Instruction> &GetInstructions(LN_Event event) const;
  const LN_ExecBlockProgram &GetExecBlockProgram(LN_Event event) const;
  bool ValidateExecBlockIR(std::vector<std::string> *r_errors = nullptr) const;
  const LN_RegisterExpressionProgram &GetRegisterExpressionProgram() const
  {
    return m_registerExpressionProgram;
  }
  bool ValidateRegisterExpressionIR(std::vector<std::string> *r_errors = nullptr) const;

 private:
  friend class LN_TreeCompiler;

  uint32_t AddVectorCommandPayload(const LN_VectorCommandPayload &payload);
  uint32_t AddGamePropertyCommandPayload(const LN_GamePropertyCommandPayload &payload);
  void UpdateSchedulerSummaryForInstruction(LN_Event event, const LN_Instruction &instruction);
  void ValidateInstructionPayloadCollection(LN_Event event,
                                            const std::vector<LN_InstructionHeader> &headers,
                                            const std::vector<LN_Instruction> &payloads,
                                            std::vector<std::string> &r_errors) const;
  void RebuildExecBlockIR(LN_Event event);
  bool ValidateExecBlockProgram(const LN_ExecBlockProgram &ir,
                                const std::vector<LN_InstructionHeader> &headers,
                                std::vector<std::string> &r_errors) const;
  void RebuildRegisterExpressionIR();
  bool ValidateRegisterExpressionProgram(const LN_RegisterExpressionProgram &ir,
                                         std::vector<std::string> &r_errors) const;
  void UpdateSchedulerSummaryForBoolExpression(const LN_BoolExpression &expression);
  void UpdateSchedulerSummaryForFloatExpression(const LN_FloatExpression &expression);
  void UpdateSchedulerSummaryForIntExpression(const LN_IntExpression &expression);
  void UpdateSchedulerSummaryForStringExpression(const LN_StringExpression &expression);
  void UpdateSchedulerSummaryForVectorExpression(const LN_VectorExpression &expression);
  void UpdateSchedulerSummaryForQueryExpression(const LN_QueryExpression &expression);
  void UpdateSchedulerSummaryForColorExpression(const LN_ColorExpression &expression);
  void UpdateSchedulerSummaryForValueExpression(const LN_ValueExpression &expression);
  void AddInstructionDependencies(LN_Event event, const LN_Instruction &instruction);
  void AddBoolExpressionDependencies(const LN_BoolExpression &expression);
  void AddFloatExpressionDependencies(const LN_FloatExpression &expression);
  void AddIntExpressionDependencies(const LN_IntExpression &expression);
  void AddStringExpressionDependencies(const LN_StringExpression &expression);
  void AddVectorExpressionDependencies(const LN_VectorExpression &expression);
  void AddQueryExpressionDependencies(const LN_QueryExpression &expression);
  void AddColorExpressionDependencies(const LN_ColorExpression &expression);
  void AddValueExpressionDependencies(const LN_ValueExpression &expression);

  uint32_t m_programVersion;
  uint32_t m_schemaVersion;
  uint32_t m_runtimeBuildFlags;
  std::string m_name;
  std::string m_sourceTreeName;
  std::string m_sourceTreeLibraryPath;
  std::string m_sourceChecksum;
  std::vector<LN_SourceRef> m_sourceRefs;
  std::vector<int32_t> m_sourceNodeOrder;
  std::vector<LN_Constant> m_constants;
  std::vector<std::string> m_stringTable;
  std::vector<LN_GamePropertyRef> m_gamePropertyRefs;
  std::vector<LN_TreePropertyRef> m_treePropertyRefs;
  std::vector<LN_GroupCallFrame> m_groupCallFrames;
  std::vector<LN_LoopFrame> m_loopFrames;
  std::vector<LN_BoolExpression> m_boolExpressions;
  std::vector<LN_FloatExpression> m_floatExpressions;
  std::vector<LN_IntExpression> m_intExpressions;
  std::vector<LN_StringExpression> m_stringExpressions;
  std::vector<LN_VectorExpression> m_vectorExpressions;
  std::vector<LN_QueryExpression> m_queryExpressions;
  uint32_t m_queryRuntimeStateCount = 0;
  uint32_t m_spawnPoolStateCount = 0;
  uint32_t m_timeFlowStateCount = 0;
  std::vector<LN_ColorExpression> m_colorExpressions;
  std::vector<LN_ValueExpression> m_valueExpressions;
  std::vector<std::array<float, LN_TWEEN_CURVE_SAMPLE_COUNT>> m_tweenCurveTables;
  std::vector<LN_VectorCommandPayload> m_vectorCommandPayloads;
  std::vector<LN_GamePropertyCommandPayload> m_gamePropertyCommandPayloads;
  std::vector<LN_RigidBodyConstraintCommandPayload> m_rigidBodyConstraintCommandPayloads;
  std::vector<LN_InstructionHeader> m_onInit;
  std::vector<LN_InstructionHeader> m_onFixedUpdate;
  std::vector<LN_Instruction> m_onInitPayloads;
  std::vector<LN_Instruction> m_onFixedUpdatePayloads;
  std::array<LN_ExecBlockProgram, 2> m_execBlockPrograms;
  LN_RegisterExpressionProgram m_registerExpressionProgram;
  LN_DebugMap m_debugMap;
  LN_CompileReport m_compileReport;
  LN_SchedulerSummary m_schedulerSummary;
  LN_ProgramDependencySummary m_dependencySummary;
};

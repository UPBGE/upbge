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
#include "LN_LeanProgram.h"
#include "LN_RegisterExpressionIR.h"
#include "LN_Types.h"

struct LN_TreeStateLayout;
struct LN_TreeBatchProfile;

/** Counts every change of what a tree runs (a tree given a program, a program that dropped its
 * lean program because it grew): the column runner keeps its "no run is possible" answer while
 * the count and the tick's tree list are unchanged (plan C2 P1). Any thread. */
void LN_NoteProgramsChanged();
uint64_t LN_ProgramsChangedSerial();

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

  /** Never the id of another program, a copy included: a tree's snapshot keeps the game
   * properties it found by name for the program with this id. */
  uint64_t GetUniqueId() const
  {
    return m_uniqueId.value;
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
  /** Plan C2 P4: no op but Set Tree Property writes tree property `ref` (no Add Object,
   * Inventory or Assign Geometry Nodes Modifier result): its flush-time re-write is redundant.
   * Worked out once (main thread), again after the program grows. */
  bool TreePropertyOnlySetBySetTreeProperty(uint32_t ref) const;

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

  uint32_t GetTimeFlowStateCount() const
  {
    return m_timeFlowStateCount;
  }

  uint32_t GetFlowControlStateCount() const
  {
    return m_flowControlStateCount;
  }

  uint32_t GetTimerStateCount() const
  {
    return GetTimeFlowStateCount();
  }

  /** The old thread analysis: false when it says the program must run on the main thread. Only
   * tests read it, and the editor's validation report prints its summary
   * (DescribeSchedulerSummary), since Parallel Tree Execution was removed (2026-10-30); tree
   * batches decide with their own lists (LN_AnalyzeTreeBatchSafety, LN_TreeBatch.cpp). */
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
  const std::vector<LN_PauseProcessingRequest> &GetPauseProcessingRequests() const
  {
    return m_pauseProcessingRequests;
  }
  bool HasPauseProcessingRequests() const
  {
    return !m_pauseProcessingRequests.empty();
  }
  /** On Logic + Physics Step's Every N Steps gates (LN_Instruction::step_gate_index). */
  const std::vector<LN_StepGate> &GetStepGates() const
  {
    return m_stepGates;
  }
  /** How many On Collision nodes have Events = Each Object (AcquireCollisionEventNode): a tree
   * keeps one LN_RuntimeTree::CollisionEventNodeState for each. */
  uint32_t GetCollisionEventNodeCount() const
  {
    return uint32_t(m_collisionEventNodes.size());
  }
  /** Where the state of a tree running this program lives (LN_RuntimeTree builds it the first
   * time a tree takes the program, then every tree shares it). Main thread only. */
  const std::shared_ptr<const LN_TreeStateLayout> &CachedTreeStateLayout() const
  {
    return m_treeStateLayout;
  }
  void CacheTreeStateLayout(std::shared_ptr<const LN_TreeStateLayout> layout) const
  {
    m_treeStateLayout = std::move(layout);
  }
  /** The lean program (plan phase A16): made by EnsureLeanProgram on the main thread (a helper
   * thread of the job pool only reads, and finds none when none was made), dropped when the
   * program grows. */
  const std::shared_ptr<const LN_LeanProgram> &GetLeanProgram() const
  {
    return m_leanProgram;
  }
  void EnsureLeanProgram() const;
  /** Whether trees running this program may be recorded in a tree batch (LN_TreeBatch.h),
   * worked out at the first ask and kept until the program grows. Main thread only. */
  const LN_TreeBatchProfile &GetTreeBatchProfile() const;
  /** The game property refs' starting values, shared by the snapshots (LN_GamePropertyLayout);
   * made at the first ask, made again after a ref was added. Main thread only. */
  const std::shared_ptr<const LN_GamePropertyLayout> &GetGamePropertyLayout() const;
  /** Frame Update instructions that UI events start; they also run while the game is paused. */
  bool HasUIEventRoutes() const
  {
    return m_hasUIEventRoutes;
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

  const std::vector<LN_CurveTable> &GetCurveTables() const
  {
    return m_curveTables;
  }

  uint32_t AddGamePropertyRef(const LN_GamePropertyRef &property_ref);
  uint32_t AddTreePropertyRef(const LN_TreePropertyRef &property_ref);
  LN_StringId InternString(const std::string &value);
  uint32_t AddTimeFlowState();
  uint32_t AddFlowControlState();
  uint32_t AddTimerState();
  uint32_t AddGroupCallFrame(const LN_GroupCallFrame &call_frame);
  uint32_t AddLoopFrame(const LN_LoopFrame &loop_frame);
  void AddPauseProcessingRequest(const LN_PauseProcessingRequest &request)
  {
    m_pauseProcessingRequests.push_back(request);
  }
  /** The gate of an Every N Steps setting: the existing one when another step node has the same
   * setting (they are due on the same steps). */
  uint32_t AddStepGate(const LN_StepGate &gate);
  /** The index of an On Collision node with Events = Each Object (the same for all its outputs:
   * LN_CollisionRepeatRoute, the outputs' event node fields). `node_key` (the bNode) is only
   * compared, while the program is compiled. */
  uint32_t AcquireCollisionEventNode(const void *node_key);
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
  uint32_t AddCurveTable(const LN_CurveTable &table);
  uint32_t AddSourceRef(const LN_SourceRef &source_ref);
  void AddConstant(const LN_Constant &constant);
  void AddCompileIssue(LN_CompileSeverity severity,
                       const std::string &message,
                       uint32_t source_ref_index);
  uint32_t AddInstruction(LN_Event event, const LN_Instruction &instruction);
  uint32_t AddRigidBodyConstraintCommandPayload(
      const LN_RigidBodyConstraintCommandPayload &payload);

  /* Inline since plan phase A18: the interpreter asks for these at every op (and every
   * InstructionStateIndex), which as calls into another unit cost ~5% of a brain's recording. */
  const std::vector<LN_InstructionHeader> &GetInstructionHeaders(const LN_Event event) const
  {
    switch (event) {
      case LN_Event::OnInit:
        return m_onInit;
      case LN_Event::OnUpdate:
        return m_onUpdate;
      case LN_Event::OnFixedUpdate:
        return m_onFixedUpdate;
    }
    return m_onFixedUpdate;
  }
  const LN_Instruction &GetInstructionPayload(const LN_Event event,
                                              const uint32_t payload_index) const
  {
    const std::vector<LN_Instruction> &payloads = GetInstructions(event);
    if (payload_index < payloads.size()) {
      return payloads[payload_index];
    }
    static const LN_Instruction empty_instruction;
    return empty_instruction;
  }
  const LN_Instruction &GetInstructionPayload(const LN_Event event,
                                              const LN_InstructionHeader &instruction) const
  {
    return GetInstructionPayload(event, instruction.payload_index);
  }
  bool ValidateInstructionPayloads(std::vector<std::string> *r_errors = nullptr) const;

  const LN_DebugMap &GetDebugMap() const
  {
    return m_debugMap;
  }

  const LN_CompileReport &GetCompileReport() const
  {
    return m_compileReport;
  }

  const std::vector<LN_Instruction> &GetInstructions(const LN_Event event) const
  {
    switch (event) {
      case LN_Event::OnInit:
        return m_onInitPayloads;
      case LN_Event::OnUpdate:
        return m_onUpdatePayloads;
      case LN_Event::OnFixedUpdate:
        return m_onFixedUpdatePayloads;
    }
    return m_onFixedUpdatePayloads;
  }
  const LN_ExecBlockProgram &GetExecBlockProgram(const LN_Event event) const
  {
    return m_execBlockPrograms[size_t(event)];
  }
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
  void ReorderInstructionsForCompiler(
      const std::array<std::vector<std::vector<uint32_t>>, 3> &execution_orders,
      const std::array<std::vector<uint32_t>, 3> &loop_order_depths,
      const std::array<std::vector<uint32_t>, 3> &instruction_pulse_expressions,
      const std::array<std::vector<uint32_t>, 3> &instruction_index_vector_expressions);
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
  /** GetUniqueId's value; a copy takes a new one. */
  struct UniqueId {
    uint64_t value = Next();
    UniqueId() = default;
    UniqueId(const UniqueId & /*other*/) : value(Next()) {}
    UniqueId &operator=(const UniqueId & /*other*/)
    {
      value = Next();
      return *this;
    }
    static uint64_t Next();
  };
  UniqueId m_uniqueId;
  std::vector<LN_TreePropertyRef> m_treePropertyRefs;
  std::vector<LN_GroupCallFrame> m_groupCallFrames;
  std::vector<LN_LoopFrame> m_loopFrames;
  std::vector<LN_PauseProcessingRequest> m_pauseProcessingRequests;
  std::vector<LN_StepGate> m_stepGates;
  /** AcquireCollisionEventNode's keys (never dereferenced). */
  std::vector<const void *> m_collisionEventNodes;
  mutable std::shared_ptr<const LN_TreeStateLayout> m_treeStateLayout;
  mutable std::shared_ptr<const LN_LeanProgram> m_leanProgram;
  /** TreePropertyOnlySetBySetTreeProperty's answers (1: another op writes it), per ref. */
  mutable std::vector<uint8_t> m_treePropertyOtherWriters;
  mutable std::shared_ptr<const LN_TreeBatchProfile> m_treeBatchProfile;
  mutable std::shared_ptr<const LN_GamePropertyLayout> m_gamePropertyLayout;
  bool m_hasUIEventRoutes = false;
  std::vector<LN_BoolExpression> m_boolExpressions;
  std::vector<LN_FloatExpression> m_floatExpressions;
  std::vector<LN_IntExpression> m_intExpressions;
  std::vector<LN_StringExpression> m_stringExpressions;
  std::vector<LN_VectorExpression> m_vectorExpressions;
  std::vector<LN_QueryExpression> m_queryExpressions;
  uint32_t m_queryRuntimeStateCount = 0;
  uint32_t m_timeFlowStateCount = 0;
  uint32_t m_flowControlStateCount = 0;
  std::vector<LN_ColorExpression> m_colorExpressions;
  std::vector<LN_ValueExpression> m_valueExpressions;
  std::vector<LN_CurveTable> m_curveTables;
  std::vector<LN_VectorCommandPayload> m_vectorCommandPayloads;
  std::vector<LN_GamePropertyCommandPayload> m_gamePropertyCommandPayloads;
  std::vector<LN_RigidBodyConstraintCommandPayload> m_rigidBodyConstraintCommandPayloads;
  std::vector<LN_InstructionHeader> m_onInit;
  std::vector<LN_InstructionHeader> m_onUpdate;
  std::vector<LN_InstructionHeader> m_onFixedUpdate;
  std::vector<LN_Instruction> m_onInitPayloads;
  std::vector<LN_Instruction> m_onUpdatePayloads;
  std::vector<LN_Instruction> m_onFixedUpdatePayloads;
  std::array<LN_ExecBlockProgram, 3> m_execBlockPrograms;
  LN_RegisterExpressionProgram m_registerExpressionProgram;
  LN_DebugMap m_debugMap;
  LN_CompileReport m_compileReport;
  LN_SchedulerSummary m_schedulerSummary;
  LN_ProgramDependencySummary m_dependencySummary;
};

/** Whether an instruction of `opcode` keeps its vector command inputs in the vector command payload
 * table (`command_payload_index`); Set Game Property's are in the game property table, Add Physics
 * Constraint's in the rigid body constraint table. */
bool LN_OpcodeUsesVectorCommandPayload(LN_OpCode opcode);

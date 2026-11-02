/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * The execution frame of one event's instructions (lean runtime plan, phase A11): what the
 * handlers of LN_RuntimeTree (intern/LN_RuntimeTreeOps.inl) read through and write into, so the
 * same handler code can run under another reader and sink.
 *
 * - the READER: Bool, Int, Float, String, Vector, Rotation, Color and Value give an expression's
 *   value (`index`, and what to give when there is none). This frame reads through the tree's
 *   recursive Resolve* functions, as the handlers always did.
 * - the SINK: `command_buffer`, whose Append functions record the writes.
 * - the event, the instructions' headers, the tick context (it belongs to ExecuteEvent, which
 *   changes it for loop bodies and Every N Steps routes) and the sort key every command of the
 *   event gets (the tree's: the command buffer keeps the record order within a tree).
 *
 * A handler is a member template of LN_RuntimeTree taking a frame, so a frame of another kind
 * (the lean VM's, which reads from slots) runs the same code. */

#include <string>
#include <vector>

#include "LN_LeanProgram.h"
#include "LN_LeanReader.h"
#include "LN_RuntimeTree.h"
#include "LN_TreeReader.h"

template<class Reader>
struct LN_ExecFrameT {
  LN_RuntimeTree &tree;
  const LN_Event event;
  const std::vector<LN_InstructionHeader> &instructions;
  LN_CommandBuffer &command_buffer;
  LN_TickContext &context;
  const uint64_t sort_key;
  /** The lean program the frame runs (set by ExecuteEvent for the lean VM's frame, else null):
   * the native ops read the constants the lowering made from it. */
  const LN_LeanProgram *lean = nullptr;
  /** What the reads below ask: LN_TreeReader (the tree's own recursive Resolve functions) or
   * LN_LeanReader (the kind functions straight, plan phase A16). */
  Reader reader;

  LN_ExecFrameT(LN_RuntimeTree &tree,
                const LN_Event event,
                const std::vector<LN_InstructionHeader> &instructions,
                LN_CommandBuffer &command_buffer,
                LN_TickContext &context)
      : tree(tree),
        event(event),
        instructions(instructions),
        command_buffer(command_buffer),
        context(context),
        sort_key(tree.CommandSortKey(0, 0)),
        reader(tree, context)
  {
  }

  /* The reader. */
  bool Bool(const uint32_t index, const bool fallback)
  {
    return reader.Bool(index, fallback);
  }
  int32_t Int(const uint32_t index, const int32_t fallback)
  {
    return reader.Int(index, fallback);
  }
  float Float(const uint32_t index, const float fallback)
  {
    return reader.Float(index, fallback);
  }
  std::string String(const uint32_t index, const std::string &fallback)
  {
    return reader.String(index, fallback);
  }
  MT_Vector3 Vector(const uint32_t index, const MT_Vector3 &fallback)
  {
    return reader.Vector(index, fallback);
  }
  LN_Rotation Rotation(const uint32_t index, const LN_Rotation &fallback)
  {
    return reader.Rotation(index, fallback);
  }
  MT_Vector4 Color(const uint32_t index, const MT_Vector4 &fallback)
  {
    return reader.Color(index, fallback);
  }
  LN_Value Value(const uint32_t index, const LN_Value &fallback)
  {
    return reader.Value(index, fallback);
  }
  const LN_QueryResult &Query(const uint32_t index)
  {
    return reader.Query(index);
  }

  /** The key of every command the event records (the command buffer keeps the record order
   * within one tree, so it is the tree's identity alone). */
  uint64_t SortKey() const
  {
    return sort_key;
  }

  /* Helpers of the handlers (defined in intern/LN_RuntimeTreeOps.inl). */
  KX_GameObject *ResolveCommandTarget(const LN_Instruction &instruction);
  KX_GameObject *ResolveRequiredObject(uint32_t expr_index);
  bool RelativeTransformIsSafe(KX_GameObject *target,
                               const LN_Instruction &instruction,
                               uint32_t instruction_id,
                               LN_CommandBuffer::CommandType command_type);
  KX_GameObject *ResolveAddObjectTemplate(uint32_t expr_index);
  KX_GameObject *ResolveObjectExpression(uint32_t expr_index);
  LN_VectorCommandPayload VectorCommandPayload(const LN_Instruction &instruction);
  LN_GamePropertyCommandPayload GamePropertyCommandPayload(const LN_Instruction &instruction);
  LN_RigidBodyConstraintCommandPayload RigidBodyConstraintCommandPayload(
      const LN_Instruction &instruction);
};

/** The frame of the old interpreter: reads through the tree's recursive Resolve functions. */
using LN_ExecFrame = LN_ExecFrameT<LN_TreeReader>;
/** The frame of the lean runtime's generic form (plan phase A16): reads through the kind functions. */
using LN_LeanFrame = LN_ExecFrameT<LN_LeanReader>;

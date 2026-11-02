/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * The lean program (lean runtime plan, phase A16): a program lowered for the lean VM, and the
 * switch that says which trees run it.
 *
 * Mode: `UPBGE_LN_LEAN` unset = Auto (since plan phase A27: the lean VM runs the events whose ops
 * all have a native handler, the old interpreter the others), `0` or `off` = Off (the old
 * interpreter only), `force` = Force (the lean VM for every event; the unit suite fails a Force run
 * that ran no lean tree). Tests set it with LN_SetLeanModeForTests.
 *
 * The generic form: per event, one lean op for every op of the event's exec block program, in the
 * same order, each running the same handler template as the old interpreter but reading through
 * the lean reader (the kind functions straight), and one entry for every expression with its
 * original index. An op with a native handler (LN_LeanNative, plan phases A18 and A21) has a number
 * in `native` and what its lowering worked out once; the others run the generic handler. The
 * generic form is also the test-only reference mode of decision D10. */

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LN_Types.h"

class LN_Program;

enum class LN_LeanMode : uint8_t { Off = 0, Auto, Force };

/** The mode in force: the test override if set, else UPBGE_LN_LEAN read once. */
LN_LeanMode LN_GetLeanMode();
/** -1: the environment again; 0 Off; 1 Auto; 2 Force. */
void LN_SetLeanModeForTests(int mode);
/** Trees that ran on the lean VM since the process started (a Force run that ran none fails). */
uint64_t LN_LeanTreeRunCount();
void LN_NoteLeanTreeRun();
/** Set by the helper threads of the job pool: a lean program is only made on the main thread. */
bool LN_LeanHelperThread();
void LN_SetLeanHelperThread(bool is_helper);

/** The native handlers an op can have (LN_LeanOp::native); 0 is the generic handler. A native
 * handler does exactly what the generic one does for that op, with what the lowering worked out
 * once instead of at every run (the lean differential tester and the goldens hold it to that). */
enum class LN_LeanNative : uint16_t {
  Generic = 0,
  /** Nop: marks the instruction. */
  Nop,
  /** BranchRoute: marks the instruction when the condition matches. */
  BranchRoute,
  /** Set Game Property Bool / Int / Float (no Modify): the number is recorded as a number. */
  SetNumber,
  /** The vector commands (OpDirectVectorCommand's opcodes): positions, orientations, scales,
   * velocities, Set / Apply Transform Vector, Set Velocity Vector, Apply Physics Vector. */
  VectorCommand,
  /** Modify Property (Set Game Property with a Modify op: Add, Subtract, ... with an optional
   * clamp): the change is recorded and applied to the live value at the flush. */
  ModifyProperty,
};

struct LN_LeanOp {
  /** The op of the event's exec block program this one stands for. */
  uint32_t exec_op_index = LN_INVALID_INDEX;
  /** An LN_LeanNative; 0: the generic handler. */
  uint16_t native = 0;
  /** The op's guard is a Delay's Done: only then the guard may bring its own tree property
   * snapshot (LN_RuntimeTree::DelayTreePropertySnapshotForGuard). */
  bool guard_is_delay_done = false;
  /** The instruction's stamp index (LN_RuntimeTree::InstructionStateIndex) of the op's
   * instruction, LN_INVALID_INDEX when it has none. */
  uint32_t state_index = LN_INVALID_INDEX;
  /** An orientation command (Set World / Local Orientation, Set Transform Vector with the
   * Orientation channel, Apply Transform Vector with the Rotation channel): the index into
   * LN_LeanProgram::rotation_constants of the rotation its constant Euler vector stands for (the
   * value the generic handler works out at every run as the fallback of the rotation read). */
  uint32_t constant_index = LN_INVALID_INDEX;
  /** The op may run in a block (plan phase B1): a native op whose commands go to the typed streams
   * of property writes, transforms, velocities or relative moves (LN_CommandBuffer::BeginBlock),
   * with no latent state of its own. */
  bool block_ok = false;
};

struct LN_LeanEvent {
  std::vector<LN_LeanOp> ops;
  /** Every op of the event has a native handler. Auto mode runs the lean VM only for such an
   * event (one with an op that has none stays on the old interpreter); Force runs them all. */
  bool fully_native = true;
  /** The trees running this event may run it in a block: every op is block_ok and the event is
   * a plain run of ops (no Loop, no Every N Steps gate, no UI event route). */
  bool block_ok = false;
};

struct LN_LeanProgram {
  /** Per event (OnInit, OnUpdate, OnFixedUpdate). */
  std::array<LN_LeanEvent, 3> events;
  /** The constants the native ops point at (LN_LeanOp::constant_index). */
  std::vector<LN_Rotation> rotation_constants;
  uint32_t generic_op_count = 0;
  uint32_t native_op_count = 0;
  /** The expressions per table (Bool, Int, Float, String, Vector, Color, Value, Query); every one
   * is a generic kind op under the lean reader for now. */
  std::array<uint32_t, 8> expression_entries = {};
  uint32_t native_expression_count = 0;
  bool valid = true;
};

/** Lowers `program` (always the generic form for now). */
std::shared_ptr<const LN_LeanProgram> LN_LowerToLean(const LN_Program &program);
/** The lean program as text, for goldens and for reading: counts per event, then every op with the
 * exec op it stands for. */
std::string LN_DumpLeanProgramText(const LN_LeanProgram &lean, const LN_Program &program);

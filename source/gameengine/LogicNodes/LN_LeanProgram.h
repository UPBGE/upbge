/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * The lean program (lean runtime plan, phase A16): a program lowered for the lean VM, and the
 * switch that says which trees run it.
 *
 * Mode (plan "Attribute and Retirement", step R3, 2026-11-02: the lean VM is the only interpreter):
 * Lean, the default = every op through its native handler where it has one and every event that has
 * a compiled route through it, the others through the generic handlers; Reference (`UPBGE_LN_LEAN`
 * = `reference`, `generic`, `0` or `off`) = every op through the generic handlers, no native op, no
 * route, no lean memo (decision D10: the test oracle, also a way out if a native handler were
 * wrong). Before R3 there were Off (the old interpreter), Auto and Force. Tests set it with
 * LN_SetLeanModeForTests (0 Reference, 1 or 2 Lean). The unit suite fails a run that ran no lean tree.
 *
 * The generic form: per event, one lean op for every op of the event's exec block program, in the
 * same order, each running the same handler template as the old interpreter but reading through
 * the lean reader (the kind functions straight), and one entry for every expression with its
 * original index. An op with a native handler (LN_LeanNative, plan phases A18 and A21) has a number
 * in `native` and what its lowering worked out once; the others run the generic handler. Every op
 * through its generic handler is the reference mode (LN_LeanMode::Reference). */

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LN_Types.h"

class LN_Program;

enum class LN_LeanMode : uint8_t { Reference = 0, Lean = 1 };

/** The mode in force: the test override if set, else UPBGE_LN_LEAN read once. */
LN_LeanMode LN_GetLeanMode();
/** -1: the environment again; 0 Reference; 1 or 2 Lean (the old Auto and Force). */
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

/** Compiled routes (plan phase A33): `UPBGE_LN_ROUTES` = 0 / off (the lean VM walks its blocks op by
 * op, as before), unset / 1 / on (the default since A33's gate: an event with a route runs it). Tests set it with LN_SetRouteModeForTests;
 * a run with routes on that ran none says so through LN_RouteRunCount. */
enum class LN_RouteMode : uint8_t { Off = 0, On };
LN_RouteMode LN_GetRouteMode();
/** -1: the environment again; 0 Off; 1 On. */
void LN_SetRouteModeForTests(int mode);
/** Events that ran as a compiled route since the process started (per-thread slots, as the lean
 * run counter). */
uint64_t LN_RouteRunCount();
void LN_NoteRouteRun();

/** How a route op's guard is read (the op's guard bool expression). */
enum class LN_RouteGuard : uint8_t {
  /** No guard: the op always runs. */
  None = 0,
  /** An InstructionExecuted guard (a flow from an op that ran in this pass): the stamp of
   * `guard_state` read in place, exactly LN_RuntimeTree::InstructionExecutedNow. */
  Ran,
  /** Any other guard: the lean reader's Bool. */
  Generic,
};

/** One op of a compiled route: a native op of the event with everything its handler looks up at
 * every run worked out at the lowering (its instruction and payload, its stamp, its guard). */
struct LN_RouteOp {
  /** An LN_LeanNative: Nop, BranchRoute, SetNumber, VectorCommand or ModifyProperty. */
  uint16_t native = 0;
  LN_RouteGuard guard = LN_RouteGuard::None;
  /** The guard's bool expression (Generic). */
  uint32_t guard_expr = LN_INVALID_INDEX;
  /** The stamp index of the instruction a Ran guard asks about. */
  uint32_t guard_state = LN_INVALID_INDEX;
  /** The op's own stamp index (LN_LeanOp::state_index). */
  uint32_t state_index = LN_INVALID_INDEX;
  /** LN_LeanOp::constant_index (orientation commands). */
  uint32_t constant_index = LN_INVALID_INDEX;
  /** The exec op's instruction index (diagnostics of the vector handler). */
  uint32_t instruction_index = LN_INVALID_INDEX;
  /** Into the program's own tables: the lean program is dropped whenever the program grows. */
  const LN_Instruction *instruction = nullptr;
  const LN_GamePropertyCommandPayload *property_payload = nullptr;
  const LN_VectorCommandPayload *vector_payload = nullptr;
};

/** A block of a route: its ops, and the Every N Steps gate it runs behind (LN_ExecBlock). */
struct LN_RouteBlock {
  uint32_t first_op = 0;
  uint32_t op_count = 0;
  uint32_t step_gate_index = LN_INVALID_INDEX;
};

/** An event as a compiled route: what LN_RuntimeTree::RunBlocksWith and the native handlers do for
 * it, without the per-op lookups, checks and dispatch. Made for an event whose ops are all native,
 * with no Loop, no UI event route and no Delay-Done guard. */
struct LN_LeanRoute {
  std::vector<LN_RouteOp> ops;
  std::vector<LN_RouteBlock> blocks;
};

struct LN_LeanEvent {
  std::vector<LN_LeanOp> ops;
  /** The event as a compiled route (plan phase A33); `has_route` false: none could be made. */
  bool has_route = false;
  LN_LeanRoute route;
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
  /** Per query expression: 1 when it is a Get Distance between fixed ends
   * (LN_DistanceQueryEndsAreFixed: taken once a pass), worked out at the lowering instead of at
   * every read (plan phase A33). */
  std::vector<uint8_t> query_fixed_distance;
  /** Per Bool, Int, Float and Vector expression: 1 when the lean reader keeps its value for the
   * tick in its memo register (plan phase A33), as the old interpreter's memo does: a kind the memo
   * may keep (LN_MEMO_STORE), every input of it pure all the way down (the lean reader tracks no
   * taint), read at least twice by the program's expressions and native ops, and not a single
   * property read (cheaper than its register). The memo never changes a result (plan phase A15). */
  std::vector<uint8_t> memo_bool;
  std::vector<uint8_t> memo_int;
  std::vector<uint8_t> memo_float;
  std::vector<uint8_t> memo_vector;
  uint32_t memo_count = 0;
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

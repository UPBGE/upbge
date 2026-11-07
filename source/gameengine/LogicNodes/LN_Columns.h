/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * Plan C1 (2026-11-03, `~/Desktop/CPP_LN_TESTS/Logic_Nodes_C1_Plan.md`): columns inside Logic
 * Nodes. On by default; `UPBGE_LN_COLUMNS=0` is the way back (a test's choice wins).
 *
 * The way ECS systems run (Unity DOTS, Flecs): the trees of one program are one archetype; a run
 * of consecutive trees of it (one tree is a run since plan C2 P1; 16 before,
 * UPBGE_LN_COLUMN_MIN_RUN) is cut into chunks of 256 NPCs (on the job pool when it
 * has helpers) and each node of the program runs as one loop over the chunk, its values in arrays
 * (float, int, bool, vector axes), a route as a per-NPC mask. Nothing is recorded: each write op's
 * value and mask stay in the run's arrays, and the command buffer's flush applies every NPC's
 * writes at its tree's sort key, in op order, with the very functions the flush uses
 * (LN_CommandBuffer::Apply*Now). The game properties (and the transform when Get Axis Vector reads
 * it) are gathered by the snapshot's own capture (LN_Snapshot::CaptureGamePropertiesOnly).
 *
 * What runs in columns (LN_CompileColumnProgram says why a program does not): with a kernel (C1,
 * C1-7) the flow ops, number and property writes, moves, velocities and forces, Set Tree Property,
 * Once, Flow Router, Repeat Limit, Every N Steps gates, the time nodes, Store Value, Boolean Edge,
 * Flow Gate, Move To / Rotate To / Translate / Align Axis, Set Visible / Color / Teleport, the math,
 * compare, vector and rotation values, Get Object Attribute, Get Axis Vector, Get Distance; through
 * the bridge (C1-8: the VM's own code NPC by NPC inside the run) most other kinds and the ops on
 * BridgeableOp's list. Since C1-9: a bridged run's nodes in segments (the helpers take those that
 * may run there, the main thread the rest, in lane order), objects with several trees in groups,
 * objects each NPC finds while playing (an Object property, an unwritten tree property, Get Object
 * By Name, Get Child (By Name) of the owner) as distance ends, live pose reads and move targets (a
 * turret), Loops (a body block as one bridged node), Add Object / Set Parent / Replace Mesh under
 * their rules, Get Average Center Position. UI routes, input devices, file variables, scene and
 * game nodes keep the program on the lean VM.
 *
 * Exactly the VM's result, next to any other trees, because:
 * - a run's writes behave as its trees' recorded commands (LN_ExternalWrites): the flush applies
 *   each NPC's at its sort key among the other commands (another tree may write or move the same
 *   object; nothing is applied to an object removed earlier in the flush), its merge of
 *   last-write-wins writes decides as if they were recorded (gate count, merge walk), a read that
 *   applies an object's pending moves (Get Distance, a ray's caster, Add Object's reference...)
 *   applies its column moves too, merged by sort key (one tree's moves of the read's object and
 *   its parents together, in op order: a turret and its NPC), and the pending checks and marks
 *   see them;
 * - each NPC's pass does the VM's bookkeeping (LN_RuntimeTree::BeginColumnPass / EndColumnPass,
 *   op stamps, flow states, tree properties written back at once);
 * - a lane is a tree that needs nothing more of ExecuteReady (On Init done, not paused to UI
 *   routes), no Run Logic Tree pass runs it this tick, owned by an object without a parent that
 *   is no menu copy and has no other tree (or whose trees form a group with its neighbours');
 * - inside a run no lane reads another: no lane's object is one a column program names (with its
 *   parents), and a run is consecutive trees in the recording order; an object a distance reads
 *   has its pending moves applied when the run starts only when some NPC really reads it (whatever
 *   its route, or behind a gate that lets its route run; end B only when end A is valid), which no
 *   NPC can tell from its own read applying them. */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

#include "LN_CommandBuffer.h"
#include "LN_Types.h"

class KX_GameObject;
class LN_JobPool;
class LN_Program;
class LN_RuntimeTree;
struct LN_TickContext;
struct LN_TickReadContext;

enum class LN_ColumnMode : uint8_t { Off = 0, On, All };
/** UPBGE_LN_COLUMNS: unset / 1 / on = On (the default since plan C1-6), 0 / off = Off, all = All
 * (plan C2 P0: every tree whose program lowers runs in columns, runs of 1 included even where
 * that is slower; for tests and coverage, never the default); a test's choice wins. */
LN_ColumnMode LN_GetColumnMode();
/** -1: the environment again; 0 Off; 1 On; 2 All. */
void LN_SetColumnModeForTests(int mode);
/** Trees run in columns since the process started (tests: a run that formed none). */
uint64_t LN_ColumnLaneCount();
/** Lanes the bridge (plan C1-8) gave back to the VM: a pending move when their run started. */
uint64_t LN_ColumnBridgeFallbackCount();
/** Runs that started only with nothing pending (bridged or quiet) split at a lane another tree
 * pushed (plan C2 fix 1): the lanes around it ran, it recorded on the VM. */
uint64_t LN_ColumnPushedSplitCount();
/** Segments of bridged runs (plan C1-9) since the process started: those the helpers worked
 * through and those the main thread took (nodes only it may run). */
void LN_ColumnSegmentCounts(uint64_t &r_helper_segments, uint64_t &r_main_segments);
/** Lanes given back to the VM because an object one found while playing (plan C1-9's runtime
 * ends) is a lane's object this tick, or a parent of one. */
uint64_t LN_ColumnRuntimeFallbackCount();
/** Runs split lane by lane and lanes given back to the VM because their lanes depend on an object
 * with pending moves (plan C2 P2: a parented lane's ancestor, an object a program names). */
void LN_ColumnDependCounts(uint64_t &r_splits, uint64_t &r_fallbacks);
/** Lanes whose own property writes were applied ahead of the flush (plan C1-11 C-4) since the
 * process started. */
uint64_t LN_ColumnPreAppliedLaneCount();
/** The flush's write counts checked against a walk of every lane (plan C1-11 C-1): mismatches
 * since the process started ("Logic Nodes: columns: write counts differ"). The check runs with
 * UPBGE_LN_COLUMN_COUNT_VERIFY=1 or a test's switch (-1: the environment again; 0 off; 1 on). */
uint64_t LN_ColumnCountMismatchCount();
void LN_SetColumnCountVerifyForTests(int mode);
/** UPBGE_LN_COLUMN_PREAPPLY: 0 / off = Off, unset / 1 = Auto (with helper threads, from
 * kMinPreApplyLanes lanes), force = always (also on the main thread alone, any number of lanes:
 * tests). -1: the environment again; 0 Off; 1 Auto; 2 Force. */
void LN_SetColumnPreApplyModeForTests(int mode);
/** UPBGE_LN_COLUMN_NATIVE_RAYS (plan C2 P3): a Raycast with constant settings as a column node
 * (QRay). -1: the environment again; 0 off (bridged as before); 1 on. Lowering reads it. */
void LN_SetColumnNativeRaysForTests(int mode);
/** Direct column programs (plan C2 P5): a program event the column compiler refuses runs in its
 * tree's place as a run of 1 whose blocks the column runner walks itself (Every N Steps gates, a
 * Loop's Completed route, a Loop body) and whose ops run through the shared op handlers,
 * recording into the tick's buffer where the old system records them; its reads apply pending
 * moves where the old system's do (the same handlers and kinds). `UPBGE_LN_COLUMN_DIRECT`:
 * 0 / off = Off (the old system runs such trees), unset / 1 = Auto (only where the old system is
 * no faster: an event without a compiled route, and a program that is no tree batch candidate or
 * a tick without helpers), force = every program event direct (tests: the direct walk against the
 * old system). -1: the environment again; 0 Off; 1 Auto; 2 Force. Lowering reads it. */
enum class LN_ColumnDirectMode : uint8_t { Off = 0, Auto, Force };
LN_ColumnDirectMode LN_GetColumnDirectMode();
void LN_SetColumnDirectModeForTests(int mode);
/** Plan C2 P6: whether a tree no wide run takes runs the column system's pass
 * (LN_RuntimeTree::ColumnExecuteReady: the event's compiled route or the column walk of its
 * blocks, the op handlers recording where the old system records) instead of the old system's
 * ExecuteReady: columns on and direct programs not off. The manager, its tree batches and Run
 * Logic Tree passes ask it; the tests' reference rigs never do. */
bool LN_ColumnPassesOn();
/** Passes the column system ran outside wide runs (ColumnExecuteReady, ColumnExecuteForcedUpdate)
 * since the process started. */
uint64_t LN_ColumnDirectLaneCount();
/** Counts `passes` such passes (their callers count in bulk: per tick, per tree batch). */
void LN_AddColumnPasses(uint64_t passes);
/** The kind input table compiled into the column compiler (`intern/LN_KindInputsData.inc`), one
 * "Domain::Kind: field:family ..." line a kind, as the golden `kind_inputs_v0` holds it. */
std::string LN_ColumnKindInputsText();
/** Tests: what the column compiler's walk says expression `index` of table `family` (B I F S V R
 * C A Q) of `program` may read: "unknown: <why>", or the query indices it reaches ("q0 q2"). */
std::string LN_ColumnQueryReachForTests(const LN_Program &program, char family, uint32_t index);

/** A program's event lowered to columns. */
struct LN_ColumnProgram;
/** Lowers `program`'s `event`; nullptr with the reason in `r_refusal` when it cannot be. Main
 * thread (it may lower the lean program). `allow_direct` (the runner's): a refused event (every
 * event in LN_ColumnDirectMode::Force) as a direct program, `r_refusal` still saying why the
 * compiler refused it. */
std::shared_ptr<const LN_ColumnProgram> LN_CompileColumnProgram(const LN_Program &program,
                                                                 LN_Event event,
                                                                 std::string *r_refusal = nullptr,
                                                                 bool allow_direct = false);
/** The column program as text (tests). */
std::string LN_DescribeColumnProgram(const LN_ColumnProgram &program);
/** What a column program costs in memory (plan C2 P0): its own arrays (once per program and
 * event), what a run of it holds per lane (output columns, write masks, found objects, capture
 * bookkeeping; kept between ticks), and one chunk's scratch (kChunkLanes lanes of every slot; one
 * per thread, kept). */
struct LN_ColumnMemory {
  size_t program_bytes = 0;
  size_t lane_bytes = 0;
  size_t chunk_bytes = 0;
};
LN_ColumnMemory LN_ColumnProgramMemory(const LN_ColumnProgram &program);
/** Runs in a row (no tree between them) whose trees have this many column nodes or more on
 * average are all taken, a small program's short runs included (plan C2 P1): left out, their
 * trees would cut a wave apart, make the flush list the lanes' writes, and be too few in a row
 * for a tree batch. Smaller trees take the size floors. */
constexpr size_t LN_COLUMN_TAKE_ALL_AVERAGE_NODES = 26;
/** The fewest trees a run of `program` needs by its size (plan C2 P1, the D3 gate: a small
 * program's run start and end cost more than its trees save where the old system runs them as
 * tree batches): 4 below 16 column nodes, 2 below LN_COLUMN_TAKE_ALL_AVERAGE_NODES (26), else 1.
 * The runner applies it only with helpers (SetPlanHelpers), not when a shortest run was asked
 * for (UPBGE_LN_COLUMN_MIN_RUN, SetMinRunLanes), not in the All mode. */
size_t LN_ColumnSizeFloor(const LN_ColumnProgram &program);
/** A column program's nodes (not its constants): what LN_ColumnSizeFloor measures. */
size_t LN_ColumnNodeCount(const LN_ColumnProgram &program);

/** What the column system does with a program's event (2026-11-06: the play report the Logic
 * Nodes sidebar shows, LN_Manager's WriteGameReport). */
struct LN_ColumnVerdict {
  enum class Kind : uint8_t {
    /** The program has no ops on this event. */
    None,
    /** The column system is switched off (UPBGE_LN_COLUMNS=0): the old system runs it. */
    Off,
    /** Its trees run in wide runs (node by node over arrays). */
    Wide,
    /** Each of its trees runs alone as a column pass, at the old system's speed (`reason`). */
    Pass,
  };
  Kind kind = Kind::None;
  /** A wide run whose chunks stay on the main thread (`reason` says why). */
  bool main_thread = false;
  std::string reason;
};

/** How to write a tree so the column system takes it into wide runs, for a refusal reason a
 * natural tree meets (LN_ColumnVerdict::reason of a Pass); empty: no advice. */
std::string LN_ColumnRefusalHint(const std::string &reason);
/** The nodes whose routes run every step in a program that also has Every N Steps routes: they
 * keep its trees awake between their steps (a Boolean Edge, a collision or an event sensor is
 * a route of its own that runs every step). Empty: the trees sleep, or nothing is slowed. */
std::vector<std::string> LN_NodesKeepingTreesAwake(const LN_Program &program);

/** The runs of one tick. Its writes are LN_ExternalWrites of the buffer it runs against (Plan sets
 * them, EndTick takes them away): the flush applies each NPC's at its sort key among the commands,
 * and a read applying an NPC's pending moves applies its column moves. */
class LN_ColumnRunner final : public LN_ExternalWrites {
 public:
  LN_ColumnRunner();
  ~LN_ColumnRunner() override;
  LN_ColumnRunner(const LN_ColumnRunner &) = delete;
  LN_ColumnRunner &operator=(const LN_ColumnRunner &) = delete;

  /** Whether an object is a menu copy (the manager's scene); unset: none is. `any_overlay_copy`
   * (optional) says once a tick whether the scene has any: without, no lane is asked (a hash
   * lookup per lane, 10,000 lanes a step). */
  void SetOverlayTest(std::function<bool(KX_GameObject *)> is_overlay_copy,
                      std::function<bool()> any_overlay_copy = {});
  /** Runs shorter than this record one tree after the other (tests set 1). */
  void SetMinRunLanes(size_t lanes);
  /** Lowers `program` for Frame Update and Logic + Physics Step now, the events it has ops for
   * (plan C2 P1: when a scene starts or a tree is installed, not in the tick a program first
   * runs); `tree` keeps the program's entry. Main thread. Nothing while columns are off. */
  void PrepareProgram(const LN_Program &program, LN_RuntimeTree *tree = nullptr);
  /** What the column system does with `program`'s `event` (lowered now if it was not). Main
   * thread. */
  LN_ColumnVerdict Verdict(const LN_Program &program, LN_Event event);
  /** How many column programs PrepareProgram and Plan compiled (tests). */
  static uint64_t CompiledCountForTests();
  /** How many lanes the lane map (lanes with moves, by object) looked at: each once a tick
   * (tests; plan C2 P1 found it made again from every lane after each run). */
  static uint64_t LaneMapWorkForTests();
  /** Waves run (consecutive short runs whose chunks ran together on the pool, plan C2 P1). */
  static uint64_t WaveCountForTests();
  /** The fewest lanes a wave takes (0: none; default 64, UPBGE_LN_COLUMN_WAVE_MIN). */
  void SetMinWaveLanes(size_t lanes);
  /** The helpers of the pool the tick's RunLanes calls get (0: none: no waves, no size floors),
   * told before Plan. */
  void SetPlanHelpers(size_t helpers);

  /** Decides which of the tick's runnable `trees` (in recording order) run in columns, the
   * buffer's flush and reads taking their writes. `forced_trees`: the trees a Run Logic Tree pass
   * runs this tick (they stay out). `read`: the tick read context the trees' snapshots read
   * (timing); without one a program reading the tick's timing stays out. False: none (columns
   * off, no run of trees that lower). */
  bool Plan(LN_RuntimeTree *const *trees,
            size_t count,
            LN_Event event,
            LN_CommandBuffer &buffer,
            const std::unordered_set<LN_RuntimeTree *> *forced_trees,
            const LN_TickReadContext *read = nullptr);
  bool IsLane(size_t index) const;
  size_t LaneCount() const;
  /** The lanes' game properties, captured as a snapshot would (instead of their whole snapshot). */
  void CaptureLanes(uint64_t tick_index, LN_JobPool *pool);
  /** Runs the run of lanes starting at `begin` (consecutive lanes of one program); returns where
   * it ends. While the buffer records, main thread. */
  size_t RunLanes(size_t begin,
                  LN_CommandBuffer &buffer,
                  const LN_TickContext &context,
                  LN_JobPool *pool);
  /** Before a tree batch records: its helper threads may ask about the runs' pending moves
   * (through the buffer's pending checks); what they read is made here, on the main thread. */
  void PrepareForBatchReads() const;
  /** Forgets the tick (the trees' lane marks, the buffer's link to the writes). Call it after
   * the buffer's flush. */
  void EndTick();
  /** Why the last Plan returned false ("" when it planned lanes). */
  const std::string &LastRefusal() const;
  /** When the last Plan planned lanes: the trees that stayed out because their program does not
   * lower, the most of one program first ("'Brain' (120 trees): no column kernel: ..."); "" when
   * every runnable tree's program lowers. */
  const std::string &LastOutsideNote() const;
  /** This tick's lanes the runs gave back to the lean VM (a pending move when their run started,
   * an object found while playing that is a lane's): they ran there, not in columns. */
  size_t GivenBackCount() const;

  /* LN_ExternalWrites */
  bool HasWrites() const override;
  void CountWrites(uint32_t &r_total, uint32_t &r_last_write_wins) const override;
  size_t ListWrites(const std::vector<const void *> &buffer_objects,
                    bool buffer_can_merge) override;
  const LN_ExternalWriteKey &WriteKey(size_t index) const override;
  void MarkSuperseded(size_t index) override;
  void ApplyWritesBefore(uint64_t sort_key, LN_CommandBuffer &buffer) override;
  void PrepareApply(LN_CommandBuffer &buffer) override;
  bool HasPendingMove(const KX_GameObject *object) const override;
  bool PendingMoveSortKey(const KX_GameObject *object, uint64_t &r_sort_key) const override;
  void ApplyPendingMoves(uint64_t sort_key, KX_GameObject *const *objects, size_t count) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

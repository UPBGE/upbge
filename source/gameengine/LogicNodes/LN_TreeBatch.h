/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeBatch.h
 *  \ingroup logicnodes
 *
 * Tree batches (2026-10-31, step 7): many trees recorded at once on the job pool, with exactly
 * the results of recording them one after the other.
 *
 * During a tick the trees only record their writes; the flush applies them after. The one thing
 * that changes the scene while trees record is a read that must see the step's earlier moves
 * (Get Distance with an object end): it applies the read object's pending moves at once. A batch
 * holds trees for which no such apply can happen: each moves only its own object, which no other
 * tree of the batch moves or measures, and reads its own position only before its first move. The
 * scene then stays as it is through the whole batch, so every read gives what it gives in a run
 * one after the other, on any thread.
 *
 * Which trees qualify is decided twice. Once per program (LN_AnalyzeTreeBatchSafety): every node
 * the program can run is on a list of nodes known to read and write only what a batch allows.
 * Then each tick, per tree (LN_Manager): its object has no parent, is no menu copy, has no pending
 * move of an earlier tree, and is not measured by a tree of the batch; its object has no other
 * tree, unless the program measures no distance from its own object (LN_TreeMayShareItsObject).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class LN_CommandBuffer;
class LN_JobPool;
class LN_Manager;
class LN_Program;
struct LN_Instruction;
class LN_RuntimeTree;
struct LN_QueryExpression;
struct LN_RuntimeProfileCounters;
struct LN_TickContext;

/** Off: every tree records on the main thread, one after the other. Auto (the default): runs of
 * at least LN_TREE_BATCH_MIN_TREES batch safe trees are batches, when the job pool has helpers.
 * Force: any run of two or more is a batch of at least two chunks, also without helpers (tests:
 * the acceptance suite plays with it to put every batch safe tree through the batch path). */
enum class LN_TreeBatchMode : uint8_t {
  Off = 0,
  Auto,
  Force,
};
/** The mode: UPBGE_LN_BATCHES = 0 (Off), 1 or unset (Auto), force (Force), read once; a test's
 * choice (LN_SetTreeBatchModeForTests) wins. */
LN_TreeBatchMode LN_GetTreeBatchMode();
void LN_SetTreeBatchModeForTests(LN_TreeBatchMode mode);
/** UPBGE_LN_BATCH_WIDE (plan phase B0): 0 = the rules of step 7 only (its allow lists, and a tree
 * batches only as its object's one tree), else also the B0 reads (Get Object Attribute of the owner
 * or a constant object) and trees sharing their object (LN_TreeMayShareItsObject). -1 here = the
 * environment again. Set it before a program's batch profile is first worked out (it is kept with
 * the program). */
bool LN_TreeBatchWide();
void LN_SetBatchWideForTests(int wide);
void LN_ClearTreeBatchModeForTests();

/** Parallel snapshot capture: at least this many trees to capture, and chunks of at least
 * LN_PARALLEL_CAPTURE_MIN_CHUNK of them. */
constexpr size_t LN_PARALLEL_CAPTURE_MIN_TREES = 256;
constexpr size_t LN_PARALLEL_CAPTURE_MIN_CHUNK = 64;
/** UPBGE_LN_CAPTURE_PARALLEL=force captures any two or more trees on the pool, in chunks of one
 * (tests: the acceptance suite plays with it); 0 never; unset the limits above. */
size_t LN_ParallelCaptureMinTrees();
size_t LN_ParallelCaptureMinChunk();

/** Tests (lean runtime plan A2): the reason the tree batch lists refuse a kind judged by the kind
 * alone (a default expression or instruction of that kind, in an empty program), or nullptr when
 * it is accepted. For a kind whose acceptance depends on more than the kind (a move of the owner
 * only ...) the default fails the condition, so a refusal is no proof that a kind is refused:
 * the kind catalog calls those kinds Conditional. `value` is the kind's enumerator value in the
 * enum of `domain` (LN_KindCatalog.h). */
const char *LN_BatchKindRefusalForTests(uint8_t domain, uint16_t value);
/** Why expression `index` of table `domain` (LN_KindDomain: Bool .. Query) of `program` may not be
 * worked out on a helper thread by its own kind (nullptr: it may; its inputs are not looked at),
 * or why `instruction`'s op may not run there: the tree batches' allow lists (plan C1-9: the
 * column bridge runs such nodes on the helpers too). */
const char *LN_BatchExpressionRefusal(const LN_Program &program, uint8_t domain, uint32_t index);
const char *LN_BatchOpRefusal(const LN_Program &program, const LN_Instruction &instruction);

/** Fewer trees in a row are recorded on the main thread: a batch's start and end cost about as
 * much as 4-13 us of logic (measured with the job pool's round trip). */
constexpr size_t LN_TREE_BATCH_MIN_TREES = 64;
/** The fewest trees a chunk gets (Auto), so a chunk is worth handing to a thread. */
constexpr size_t LN_TREE_BATCH_MIN_CHUNK_TREES = 16;

/** In a query's cache key: the value is kept for the whole execution scope (one pass of the
 * tree's event, or one Loop iteration), not only for one instruction. */
constexpr uint64_t LN_QUERY_PASS_KEY_BIT = uint64_t(1) << 63;

/** A Get Distance whose ends cannot change within a pass of the tree: the owner, an object named
 * in the tree, a constant position or a position of the tree's snapshot. Its distance is taken
 * once a pass, at its first use (which sees the step's writes recorded until then). */
bool LN_DistanceQueryEndsAreFixed(const LN_Program &program, const LN_QueryExpression &expression);

/** What a program's trees need to be recorded in a batch. */
struct LN_TreeBatchProfile {
  /** Every node of the program is batch safe. */
  bool eligible = false;
  /** Why not, for the log ("" when eligible). */
  std::string reason;
  /** The value expressions (constant objects) a Get Distance measures from or to: their objects
   * must not be moved by a tree of the batch, and have no pending move when it starts. */
  std::vector<uint32_t> endpoint_value_expressions;
  /** Some Get Distance measures from or to the tree's own object. That read applies the pending
   * moves of the object, so another tree of the same object that moves it, recorded before it,
   * would change its answer. */
  bool reads_owner_distance = false;
  /** Some op moves the tree's own object (the transform and relative streams: what a Get
   * Distance from it applies). Set only for an eligible program. */
  bool moves_owner = false;
};

/** Whether a tree whose object has other trees may still join a batch. `object_movers`: how many
 * runnable trees of the object, this one included, have a batch-safe program that moves it
 * (a tree whose program is not batch safe records on the main thread, and a run never holds a
 * tree whose object has a pending move, so only batch-safe movers can sit before it in a run).
 * Yes when nothing the tree reads applies a pending move of its object (every other read in a
 * batch tree sees the scene as it is), or when no other tree of the object moves it. A crowd
 * with a brain (Get Distance from the NPC to the Player, moves) and a second tree on each NPC
 * that does not move it batches both. */
inline bool LN_TreeMayShareItsObject(const LN_TreeBatchProfile &profile,
                                     const uint32_t object_movers)
{
  return !profile.reads_owner_distance || object_movers <= (profile.moves_owner ? 1u : 0u);
}

/** Reads the whole program (every expression and every op of its Frame Update and Logic +
 * Physics Step routes; On Init runs before a tree can join a batch). Conservative: a node not
 * known to be safe keeps the program's trees one after the other. */
LN_TreeBatchProfile LN_AnalyzeTreeBatchSafety(const LN_Program &program);

/** Records one batch: consecutive trees split into chunks of consecutive trees, each chunk
 * recorded into a buffer of its own on a job pool thread, the chunks then appended to the tick's
 * buffer in order (LN_CommandBuffer::AppendRecordedChunk). The buffer ends exactly as if the
 * trees had recorded into it one after the other. The chunk buffers are kept for the next batch,
 * so a warm game allocates nothing here. */
class LN_TreeBatchRecorder {
 public:
  LN_TreeBatchRecorder();
  ~LN_TreeBatchRecorder();
  LN_TreeBatchRecorder(const LN_TreeBatchRecorder &) = delete;
  LN_TreeBatchRecorder &operator=(const LN_TreeBatchRecorder &) = delete;

  /** `buffer` records (BeginRecording) on the calling thread. `profile_counters`: one per tree,
   * or null. `chunk_count` is clamped to 1..count. Returns how many chunks read a pending move
   * (never, for trees a batch may hold): their trees are in LastChunkTreesThatReadAMove. */
  size_t Record(LN_JobPool &pool,
                LN_CommandBuffer &buffer,
                LN_RuntimeTree *const *trees,
                size_t count,
                const LN_TickContext &context,
                LN_RuntimeProfileCounters *profile_counters,
                LN_Manager *manager,
                size_t chunk_count);
  /** The trees (indexes into the last Record's `trees`) of chunks that read a pending move. */
  const std::vector<size_t> &LastChunkTreesThatReadAMove() const
  {
    return m_treesThatReadAMove;
  }
  /** The last Record's time appending the chunks (main thread, after the threads' work). */
  double LastJoinMs() const
  {
    return m_lastJoinMs;
  }
  /** Plan C2 P6: the trees take the column system's pass (ColumnExecuteReady) instead of the old
   * system's ExecuteReady (the manager sets LN_ColumnPassesOn; test rigs as they need). */
  void SetColumnPasses(const bool column_passes)
  {
    m_columnPasses = column_passes;
  }

 private:
  bool m_columnPasses = false;
  std::vector<std::unique_ptr<LN_CommandBuffer>> m_chunks;
  std::vector<size_t> m_treesThatReadAMove;
  double m_lastJoinMs = 0.0;
};

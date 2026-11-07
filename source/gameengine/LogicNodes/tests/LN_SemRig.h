/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * A small rig for the semantics tests (lean runtime plan, phase A4): one tree of a hand-built
 * program on each of a few positioned objects, run tick by tick in the manager's order with the
 * production setup spelled out (typed streams, the register evaluator, its per-program memo
 * heuristic) or with one switch changed (SemMode). Header only, in namespace ln_test::sem. */

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "KX_NodeRelationships.h"
#include "LN_Columns.h"
#include "LN_DiffTrees.h"
#include "LN_JobPool.h"
#include "LN_TreeBatch.h"

namespace ln_test::sem {

using diff::GenNode;
using diff::TreeBuilder;

/** The switches that change results today (plan decision D1): the register evaluator and its
 * memo, the heuristic that turns the memo on per program, and profiling. */
struct SemMode {
  const char *name;
  /** The per-tick memo of pure expressions (the only switch left after plan phase A15). */
  bool memo;
  bool profiled;
};

/** Production: the memo on, not profiled. */
inline const SemMode kProduction = {"production", true, false};
/** The same as production (kept for the tests that name both). */
inline const SemMode kMemoOn = {"memo on", true, false};
/** No memo: expressions are worked out at each read. */
inline const SemMode kMemoOff = {"memo off", false, false};
/** Profiling on. */
inline const SemMode kProfiled = {"profiled", true, true};

inline std::vector<SemMode> AllModes()
{
  return {kProduction, kMemoOn, kMemoOff, kProfiled};
}

inline std::string Bits(const float value)
{
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08x", bits);
  return text;
}

class SemRig {
 public:
  /** `owners` objects "Obj0".. each with a tree of `program`; more objects are added by Add. */
  /** `scene_index_stride`: tree i's sort key is that of scene object i x stride (2: room for other
   * trees' keys between them, AddTree). */
  SemRig(const std::shared_ptr<LN_Program> &program,
         const SemMode &mode = kProduction,
         const uint32_t owners = 1,
         const uint32_t scene_index_stride = 1)
      : m_program(program), m_mode(mode)
  {
    LN_RuntimeTree::SetMemoForTests(mode.memo ? 1 : 0);
    /* As the scene's buffer: its objects are real (plan phase A33's pending move marks). */
    m_buffer.SetMarksPendingMoves(true);
    for (uint32_t index = 0; index < owners; index++) {
      AddObject("Obj" + std::to_string(index), MT_Vector3(0.0f, 0.0f, 0.0f));
    }
    for (uint32_t index = 0; index < owners; index++) {
      m_trees.push_back(std::make_unique<LN_RuntimeTree>(
          program, &m_objects[index].object, index * scene_index_stride, 0));
      m_trees.back()->SetStepSpread(index);
      /* Each owner has this one tree (what the manager's tree index notes). */
      m_trees.back()->GetBatchNote().sole_tree = true;
    }
    Publish();
  }
  ~SemRig()
  {
    m_trees.clear();
    LN_SetSceneAccessForTests(nullptr);
    LN_RuntimeTree::SetMemoForTests(-1);
  }
  SemRig(const SemRig &) = delete;

  /** An object that is not a tree's owner (a "Target"...), found by name through the seam. */
  KX_GameObject &AddObject(const std::string &name, const MT_Vector3 &position)
  {
    m_objects.emplace_back(position);
    KX_GameObject &object = m_objects.back().object;
    object.SetName(name);
    if (!m_trees.empty() || m_published) {
      Publish();
    }
    return object;
  }

  /** A tree of another program on object `object_index` (its only tree), with the sort key of
   * scene object `scene_index`, recorded before the others (`first`) or after them. */
  LN_RuntimeTree &AddTree(const std::shared_ptr<LN_Program> &program,
                          const size_t object_index,
                          const uint32_t scene_index,
                          const bool first)
  {
    auto tree = std::make_unique<LN_RuntimeTree>(
        program, &m_objects[object_index].object, scene_index, 0);
    tree->SetStepSpread(uint32_t(m_trees.size()));
    tree->GetBatchNote().sole_tree = true;
    LN_RuntimeTree &added = *tree;
    if (first) {
      m_trees.insert(m_trees.begin(), std::move(tree));
    }
    else {
      m_trees.push_back(std::move(tree));
    }
    return added;
  }

  /** A tree of `program` on object `object_index` after the others, with the sort key of scene
   * object `scene_index` and applied tree index `tree_index` (an object's second tree is 1, as
   * the manager orders an object's trees). Call MarkSharedObjects after the last one. */
  LN_RuntimeTree &AddTreeOf(const std::shared_ptr<LN_Program> &program,
                            const size_t object_index,
                            const uint32_t scene_index,
                            const uint32_t tree_index)
  {
    auto tree = std::make_unique<LN_RuntimeTree>(
        program, &m_objects[object_index].object, scene_index, tree_index);
    tree->SetStepSpread(uint32_t(m_trees.size()));
    tree->GetBatchNote().sole_tree = true;
    LN_RuntimeTree &added = *tree;
    m_trees.push_back(std::move(tree));
    return added;
  }
  /** Marks the trees of objects that have several as no object's only tree (as the manager's
   * index does). */
  void MarkSharedObjects()
  {
    std::unordered_map<const KX_GameObject *, size_t> trees_of;
    for (const std::unique_ptr<LN_RuntimeTree> &tree : m_trees) {
      trees_of[tree->GetGameObject()]++;
    }
    for (std::unique_ptr<LN_RuntimeTree> &tree : m_trees) {
      tree->GetBatchNote().sole_tree = trees_of[tree->GetGameObject()] == 1;
    }
  }
  /** Which objects are menu copies for the columns' plan (the manager's scene says it in the
   * game; names of a copy's trees find its counterparts through SetOverlayCounterpart). */
  void SetOverlayCopies(std::function<bool(KX_GameObject *)> is_copy)
  {
    m_columns.SetOverlayTest(std::move(is_copy));
  }

  /** Gives each of the first `owners` trees' objects a second tree of `program`, right after its
   * own (the same scene object, applied tree index 1, as the manager orders an object's trees):
   * objects with two trees (plan C1-9's groups). */
  void AddSecondTrees(const std::shared_ptr<LN_Program> &program, const size_t owners)
  {
    std::vector<std::unique_ptr<LN_RuntimeTree>> trees;
    for (size_t index = 0; index < m_trees.size(); index++) {
      LN_RuntimeTree *first = m_trees[index].get();
      trees.push_back(std::move(m_trees[index]));
      if (index < owners) {
        first->GetBatchNote().sole_tree = false;
        auto second = std::make_unique<LN_RuntimeTree>(
            program, first->GetGameObject(), first->GetSceneObjectIndex(), 1);
        second->SetStepSpread(uint32_t(trees.size()));
        second->GetBatchNote().sole_tree = false;
        trees.push_back(std::move(second));
      }
    }
    m_trees = std::move(trees);
  }

  /** Binds every tree's ids in one registry, as the manager does (plan C1-10): game property
   * names get ids, so two trees' writes of one property share a merge key in the flush (without,
   * each tree's write is keyed by its tree). Call after the trees are added. */
  void BindIds()
  {
    for (std::unique_ptr<LN_RuntimeTree> &tree : m_trees) {
      tree->BindDenseIds(m_ids);
    }
  }

  /** A menu copy rule: the object a tree finds by name instead of `object` (Publish keeps it). */
  void SetOverlayCounterpart(
      std::function<KX_GameObject *(KX_GameObject *object, KX_GameObject *viewer)> rule)
  {
    m_access.overlay_counterpart = std::move(rule);
    Publish();
  }

  KX_GameObject &Object(const size_t index)
  {
    return m_objects[index].object;
  }
  size_t ObjectCount() const
  {
    return m_objects.size();
  }
  /** Makes object `child` a child of object `parent` on the scene graph (a normal parent
   * relation): its local pose stays as it was set, its world pose follows the parent's. */
  void Parent(const size_t child, const size_t parent)
  {
    BatchObject &child_object = m_objects[child];
    BatchObject &parent_object = m_objects[parent];
    child_object.node.SetParentRelation(new KX_NormalParentRelation());
    parent_object.node.AddChild(&child_object.node);
    parent_object.object.NodeUpdateGS(0.0);
  }
  LN_RuntimeTree &Tree(const size_t index)
  {
    return *m_trees[index];
  }
  size_t TreeCount() const
  {
    return m_trees.size();
  }

  void SetFloat(KX_GameObject &object, const char *name, const float value)
  {
    object.SetProperty(name, new EXP_FloatValue(value, name));
  }
  void SetInt(KX_GameObject &object, const char *name, const int value)
  {
    object.SetProperty(name, new EXP_IntValue(value, name));
  }
  void SetBool(KX_GameObject &object, const char *name, const bool value)
  {
    object.SetProperty(name, new EXP_BoolValue(value, name));
  }
  void SetString(KX_GameObject &object, const char *name, const char *value)
  {
    object.SetProperty(name, new EXP_StringValue(value, name));
  }
  /** The property as a float; `fallback` when the object has none. */
  float Float(KX_GameObject &object, const char *name, const float fallback = -12345.0f)
  {
    EXP_Value *property = object.GetProperty(name);
    return property != nullptr ? float(property->GetNumber()) : fallback;
  }
  std::string Text(KX_GameObject &object, const char *name)
  {
    EXP_Value *property = object.GetProperty(name);
    return property != nullptr ? property->GetText() : std::string("<none>");
  }
  bool Has(KX_GameObject &object, const char *name)
  {
    return object.GetProperty(name) != nullptr;
  }

  /** One tick of `event` in the manager's order; `forced` runs after the trees recorded (the Run
   * Logic Tree passes). The commands apply at the end, as the flush does. */
  void Tick(const LN_Event event,
            const std::function<void(LN_CommandBuffer &, const LN_TickContext &)> &forced = {})
  {
    /* The scene seam is one for the process: with two rigs alive, each ticks in its own world
     * (PublishEachTick; off, a test may point the seam elsewhere before a tick). */
    if (m_publish_each_tick) {
      LN_SetSceneAccessForTests(&m_access);
    }
    LN_TickContext context = MakeContext(event);
    const bool fixed = event == LN_Event::OnFixedUpdate;
    std::vector<LN_RuntimeTree *> runnable;
    for (std::unique_ptr<LN_RuntimeTree> &tree : m_trees) {
      if (fixed && tree->HasStepGates()) {
        tree->PrepareStepGates(context.phase_tick_index, kStep, kStep);
      }
      if (tree->CanTick(event)) {
        runnable.push_back(tree.get());
      }
    }
    LN_TickReadContext read;
    read.has_timing = true;
    read.frame_delta = float(kStep);
    read.elapsed_time = float(m_time);
    read.fps = 60.0f;
    read.tick_index = context.tick_index;
    read.has_gravity = true;
    const auto time_since = [](const std::chrono::steady_clock::time_point from) {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from)
          .count();
    };
    const auto capture_start = std::chrono::steady_clock::now();
    /* Plan C1 prototype: as the manager plans its columns (LN_Columns.h). */
    /* A Run Logic Tree pass here may run any tree: none is a lane then. */
    std::unordered_set<LN_RuntimeTree *> all_forced;
    if (forced) {
      all_forced.insert(runnable.begin(), runnable.end());
    }
    const auto plan_start = std::chrono::steady_clock::now();
    m_columns.SetPlanHelpers(m_column_pool.GetHelperCount());
    const bool columns = m_use_columns && m_columns.Plan(runnable.data(),
                                                         runnable.size(),
                                                         event,
                                                         m_buffer,
                                                         forced ? &all_forced : nullptr,
                                                         &read);
    if (m_use_columns) {
      m_times.plan_ms += time_since(plan_start);
    }
    LN_JobPool *column_pool = m_column_pool.GetHelperCount() > 0 ? &m_column_pool : nullptr;
    std::vector<LN_RuntimeTree *> parallel_captures;
    for (size_t index = 0; index < runnable.size(); index++) {
      LN_RuntimeTree *tree = runnable[index];
      /* As the manager: a lane's tree captures only its properties (a bridged one all of it). */
      if (columns && tree->IsColumnLane()) {
        continue;
      }
      if (tree->ShouldCaptureSnapshot()) {
        if (m_capture_pool.GetHelperCount() > 0 && tree->PrepareParallelCapture(&read)) {
          parallel_captures.push_back(tree);
          continue;
        }
        tree->CaptureSnapshot(&read, float(kStep), context.tick_index, false);
      }
    }
    if (!parallel_captures.empty()) {
      /* As the manager's capture loop does: chunks of consecutive trees on the pool. */
      const size_t count = parallel_captures.size();
      const size_t chunks = std::max<size_t>(1, std::min<size_t>(count, 8));
      m_capture_pool.Run(chunks, [&](const size_t chunk, uint32_t /*thread*/) {
        for (size_t index = count * chunk / chunks; index < count * (chunk + 1) / chunks; index++) {
          parallel_captures[index]->CaptureSnapshot(&read, float(kStep), context.tick_index, false);
        }
      });
      m_parallel_captured += count;
    }
    if (columns) {
      m_columns.CaptureLanes(context.tick_index, column_pool);
    }
    for (size_t index = 0; index < runnable.size(); index++) {
      if (!(columns && m_columns.IsLane(index))) {
        runnable[index]->WarmQueryCache(context);
      }
    }
    m_times.capture_ms += time_since(capture_start);
    const auto record_start = std::chrono::steady_clock::now();
    m_buffer.Clear();
    m_buffer.BeginRecording();
    /* Plan C2 P6: a rig with columns records every tree outside the runs as the manager does,
     * through the column system's pass (unless direct programs are off); a rig without columns
     * is the old system's reference. */
    const bool column_passes = m_use_columns &&
                               LN_GetColumnMode() != LN_ColumnMode::Off &&
                               LN_GetColumnDirectMode() != LN_ColumnDirectMode::Off;
    const uint64_t passes_before = LN_ColumnDirectLaneCount();
    uint64_t column_pass_count = 0; /* (counted in bulk, as the manager counts) */
    if (m_recorder_override) {
      m_recorder_override(m_buffer, context, runnable);
    }
    else if (columns) {
      /* The manager's order: runs of lanes in columns, the other trees one after the other. */
      size_t index = 0;
      while (index < runnable.size()) {
        if (m_columns.IsLane(index)) {
          index = m_columns.RunLanes(index, m_buffer, context, column_pool);
          continue;
        }
        LN_RuntimeProfileCounters counters;
        /* Plan C2 P6: as the manager, the column system's pass (unless direct programs are off). */
        if (column_passes) {
          runnable[index]->ColumnExecuteReady(
              m_buffer, context, m_mode.profiled ? &counters : nullptr);
          column_pass_count++;
        }
        else {
          runnable[index]->ExecuteReady(m_buffer, context, m_mode.profiled ? &counters : nullptr);
        }
        index++;
      }
    }
    else if (m_batch_chunks > 0 && runnable.size() >= 2) {
      /* As a tree batch (benchmarks): the trees record in chunks on the pool. */
      m_recorder.SetColumnPasses(column_passes);
      m_recorder.Record(m_pool,
                        m_buffer,
                        runnable.data(),
                        runnable.size(),
                        context,
                        nullptr,
                        nullptr,
                        std::min<size_t>(m_batch_chunks, runnable.size()));
    }
    else {
      for (LN_RuntimeTree *tree : runnable) {
        LN_RuntimeProfileCounters counters;
        if (column_passes) {
          tree->ColumnExecuteReady(m_buffer, context, m_mode.profiled ? &counters : nullptr);
          column_pass_count++;
        }
        else {
          tree->ExecuteReady(m_buffer, context, m_mode.profiled ? &counters : nullptr);
        }
      }
    }
    LN_AddColumnPasses(column_pass_count);
    if (forced) {
      forced(m_buffer, context);
    }
    m_buffer.EndRecording();
    m_times.record_ms += time_since(record_start);
    if (m_dump) {
      m_last_dump = m_buffer.DumpRecordedForTests(LN_CommandBuffer::DumpOrder::Record);
    }
    /* What the Print node writes appears when the commands apply. */
    if (m_flush) {
      const auto flush_start = std::chrono::steady_clock::now();
      if (m_capture_prints) {
        testing::internal::CaptureStdout();
      }
      /* Forces and torques have no physics to act on here: the rig notes them (Forces). */
      LN_SetForceObserverForTests(&SemRig::NoteForce, this);
      m_buffer.Flush(); /* the column runs' writes too, at their sort keys */
      LN_SetForceObserverForTests(nullptr, nullptr);
      if (m_capture_prints) {
        m_printed += testing::internal::GetCapturedStdout();
      }
      m_times.flush_ms += time_since(flush_start);
    }
    m_last_column_passes = size_t(LN_ColumnDirectLaneCount() - passes_before);
    if (columns) {
      m_last_column_lanes = m_columns.LaneCount();
      m_columns.EndTick();
    }
    else {
      m_last_column_lanes = 0;
      m_last_column_refusal = m_use_columns ? m_columns.LastRefusal() : std::string();
    }
    m_last_context = context;
    m_time += kStep;
  }
  /** One Frame Update tick and then one Logic + Physics step. */
  void Frame()
  {
    Tick(LN_Event::OnUpdate);
    Tick(LN_Event::OnFixedUpdate);
  }
  void Update()
  {
    Tick(LN_Event::OnUpdate);
  }
  void Step()
  {
    Tick(LN_Event::OnFixedUpdate);
  }

  /** A forced pass (Run Logic Tree) of tree `index` inside the tick that is running. */
  void Force(const size_t index, LN_CommandBuffer &buffer, const LN_TickContext &context)
  {
    LN_RuntimeProfileCounters counters;
    LN_TickContext pass = context;
    pass.event = LN_Event::OnFixedUpdate;
    /* Plan C2 P6: as the manager, the column system's forced pass in a rig with columns. */
    if (m_use_columns && LN_ColumnPassesOn()) {
      m_trees[index]->ColumnExecuteForcedUpdate(
          buffer, pass, m_mode.profiled ? &counters : nullptr);
      LN_AddColumnPasses(1);
    }
    else {
      m_trees[index]->ExecuteForcedUpdate(buffer, pass, m_mode.profiled ? &counters : nullptr);
    }
  }

  /** What the Print node printed so far (until now). */
  const std::string &Printed() const
  {
    return m_printed;
  }
  /** Every force and torque the flushes applied, in order (plan C2 P5: the rig has no physics). */
  const std::string &Forces() const
  {
    return m_forces;
  }
  static void NoteForce(void *user,
                        const KX_GameObject *object,
                        const MT_Vector3 &vector,
                        const bool local,
                        const bool torque)
  {
    SemRig &rig = *static_cast<SemRig *>(user);
    rig.m_forces += (object != nullptr ? const_cast<KX_GameObject *>(object)->GetName() :
                                         std::string("-")) +
                    (torque ? " torque " : " force ") + LN_DumpFloatBits(vector.x()) + "," +
                    LN_DumpFloatBits(vector.y()) + "," + LN_DumpFloatBits(vector.z()) +
                    (local ? " local\n" : "\n");
  }

  /** The scene seam the rig publishes (tests of the object lookups change its lists by hand;
   * NoteObjectListChanged after a change of the lists). */
  LN_SceneAccessForTests &Access()
  {
    return m_access;
  }
  void NoteObjectListChanged()
  {
    m_access.object_list_generation++;
  }

  /** The milliseconds the ticks spent in each phase since the last ResetTimes (benchmarks). */
  struct PhaseTimes {
    double capture_ms = 0.0, record_ms = 0.0, flush_ms = 0.0;
    /** The columns' Plan alone (counted in capture_ms too). */
    double plan_ms = 0.0;
  };
  const PhaseTimes &Times() const
  {
    return m_times;
  }
  void ResetTimes()
  {
    m_times = PhaseTimes();
  }
  /** Ticks record as a tree batch of `chunks` chunks on a pool of `helpers` helper threads
   * (0 chunks: one tree after the other, as before). The trees must be batch safe. */
  void SetBatchRecording(const uint32_t chunks, const uint32_t helpers)
  {
    m_batch_chunks = chunks;
    m_pool.SetHelperCount(helpers);
  }
  /** Plan C1 prototype: ticks plan and run columns as the manager does (LN_Columns.h), runs of at
   * least `min_run` trees, chunks on a pool of `helpers` helper threads (0: this thread). */
  void SetColumns(const bool on, const uint32_t helpers = 0, const size_t min_run = 1)
  {
    m_use_columns = on;
    m_column_pool.SetHelperCount(helpers);
    /* 0 keeps the runner's own shortest run (1, with the size floors of small programs). */
    if (min_run != 0) {
      m_columns.SetMinRunLanes(min_run);
    }
  }
  /** Each tick points the process's scene seam at this rig first (two rigs alive side by side:
   * by-name lookups find this rig's objects). */
  void PublishEachTick(const bool on)
  {
    m_publish_each_tick = on;
  }
  /** The fewest lanes a wave of short runs takes (0: none; plan C2 P1). */
  void SetMinWaveLanes(const size_t lanes)
  {
    m_columns.SetMinWaveLanes(lanes);
  }
  /** Lowers every tree's program to columns now, as the manager does when a tree registers
   * (plan C2 P1). */
  void PrepareColumns()
  {
    for (std::unique_ptr<LN_RuntimeTree> &tree : m_trees) {
      m_columns.PrepareProgram(*tree->GetProgram(), tree.get());
    }
  }
  /** How many trees the last tick ran in columns, and why it ran none (with columns on). */
  size_t LastColumnLanes() const
  {
    return m_last_column_lanes;
  }
  /** Plan C2 P6: the trees the last tick ran through the column system's pass outside the runs
   * (ColumnExecuteReady, lanes given back included). */
  size_t LastColumnPasses() const
  {
    return m_last_column_passes;
  }
  const std::string &LastColumnRefusal() const
  {
    return m_last_column_refusal;
  }
  /** Snapshots are captured on a pool of `helpers` helper threads where a tree allows it (the
   * manager's capture loop does the same); 0: one after the other. */
  void SetParallelCapture(const uint32_t helpers)
  {
    m_capture_pool.SetHelperCount(helpers);
  }
  /** How many snapshots were captured on the pool so far. */
  size_t ParallelCaptured() const
  {
    return m_parallel_captured;
  }
  /** Benchmarks of hand-written recorders: `recorder` records the runnable trees instead of the
   * trees themselves (snapshots, the dump and the flush stay the rig's). Empty: the trees. */
  using Recorder = std::function<void(
      LN_CommandBuffer &, const LN_TickContext &, const std::vector<LN_RuntimeTree *> &)>;
  void SetRecorder(Recorder recorder)
  {
    m_recorder_override = std::move(recorder);
  }
  /** Off: ticks do not dump what they recorded (benchmarks; LastRecorded() stays as it was). */
  void SetDump(const bool dump)
  {
    m_dump = dump;
  }
  /** Off: a flush does not capture the standard output (Printed() stays as it was). */
  void SetCapturePrints(const bool capture)
  {
    m_capture_prints = capture;
  }

  /** Off: ticks record but do not apply their commands (nodes that need a scene to apply them:
   * the test reads LastRecorded() instead). */
  void SetFlush(const bool flush)
  {
    m_flush = flush;
  }

  /** The commands the last tick recorded (strict dump), before they were applied. */
  const std::string &LastRecorded() const
  {
    return m_last_dump;
  }
  const LN_TickContext &LastContext() const
  {
    return m_last_context;
  }
  LN_CommandBuffer &Buffer()
  {
    return m_buffer;
  }
  /** The context of the next tick of `event` (counters advance). */
  LN_TickContext MakeContext(const LN_Event event)
  {
    LN_TickContext context;
    context.event = event;
    context.current_time = m_time;
    context.delta_time = kStep;
    context.unscaled_dt = kStep;
    context.use_fixed_timestep = true;
    context.tick_index = m_tick_index++;
    context.phase_tick_index = event == LN_Event::OnUpdate ? m_update_index++ : m_fixed_index++;
    return context;
  }

  static constexpr double kStep = 1.0 / 60.0;

 private:
  void Publish()
  {
    m_access.objects.clear();
    for (BatchObject &object : m_objects) {
      m_access.objects.push_back(&object.object);
    }
    m_access.object_list_generation++;
    LN_SetSceneAccessForTests(&m_access);
    m_published = true;
  }

  std::shared_ptr<LN_Program> m_program;
  SemMode m_mode;
  std::deque<BatchObject> m_objects;
  /** BindIds' registry (before the trees: it outlives them). */
  LN_DenseIdRegistry m_ids;
  std::vector<std::unique_ptr<LN_RuntimeTree>> m_trees;
  LN_SceneAccessForTests m_access;
  LN_CommandBuffer m_buffer;
  LN_TickContext m_last_context;
  std::string m_printed;
  std::string m_forces;
  std::string m_last_dump;
  bool m_published = false;
  bool m_flush = true;
  LN_JobPool m_capture_pool;
  size_t m_parallel_captured = 0;
  uint32_t m_batch_chunks = 0;
  LN_JobPool m_pool;
  LN_TreeBatchRecorder m_recorder;
  bool m_dump = true;
  bool m_capture_prints = true;
  Recorder m_recorder_override;
  bool m_use_columns = false;
  bool m_publish_each_tick = false;
  LN_ColumnRunner m_columns;
  LN_JobPool m_column_pool;
  size_t m_last_column_lanes = 0;
  size_t m_last_column_passes = 0;
  std::string m_last_column_refusal;
  PhaseTimes m_times;
  double m_time = 0.0;
  uint64_t m_tick_index = 0, m_update_index = 0, m_fixed_index = 0;
};

}  // namespace ln_test::sem

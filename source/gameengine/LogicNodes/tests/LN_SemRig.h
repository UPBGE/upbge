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
#include <vector>

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
  SemRig(const std::shared_ptr<LN_Program> &program,
         const SemMode &mode = kProduction,
         const uint32_t owners = 1)
      : m_program(program), m_mode(mode)
  {
    LN_RuntimeTree::SetMemoForTests(mode.memo ? 1 : 0);
    /* As the scene's buffer: its objects are real (plan phase A33's pending move marks). */
    m_buffer.SetMarksPendingMoves(true);
    for (uint32_t index = 0; index < owners; index++) {
      AddObject("Obj" + std::to_string(index), MT_Vector3(0.0f, 0.0f, 0.0f));
    }
    for (uint32_t index = 0; index < owners; index++) {
      m_trees.push_back(std::make_unique<LN_RuntimeTree>(program, &m_objects[index].object, index, 0));
      m_trees.back()->SetStepSpread(index);
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
    std::vector<LN_RuntimeTree *> parallel_captures;
    for (LN_RuntimeTree *tree : runnable) {
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
    for (LN_RuntimeTree *tree : runnable) {
      tree->WarmQueryCache(context);
    }
    m_times.capture_ms += time_since(capture_start);
    const auto record_start = std::chrono::steady_clock::now();
    m_buffer.Clear();
    m_buffer.BeginRecording();
    if (m_recorder_override) {
      m_recorder_override(m_buffer, context, runnable);
    }
    else if (m_batch_chunks > 0 && runnable.size() >= 2) {
      /* As a tree batch (benchmarks of blocks): the trees record in chunks on the pool. */
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
        tree->ExecuteReady(m_buffer, context, m_mode.profiled ? &counters : nullptr);
      }
    }
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
      m_buffer.Flush();
      if (m_capture_prints) {
        m_printed += testing::internal::GetCapturedStdout();
      }
      m_times.flush_ms += time_since(flush_start);
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
    m_trees[index]->ExecuteForcedUpdate(buffer, pass, m_mode.profiled ? &counters : nullptr);
  }

  /** What the Print node printed so far (until now). */
  const std::string &Printed() const
  {
    return m_printed;
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
  std::vector<std::unique_ptr<LN_RuntimeTree>> m_trees;
  LN_SceneAccessForTests m_access;
  LN_CommandBuffer m_buffer;
  LN_TickContext m_last_context;
  std::string m_printed;
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
  PhaseTimes m_times;
  double m_time = 0.0;
  uint64_t m_tick_index = 0, m_update_index = 0, m_fixed_index = 0;
};

}  // namespace ln_test::sem
